#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <time.h>
#include <stdint.h>

#include "Timings.h"
#include "Log.h"

/**
 * Scheduler centralizzato di tutti i flussi temporizzati: fetch di rete,
 * refresh del pannello, poll del sensore.
 *
 * Possiede il timing per intero. Intervallo, istante dell'ultimo slot,
 * contatore dei fallimenti e politica di ritento vivono qui, nella coppia
 * tabella + stato; i moduli espongono solo un'esecuzione che fa I/O e dice
 * com'e' andata. L'intervallo non e' copiato nella tabella ma indirizzato con
 * un puntatore a membro di Timings::Valori e riletto a ogni valutazione,
 * quindi una modifica dalla pagina di configurazione ha effetto dal giro
 * successivo senza riavvio.
 *
 * Non possiede la radio: la accende e la spegne solo attraverso le due
 * funzioni che gli vengono passate in begin(), e durante la finestra di
 * manutenzione non la tocca affatto, limitandosi a leggerne lo stato.
 *
 * L'esito di ogni task e' indipendente dagli altri. Nessun percorso del
 * runner interrompe il ciclo: un fallimento aggiorna lo stato del solo task
 * che l'ha prodotto e viene loggato, e la sequenza prosegue. I moduli
 * conservano la cache precedente quando un fetch fallisce, quindi la UI mostra
 * i dati vecchi o i placeholder "--", mai dati incoerenti.
 */
namespace Scheduler
{
  /**
   * Esito di un task nel giro corrente.
   *
   *   OK            eseguito, con dati nuovi da mostrare.
   *   OK_INVARIATO  eseguito, ma i dati sono identici a quelli in cache:
   *                 nessun motivo di pagare un refresh del pannello.
   *   FALLITO       guasto imputabile al task: consuma un tentativo.
   *   SALTATO       niente da fare, o guasto a monte (radio giu'): lo stato
   *                 resta intatto e il tentativo non viene speso.
   */
  enum class Esito : uint8_t { OK, OK_INVARIATO, FALLITO, SALTATO };

  /** Come si apre lo slot di un task. */
  enum class Cadenza : uint8_t
  {
    PERIODICA,   // ogni intervallo dall'ultimo slot consumato
    GIORNALIERA  // una volta al giorno, dall'ora locale indicata in poi
  };

  /** Di chi e' la radio durante il giro. */
  enum class Radio : uint8_t
  {
    PROPRIA,  // la accende e la spegne lo scheduler
    ESTERNA   // e' della finestra di manutenzione: si usa se c'e', non si tocca
  };

  /** Scadenza non calcolabile o non applicabile. */
  inline constexpr uint32_t MAI = UINT32_MAX;

  /**
   * Descrittore di un task. La parte immutabile e' la configurazione, `stato`
   * e' la sola parte che il runner modifica.
   *
   * Gli hook opzionali (nullptr = assente):
   *   pronto()          precondizione non temporale; falso esclude il task
   *                     anche dal calcolo del prossimo risveglio.
   *   datiIncompleti()  vero quando l'esecuzione e' riuscita ma i dati non
   *                     bastano ancora: lo slot non si chiude e si ritenta.
   *   prima()           eseguito a inizio giro per TUTTI i task dovuti, prima
   *                     di qualunque esegui(): serve a chi deve avviare
   *                     qualcosa e lasciarlo maturare mentre gli altri girano.
   *   dopo(Esito)       notifica al resto del firmware, tipicamente la
   *                     richiesta di ridisegno su esito positivo.
   *   msAllaScadenzaCustom()  per i task la cui cadenza la decide altri (il
   *                     sensore la decide la libreria BSEC).
   */
  struct Task
  {
    const char* tag;
    Cadenza     cadenza;
    bool        richiedeRadio;
    bool        ignoraFascia;                    // vero solo per chi deve girare anche di notte

    uint16_t Timings::Valori::*intervalloMin;    // PERIODICA; per il display e' il rate limit
    uint16_t Timings::Valori::*oraLocale;        // GIORNALIERA

    uint32_t intervalloRitentoMs;
    uint8_t  maxTentativi;                       // 0 = nessun ritento: un tentativo per intervallo, comunque vada

    Esito    (*esegui)();
    bool     (*pronto)();
    bool     (*datiIncompleti)();
    void     (*prima)();
    void     (*dopo)(Esito);
    uint32_t (*msAllaScadenzaCustom)();

    struct Stato
    {
      bool     maiEseguito        = true;   // esplicito: nessuna sentinella su ultimoMs
      uint32_t ultimoMs           = 0;      // istante dell'ultimo slot consumato
      uint32_t ultimoOkMs         = 0;
      bool     inRitento          = false;
      uint32_t prossimoTentativoMs = 0;
      uint8_t  tentativi          = 0;      // falliti consecutivi dentro lo slot corrente
      int16_t  ultimoGiorno       = -1;     // tm_yday dell'ultimo slot giornaliero
      uint32_t esiti = 0, ok = 0, falliti = 0;
    } stato;
  };

  namespace detail
  {
    inline Task*    tabella   = nullptr;
    inline uint8_t  nTask     = 0;
    inline bool   (*radioOn)()  = nullptr;
    inline void   (*radioOff)() = nullptr;

    inline uint32_t bootMs           = 0;
    inline uint32_t radioProssimoMs  = 0;   // backoff dopo un tentativo di connessione fallito
    inline bool     radioInBackoff   = false;
    inline bool     eseguitoNelGiro  = false;
    inline bool     esitoReteVisto   = false;
    inline uint32_t giri             = 0;

    /** Confronto rollover-safe, come Weather::detail::elapsed. */
    inline bool trascorso(uint32_t ora, uint32_t rif, uint32_t soglia)
    {
      return (int32_t)(ora - rif) >= (int32_t)soglia;
    }

    /** Millisecondi mancanti a `quando`, 0 se gia' passato. */
    inline uint32_t resto(uint32_t ora, uint32_t quando)
    {
      const int32_t d = (int32_t)(quando - ora);
      return d <= 0 ? 0 : (uint32_t)d;
    }

  }

  /**
   * Vero se l'orologio di sistema e' stato sincronizzato. Prima di SNTP time()
   * restituisce l'uptime contato dal 1970, che dopo 27,7 h di accensione
   * diventa indistinguibile da un'ora reale.
   */
  inline bool orologioValido() { return time(nullptr) >= TIME_VALID_EPOCH_MIN; }

  /**
   * Vero dentro la fascia oraria in cui la radio puo' accendersi.
   * Fail-open a orologio non sincronizzato: senza questa eccezione il primo
   * SNTP non potrebbe mai avvenire, perche' richiede la radio.
   */
  inline bool inFascia()
  {
    if (!orologioValido()) return true;
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    const auto& v = Timings::get();
    return t.tm_hour >= v.wifiOraInizio && t.tm_hour <= v.wifiOraFine;
  }

  /**
   * Millisecondi fino alla prossima occorrenza locale di `ora`:00.
   * mktime() con tm_isdst = -1 normalizza il giorno e assorbe il cambio
   * dell'ora legale. Presuppone l'orologio sincronizzato.
   */
  inline uint32_t msFinoAllOra(uint8_t ora, bool domani)
  {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    struct tm bersaglio = t;
    bersaglio.tm_hour = ora;
    bersaglio.tm_min = 0;
    bersaglio.tm_sec = 0;
    bersaglio.tm_isdst = -1;
    if (domani || ora <= t.tm_hour) bersaglio.tm_mday += 1;
    const time_t quando = mktime(&bersaglio);
    if (quando == (time_t)-1 || quando <= now) return 0;
    const double d = difftime(quando, now) * 1000.0;
    return d >= (double)MAI ? MAI - 1 : (uint32_t)d;
  }

  /**
   * Millisecondi alla scadenza del task, senza tenere conto dei vincoli della
   * radio. 0 significa dovuto adesso, MAI non determinabile.
   */
  inline uint32_t msAllaScadenza(const Task& t, uint32_t ora, const struct tm* oggi)
  {
    if (t.pronto && !t.pronto()) return MAI;
    if (t.stato.inRitento) return detail::resto(ora, t.stato.prossimoTentativoMs);
    if (t.msAllaScadenzaCustom) return t.msAllaScadenzaCustom();
    if (t.stato.maiEseguito) return 0;

    if (t.cadenza == Cadenza::PERIODICA)
      return detail::resto(ora, t.stato.ultimoMs + Timings::ms(Timings::get().*t.intervalloMin));

    // GIORNALIERA: senza orologio non c'e' modo di sapere che ore sono.
    if (!oggi) return MAI;
    const uint8_t bersaglio = (uint8_t)(Timings::get().*t.oraLocale);
    // Un secondo slot troppo ravvicinato sarebbe un doppione: succede quando
    // l'orologio diventa valido a meta' giornata e l'ora target risulta
    // gia' passata.
    if (!detail::trascorso(ora, t.stato.ultimoMs, GIORNALIERA_DISTANZA_MIN_MS))
      return msFinoAllOra(bersaglio, true);
    // '>=' e non '==': se all'ora prevista la radio era giu', si recupera
    // alla prima occasione utile della stessa giornata.
    if (oggi->tm_yday != t.stato.ultimoGiorno && oggi->tm_hour >= bersaglio) return 0;
    return msFinoAllOra(bersaglio, oggi->tm_yday == t.stato.ultimoGiorno);
  }

  /** Scadenza effettiva: aggiunge i vincoli che riguardano la radio. */
  inline uint32_t msAllaScadenzaEffettiva(const Task& t, uint32_t ora, const struct tm* oggi)
  {
    uint32_t s = msAllaScadenza(t, ora, oggi);
    if (s == MAI || !t.richiedeRadio) return s;

    if (detail::radioInBackoff)
    {
      const uint32_t r = detail::resto(ora, detail::radioProssimoMs);
      if (r == 0) detail::radioInBackoff = false;
      else if (r > s) s = r;
    }
    if (!t.ignoraFascia && !inFascia())
    {
      if (!oggi) return MAI;
      const uint32_t r = msFinoAllOra((uint8_t)Timings::get().wifiOraInizio, false);
      if (r > s) s = r;
    }
    return s;
  }

  inline bool dovuto(const Task& t, uint32_t ora, const struct tm* oggi, uint32_t anticipoMs)
  {
    const uint32_t s = msAllaScadenzaEffettiva(t, ora, oggi);
    return s != MAI && s <= anticipoMs;
  }

  namespace detail
  {
    /**
     * Chiude lo slot corrente: da qui il task non ritenta prima della
     * scadenza successiva. Il giorno di un task giornaliero si registra solo
     * con l'orologio valido, altrimenti si memorizzerebbe il 1 gennaio 1970
     * bruciando uno slot futuro.
     */
    inline void consumaSlot(Task& t, uint32_t ora, const struct tm* oggi)
    {
      t.stato.ultimoMs = ora;
      t.stato.maiEseguito = false;
      t.stato.inRitento = false;
      t.stato.tentativi = 0;
      if (t.cadenza == Cadenza::GIORNALIERA && oggi) t.stato.ultimoGiorno = (int16_t)oggi->tm_yday;
    }

    /** Descrizione leggibile della prossima scadenza, per i log. */
    inline const char* descriviScadenza(const Task& t, uint32_t ora, const struct tm* oggi)
    {
      static char buf[40];
      const uint32_t s = msAllaScadenza(t, ora, oggi);
      if (s == MAI) snprintf(buf, sizeof buf, "sospeso");
      else if (t.cadenza == Cadenza::GIORNALIERA)
        snprintf(buf, sizeof buf, "fra %lu h", (unsigned long)(s / 3600000UL));
      else snprintf(buf, sizeof buf, "fra %lu s", (unsigned long)(s / 1000UL));
      return buf;
    }
  }

  /**
   * Registra l'esito di un task e decide se lo slot si chiude o si ritenta.
   * Un'esecuzione riuscita ma con dati ancora incompleti conta come
   * fallimento ai fini dello slot, pur sporcando il frame: e' il caso del
   * meteo che ha la corrente ma non le previsioni.
   */
  inline void registraEsito(Task& t, Esito e, uint32_t ora, uint32_t durataMs, const struct tm* oggi)
  {
    if (e == Esito::SALTATO)
    {
      LOGV("sched", "%s: saltato", t.tag);
      if (t.dopo) t.dopo(e);
      return;
    }

    detail::eseguitoNelGiro = true;
    if (t.richiedeRadio) detail::esitoReteVisto = true;
    t.stato.esiti++;

    const bool incompleto = (e != Esito::FALLITO) && t.datiIncompleti && t.datiIncompleti();
    const bool fallitoAiFini = (e == Esito::FALLITO) || incompleto;

    if (e == Esito::OK) { t.stato.ok++; t.stato.ultimoOkMs = ora; }
    if (e == Esito::FALLITO) t.stato.falliti++;

    // Il ridisegno non e' affare dello scheduler: chi porta dati nuovi lo
    // chiede nel proprio dopo(), che ne conosce la regola (di norma "su OK",
    // ma per esempio il display non si auto-sporca).
    if (t.dopo) t.dopo(e);

    if (fallitoAiFini && t.maxTentativi > 0 && t.stato.tentativi + 1 < t.maxTentativi)
    {
      t.stato.tentativi++;
      t.stato.inRitento = true;
      t.stato.prossimoTentativoMs = ora + t.intervalloRitentoMs;
      LOG("sched", "%s: %s in %lu ms (tentativo %u/%u), ritento fra %lu s",
          t.tag, incompleto ? "dati incompleti" : "fallito",
          (unsigned long)durataMs, (unsigned)t.stato.tentativi, (unsigned)t.maxTentativi,
          (unsigned long)(t.intervalloRitentoMs / 1000UL));
      return;
    }

    const bool sogliaRaggiunta = fallitoAiFini && t.maxTentativi > 0;
    detail::consumaSlot(t, ora, oggi);
    if (fallitoAiFini)
      LOG("sched", "%s: %s in %lu ms%s, prossimo %s", t.tag,
          incompleto ? "dati incompleti" : "fallito", (unsigned long)durataMs,
          sogliaRaggiunta ? ", soglia raggiunta" : "",
          detail::descriviScadenza(t, ora, oggi));
    else
      LOG("sched", "%s: %s in %lu ms, prossimo %s", t.tag,
          e == Esito::OK_INVARIATO ? "ok (dati invariati)" : "ok",
          (unsigned long)durataMs, detail::descriviScadenza(t, ora, oggi));
  }

  namespace detail
  {
    /** Esegue i task dovuti di una fase: prima tutti i prima(), poi gli esegui(). */
    inline void eseguiFase(bool conRadio, uint32_t anticipoMs, const struct tm* oggi)
    {
      const uint32_t ora = millis();
      for (uint8_t i = 0; i < nTask; ++i)
        if (tabella[i].richiedeRadio == conRadio && dovuto(tabella[i], ora, oggi, anticipoMs) &&
            tabella[i].prima)
          tabella[i].prima();

      for (uint8_t i = 0; i < nTask; ++i)
      {
        Task& t = tabella[i];
        if (t.richiedeRadio != conRadio) continue;
        if (!dovuto(t, millis(), oggi, anticipoMs)) continue;

        // Radio caduta a giro iniziato: guasto a monte, non del task. Lo slot
        // resta intatto cosi' il tentativo viene speso davvero al primo giro
        // con la radio su.
        if (t.richiedeRadio && WiFi.status() != WL_CONNECTED)
        {
          LOG("sched", "%s: saltato, radio non connessa (slot intatto)", t.tag);
          registraEsito(t, Esito::SALTATO, millis(), 0, oggi);
          continue;
        }

        const uint32_t t0 = millis();
        const Esito e = t.esegui();
        const uint32_t fine = millis();
        registraEsito(t, e, fine, fine - t0, oggi);
      }
    }
  }

  /**
   * Un giro completo. In modalita' PROPRIA accende la radio solo se almeno un
   * task di rete e' scaduto davvero (nessun anticipo), poi esegue con
   * l'anticipo del coalescing: cosi' la finestra si apre quando serve, ma una
   * volta aperta ci si infila anche chi scadrebbe fra poco.
   */
  inline void giro(Radio modalita)
  {
    using namespace detail;
    eseguitoNelGiro = false;

    struct tm tmOggi;
    const struct tm* oggi = nullptr;
    if (orologioValido())
    {
      time_t now = time(nullptr);
      localtime_r(&now, &tmOggi);
      oggi = &tmOggi;
    }

    const uint32_t ora = millis();
    const uint32_t anticipo = Timings::ms(Timings::get().coalesceMin);

    bool serveRadio = false;
    for (uint8_t i = 0; i < nTask && !serveRadio; ++i)
      if (tabella[i].richiedeRadio && dovuto(tabella[i], ora, oggi, 0)) serveRadio = true;

    if (modalita == Radio::PROPRIA)
    {
      if (serveRadio)
      {
        ++giri;
        if (radioOn && radioOn())
        {
          radioInBackoff = false;
          eseguiFase(true, anticipo, oggi);
        }
        else
        {
          radioProssimoMs = millis() + WIFI_RITENTO_MS;
          radioInBackoff = true;
          LOG("sched", "connessione fallita: nessun tentativo di rete per %lu s",
              (unsigned long)(WIFI_RITENTO_MS / 1000UL));
        }
        if (radioOff) radioOff();
      }
    }
    else if (WiFi.status() == WL_CONNECTED)
    {
      eseguiFase(true, anticipo, oggi);
    }

    // I task locali non dipendono dalla radio e non sono anticipabili: il
    // display e' guidato dai dati nuovi, il sensore da un timer che non e'
    // nostro.
    eseguiFase(false, 0, oggi);
  }

  /** Vero quando il primo frame puo' essere disegnato: o la rete ha dato un
   *  esito qualsiasi, o e' passato troppo tempo dal boot per aspettarla. */
  inline bool primoFrameConsentito()
  {
    return detail::esitoReteVisto ||
           detail::trascorso(millis(), detail::bootMs, PRIMO_FRAME_ATTESA_MS);
  }

  /** Millisecondi al prossimo evento, gia' clampati ai limiti dello sleep. */
  inline uint32_t msAlProssimoEvento(const char** chi)
  {
    struct tm tmOggi;
    const struct tm* oggi = nullptr;
    if (orologioValido())
    {
      time_t now = time(nullptr);
      localtime_r(&now, &tmOggi);
      oggi = &tmOggi;
    }
    const uint32_t ora = millis();
    uint32_t minimo = MAI;
    *chi = "nessuno";
    for (uint8_t i = 0; i < detail::nTask; ++i)
    {
      const uint32_t s = msAllaScadenzaEffettiva(detail::tabella[i], ora, oggi);
      if (s < minimo) { minimo = s; *chi = detail::tabella[i].tag; }
    }
    if (minimo == MAI) minimo = (uint32_t)SLEEP_MAX_S * 1000UL;
    if (minimo < (uint32_t)SLEEP_MIN_S * 1000UL) minimo = (uint32_t)SLEEP_MIN_S * 1000UL;
    if (minimo > (uint32_t)SLEEP_MAX_S * 1000UL) minimo = (uint32_t)SLEEP_MAX_S * 1000UL;
    return minimo;
  }

  /**
   * Light sleep fino al prossimo evento. Prima di dormire verifica che la
   * radio sia davvero giu': nel percorso nominale l'ha gia' spenta il giro, e
   * se questa riga logga significa che l'invariante e' stata rotta altrove.
   */
  inline void dormi()
  {
    if (WiFi.getMode() != WIFI_OFF)
    {
      LOG("sched", "radio ancora accesa prima del light sleep: la spengo");
      if (detail::radioOff) detail::radioOff();
    }
    const char* chi = nullptr;
    const uint32_t ms = msAlProssimoEvento(&chi);
    if (detail::eseguitoNelGiro) LOG("sched", "sleep %lu s (prossimo: %s)", (unsigned long)(ms / 1000UL), chi);
    else LOGV("sched", "sleep %lu s (prossimo: %s)", (unsigned long)(ms / 1000UL), chi);
    Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
    const esp_err_t e = esp_light_sleep_start();
    if (e != ESP_OK) LOG("sched", "light sleep rifiutato: %d", (int)e);
  }

  /**
   * Registra la tabella e le due funzioni con cui lo scheduler accende e
   * spegne la radio. Da chiamare in setup() dopo Timings::begin() e dopo i
   * begin() dei moduli.
   */
  inline void begin(Task* tab, uint8_t n, bool (*on)(), void (*off)())
  {
    detail::tabella = tab;
    detail::nTask = n;
    detail::radioOn = on;
    detail::radioOff = off;
    detail::bootMs = millis();
    uint8_t rete = 0;
    for (uint8_t i = 0; i < n; ++i) if (tab[i].richiedeRadio) ++rete;
    LOG("sched", "%u task (%u di rete), coalescing %lu s",
        (unsigned)n, (unsigned)rete, (unsigned long)(Timings::ms(Timings::get().coalesceMin) / 1000UL));
  }

  /**
   * Dump dello stato, per rispondere alla domanda "perche' questo task non
   * parte". Con lo scheduler il motivo non e' piu' una riga di codice da
   * leggere ma la combinazione di piu' campi, quindi serve poterla stampare.
   */
  inline void stampaStato(Print& out)
  {
    struct tm tmOggi;
    const struct tm* oggi = nullptr;
    if (orologioValido())
    {
      time_t now = time(nullptr);
      localtime_r(&now, &tmOggi);
      oggi = &tmOggi;
    }
    const uint32_t ora = millis();
    out.printf("giri: %lu  orologio: %s  fascia: %s\n", (unsigned long)detail::giri,
               orologioValido() ? "sincronizzato" : "non sincronizzato",
               inFascia() ? "aperta" : "chiusa");
    out.printf("%-9s %-6s %-9s %8s %8s %8s\n", "task", "radio", "prossimo", "esiti", "ok", "falliti");
    for (uint8_t i = 0; i < detail::nTask; ++i)
    {
      const Task& t = detail::tabella[i];
      const uint32_t s = msAllaScadenzaEffettiva(t, ora, oggi);
      char quando[16];
      if (s == MAI) snprintf(quando, sizeof quando, "sospeso");
      else snprintf(quando, sizeof quando, "%lus", (unsigned long)(s / 1000UL));
      out.printf("%-9s %-6s %-9s %8lu %8lu %8lu\n", t.tag, t.richiedeRadio ? "si" : "no",
                 quando, (unsigned long)t.stato.esiti, (unsigned long)t.stato.ok,
                 (unsigned long)t.stato.falliti);
    }
  }
}

#endif // SCHEDULER_H

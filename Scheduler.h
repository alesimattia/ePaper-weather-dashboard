#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <time.h>
#include <stdint.h>

#include "Timings.h"
#include "Log.h"
#include "Clock.h"

/**
 * Scheduler centralizzato di tutti i flussi temporizzati: fetch di rete,
 * refresh del pannello, poll del sensore.
 *
 * Possiede il timing per intero. Intervallo, istante dell'ultimo slot,
 * contatore dei fallimenti e politica di ritento vivono qui, nella coppia
 * tabella + stato; i moduli espongono solo un'esecuzione che fa I/O e dice
 * com'è andata. L'intervallo non è copiato nella tabella ma indirizzato con
 * un puntatore a membro di Timings::Valori e riletto a ogni valutazione,
 * quindi una modifica dalla pagina di configurazione ha effetto dal giro
 * successivo senza riavvio.
 *
 * Non possiede la radio: la accende e la spegne solo attraverso le due
 * funzioni che gli vengono passate in begin(), e durante la finestra di
 * manutenzione non la tocca affatto, limitandosi a leggerne lo stato.
 *
 * L'esito di ogni task è indipendente dagli altri. Nessun percorso del
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
   *   FAILED        guasto imputabile al task: consuma un tentativo.
   *   SKIPPED       niente da fare, o guasto a monte (radio giù): lo stato
   *                 resta intatto e il tentativo non viene speso.
   */
  enum class Outcome : uint8_t { OK, FAILED, SKIPPED };

  /** Come si apre lo slot di un task. */
  enum class Cadence : uint8_t
  {
    PERIODIC,  // ogni intervallo dall'ultimo slot consumato
    DAILY      // una volta al giorno, dall'ora locale indicata in poi
  };

  /** Di chi è la radio durante il giro. */
  enum class RadioMode : uint8_t
  {
    MANAGED_BY_SCHEDULER,    // la accende e la spegne lo scheduler
    MANAGED_BY_MAINTENANCE   // è della finestra di manutenzione: si usa se c'è, non si tocca
  };

  /** Scadenza non calcolabile o non applicabile. */
  inline constexpr uint32_t NEVER = UINT32_MAX;

  /**
   * Descrittore di un task. La parte immutabile è la configurazione, `state`
   * è la sola parte che il runner modifica.
   *
   * Gli hook opzionali (nullptr = assente):
   *   preconditionsMet()  precondizione non temporale; falso esclude il task
   *                       anche dal calcolo del prossimo risveglio.
   *   hasIncompleteData() vero quando l'esecuzione è riuscita ma i dati non
   *                       bastano ancora: lo slot non si chiude e si ritenta.
   *   onRoundStart()      eseguito a inizio giro per TUTTI i task dovuti, prima
   *                       di qualunque run(): serve a chi deve avviare
   *                       qualcosa e lasciarlo maturare mentre gli altri girano.
   *   onOutcome(Outcome)  notifica al resto del firmware, tipicamente la
   *                       richiesta di ridisegno su esito positivo.
   *   customMsUntilDue()  per i task la cui cadenza la decide altri (il
   *                       sensore la decide la libreria BSEC).
   */
  struct Task
  {
    const char* tag;
    Cadence     cadence;
    bool        needsRadio;
    bool        runsOutsideRadioHours;              // vero solo per chi deve girare anche di notte

    uint16_t Timings::Valori::*intervalMinutes;     // PERIODIC; per il display è il rate limit
    uint16_t Timings::Valori::*dailyLocalHour;      // DAILY

    uint32_t retryIntervalMs;
    uint8_t  maxAttempts;                           // 0 = nessun ritento: un tentativo per intervallo, comunque vada

    Outcome  (*run)();
    bool     (*preconditionsMet)();
    bool     (*hasIncompleteData)();
    void     (*onRoundStart)();
    void     (*onOutcome)(Outcome);
    uint32_t (*customMsUntilDue)();

    struct State
    {
      bool     neverRun          = true;   // esplicito: nessuna sentinella su lastSlotMs
      uint32_t lastSlotMs        = 0;      // istante dell'ultimo slot consumato
      bool     retrying          = false;
      uint32_t nextAttemptMs     = 0;
      uint8_t  failedAttempts    = 0;      // falliti consecutivi dentro lo slot corrente
      int16_t  lastSlotDayOfYear = -1;     // tm_yday dell'ultimo slot giornaliero
      uint32_t outcomeCount = 0, okCount = 0, failedCount = 0;
    } state;
  };

  namespace detail
  {
    inline Task*    tasks     = nullptr;
    inline uint8_t  taskCount = 0;
    inline bool   (*turnRadioOn)()  = nullptr;
    inline void   (*turnRadioOff)() = nullptr;

    inline uint32_t bootMs             = 0;
    inline uint32_t radioRetryAtMs     = 0;   // backoff dopo un tentativo di connessione fallito
    inline bool     radioBackoffActive = false;
    inline bool     ranTaskThisRound   = false;
    inline bool     networkOutcomeSeen = false;
    inline uint32_t radioRounds        = 0;

    // Ultimo giro valutato durante la finestra di manutenzione, vedi runRound().
    inline bool     maintenanceRoundDone   = false;
    inline uint32_t lastMaintenanceRoundMs = 0;

    /** Confronto rollover-safe sulla differenza signed di millis(). */
    inline bool hasElapsed(uint32_t nowMs, uint32_t sinceMs, uint32_t thresholdMs)
    {
      return (int32_t)(nowMs - sinceMs) >= (int32_t)thresholdMs;
    }

    /** Millisecondi mancanti a `atMs`, 0 se già passato. */
    inline uint32_t msRemaining(uint32_t nowMs, uint32_t atMs)
    {
      const int32_t d = (int32_t)(atMs - nowMs);
      return d <= 0 ? 0 : (uint32_t)d;
    }

    /**
     * Ora locale corrente scritta in `buffer`, oppure nullptr con l'orologio non
     * sincronizzato. Unico punto in cui lo scheduler ricava il "giorno di oggi"
     * che passa alle valutazioni delle scadenze.
     */
    inline const struct tm* localNow(struct tm& buffer)
    {
      if (!Clock::valido()) return nullptr;
      const time_t now = time(nullptr);
      localtime_r(&now, &buffer);
      return &buffer;
    }
  }

  /**
   * Vero dentro la fascia oraria in cui la radio può accendersi.
   * Fail-open a orologio non sincronizzato: senza questa eccezione il primo
   * SNTP non potrebbe mai avvenire, perchè richiede la radio.
   */
  inline bool isInsideRadioHours()
  {
    struct tm buffer;
    const struct tm* local = detail::localNow(buffer);
    if (!local) return true;
    const auto& v = Timings::get();
    return local->tm_hour >= v.wifiOraInizio && local->tm_hour <= v.wifiOraFine;
  }

  /**
   * Millisecondi fino alla prossima occorrenza locale di `hour`:00.
   * mktime() con tm_isdst = -1 normalizza il giorno e assorbe il cambio
   * dell'ora legale. La differenza si calcola in interi su time_t, senza il
   * double di difftime() che sull'ESP32 è emulato in software. Presuppone
   * l'orologio sincronizzato.
   */
  inline uint32_t msUntilLocalHour(uint8_t hour, bool tomorrow)
  {
    const time_t now = time(nullptr);
    struct tm current;
    localtime_r(&now, &current);
    struct tm target = current;
    target.tm_hour = hour;
    target.tm_min = 0;
    target.tm_sec = 0;
    target.tm_isdst = -1;
    if (tomorrow || hour <= current.tm_hour) target.tm_mday += 1;
    const time_t at = mktime(&target);
    if (at == (time_t)-1 || at <= now) return 0;
    const int64_t ms = (int64_t)(at - now) * 1000;
    return ms >= (int64_t)NEVER ? NEVER - 1 : (uint32_t)ms;
  }

  /**
   * Millisecondi alla scadenza del task, senza tenere conto dei vincoli della
   * radio. 0 significa dovuto adesso, NEVER non determinabile.
   */
  inline uint32_t msUntilDue(const Task& t, uint32_t nowMs, const struct tm* today)
  {
    if (t.preconditionsMet && !t.preconditionsMet()) return NEVER;
    if (t.state.retrying) return detail::msRemaining(nowMs, t.state.nextAttemptMs);
    if (t.customMsUntilDue) return t.customMsUntilDue();
    if (t.state.neverRun) return 0;

    if (t.cadence == Cadence::PERIODIC)
      return detail::msRemaining(nowMs, t.state.lastSlotMs + Timings::ms(Timings::get().*t.intervalMinutes));

    // DAILY: senza orologio non c'è modo di sapere che ore sono.
    if (!today) return NEVER;
    const uint8_t targetHour = (uint8_t)(Timings::get().*t.dailyLocalHour);
    // Un secondo slot troppo ravvicinato sarebbe un doppione: succede quando
    // l'orologio diventa valido a metà giornata e l'ora target risulta
    // già passata.
    if (!detail::hasElapsed(nowMs, t.state.lastSlotMs, GIORNALIERA_DISTANZA_MIN_MS))
      return msUntilLocalHour(targetHour, true);
    // '>=' e non '==': se all'ora prevista la radio era giù, si recupera
    // alla prima occasione utile della stessa giornata.
    if (today->tm_yday != t.state.lastSlotDayOfYear && today->tm_hour >= targetHour) return 0;
    return msUntilLocalHour(targetHour, today->tm_yday == t.state.lastSlotDayOfYear);
  }

  /** Scadenza effettiva: aggiunge i vincoli che riguardano la radio. */
  inline uint32_t msUntilDueWithRadioLimits(const Task& t, uint32_t nowMs, const struct tm* today)
  {
    uint32_t s = msUntilDue(t, nowMs, today);
    if (s == NEVER || !t.needsRadio) return s;

    if (detail::radioBackoffActive)
    {
      const uint32_t r = detail::msRemaining(nowMs, detail::radioRetryAtMs);
      if (r == 0) detail::radioBackoffActive = false;
      else if (r > s) s = r;
    }
    if (!t.runsOutsideRadioHours && !isInsideRadioHours())
    {
      if (!today) return NEVER;
      const uint32_t r = msUntilLocalHour((uint8_t)Timings::get().wifiOraInizio, false);
      if (r > s) s = r;
    }
    return s;
  }

  /** Vero se il task scade entro `aheadMs`, vincoli della radio compresi. */
  inline bool isDue(const Task& t, uint32_t nowMs, const struct tm* today, uint32_t aheadMs)
  {
    const uint32_t s = msUntilDueWithRadioLimits(t, nowMs, today);
    return s != NEVER && s <= aheadMs;
  }

  namespace detail
  {
    /**
     * Chiude lo slot corrente: da qui il task non ritenta prima della
     * scadenza successiva. Il giorno di un task giornaliero si registra solo
     * con l'orologio valido, altrimenti si memorizzerebbe il 1 gennaio 1970
     * bruciando uno slot futuro.
     */
    inline void closeSlot(Task& t, uint32_t nowMs, const struct tm* today)
    {
      t.state.lastSlotMs = nowMs;
      t.state.neverRun = false;
      t.state.retrying = false;
      t.state.failedAttempts = 0;
      if (t.cadence == Cadence::DAILY && today) t.state.lastSlotDayOfYear = (int16_t)today->tm_yday;
    }

    /** Descrizione leggibile della prossima scadenza, per i log. */
    inline const char* describeNextDeadline(const Task& t, uint32_t nowMs, const struct tm* today)
    {
      static char buf[40];
      const uint32_t s = msUntilDue(t, nowMs, today);
      if (s == NEVER) snprintf(buf, sizeof buf, "sospeso");
      else if (t.cadence == Cadence::DAILY)
        snprintf(buf, sizeof buf, "fra %lu h", (unsigned long)(s / 3600000UL));
      else snprintf(buf, sizeof buf, "fra %lu s", (unsigned long)(s / 1000UL));
      return buf;
    }
  }

  /**
   * Registra l'esito di un task e decide se lo slot si chiude o si ritenta.
   * Un'esecuzione riuscita ma con dati ancora incompleti conta come
   * fallimento ai fini dello slot, pur sporcando il frame: è il caso del
   * meteo che ha la corrente ma non le previsioni.
   */
  inline void recordOutcome(Task& t, Outcome outcome, uint32_t nowMs, uint32_t durationMs, const struct tm* today)
  {
    if (outcome == Outcome::SKIPPED)
    {
      LOGV("sched", "%s: saltato", t.tag);
      if (t.onOutcome) t.onOutcome(outcome);
      return;
    }

    detail::ranTaskThisRound = true;
    if (t.needsRadio) detail::networkOutcomeSeen = true;
    t.state.outcomeCount++;

    const bool incomplete = (outcome != Outcome::FAILED) && t.hasIncompleteData && t.hasIncompleteData();
    const bool countsAsFailure = (outcome == Outcome::FAILED) || incomplete;

    if (outcome == Outcome::OK) t.state.okCount++;
    if (outcome == Outcome::FAILED) t.state.failedCount++;

    // Il ridisegno non è affare dello scheduler: chi porta dati nuovi lo
    // chiede nel proprio onOutcome(), che ne conosce la regola (di norma
    // "su OK", ma per esempio il display non si auto-sporca).
    if (t.onOutcome) t.onOutcome(outcome);

    if (countsAsFailure && t.maxAttempts > 0 && t.state.failedAttempts + 1 < t.maxAttempts)
    {
      t.state.failedAttempts++;
      t.state.retrying = true;
      t.state.nextAttemptMs = nowMs + t.retryIntervalMs;
      LOG("sched", "%s: %s in %lu ms (tentativo %u/%u), ritento fra %lu s",
          t.tag, incomplete ? "dati incompleti" : "fallito",
          (unsigned long)durationMs, (unsigned)t.state.failedAttempts, (unsigned)t.maxAttempts,
          (unsigned long)(t.retryIntervalMs / 1000UL));
      return;
    }

    const bool thresholdReached = countsAsFailure && t.maxAttempts > 0;
    detail::closeSlot(t, nowMs, today);
    if (countsAsFailure)
      LOG("sched", "%s: %s in %lu ms%s, prossimo %s", t.tag,
          incomplete ? "dati incompleti" : "fallito", (unsigned long)durationMs,
          thresholdReached ? ", soglia raggiunta" : "",
          detail::describeNextDeadline(t, nowMs, today));
    else
      LOG("sched", "%s: ok in %lu ms, prossimo %s", t.tag,
          (unsigned long)durationMs, detail::describeNextDeadline(t, nowMs, today));
  }

  namespace detail
  {
    /** Esegue i task dovuti di una fase: prima tutti gli onRoundStart(), poi i run(). */
    inline void runPhase(bool withRadio, uint32_t aheadMs, const struct tm* today)
    {
      const uint32_t nowMs = millis();
      for (uint8_t i = 0; i < taskCount; ++i)
        if (tasks[i].needsRadio == withRadio && isDue(tasks[i], nowMs, today, aheadMs) &&
            tasks[i].onRoundStart)
          tasks[i].onRoundStart();

      for (uint8_t i = 0; i < taskCount; ++i)
      {
        Task& t = tasks[i];
        if (t.needsRadio != withRadio) continue;
        if (!isDue(t, millis(), today, aheadMs)) continue;

        // Radio caduta a giro iniziato: guasto a monte, non del task. Lo slot
        // resta intatto così il tentativo viene speso davvero al primo giro
        // con la radio su.
        if (t.needsRadio && WiFi.status() != WL_CONNECTED)
        {
          LOG("sched", "%s: saltato, radio non connessa (slot intatto)", t.tag);
          recordOutcome(t, Outcome::SKIPPED, millis(), 0, today);
          continue;
        }

        const uint32_t startMs = millis();
        const Outcome outcome = t.run();
        const uint32_t endMs = millis();
        recordOutcome(t, outcome, endMs, endMs - startMs, today);
      }
    }
  }

  /**
   * Un giro completo. Con la radio dello scheduler la accende solo se almeno
   * un task di rete è scaduto davvero (nessun anticipo), poi esegue con
   * l'anticipo del coalescing: così la finestra si apre quando serve, ma una
   * volta aperta ci si infila anche chi scadrebbe fra poco.
   *
   * Nella finestra di manutenzione il loop chiama questa funzione ogni pochi
   * millisecondi per servire il web server, e il giro si valuta al più ogni
   * MAINTENANCE_SCHEDULER_PERIOD_MS, perchè nessun task ha una granularità
   * più fine del secondo.
   */
  inline void runRound(RadioMode mode)
  {
    using namespace detail;
    ranTaskThisRound = false;

    if (mode == RadioMode::MANAGED_BY_MAINTENANCE)
    {
      const uint32_t nowMs = millis();
      if (maintenanceRoundDone && !hasElapsed(nowMs, lastMaintenanceRoundMs, MAINTENANCE_SCHEDULER_PERIOD_MS))
        return;
      maintenanceRoundDone = true;
      lastMaintenanceRoundMs = nowMs;
    }

    const uint32_t aheadMs = Timings::ms(Timings::get().coalesceMin);
    struct tm localBuffer;

    if (mode == RadioMode::MANAGED_BY_SCHEDULER)
    {
      const struct tm* today = localNow(localBuffer);
      const uint32_t nowMs = millis();
      bool radioNeeded = false;
      for (uint8_t i = 0; i < taskCount && !radioNeeded; ++i)
        if (tasks[i].needsRadio && isDue(tasks[i], nowMs, today, 0)) radioNeeded = true;

      if (radioNeeded)
      {
        ++radioRounds;
        if (turnRadioOn && turnRadioOn())
        {
          radioBackoffActive = false;
          // L'ora si rilegge qui: la prima sincronizzazione SNTP avviene
          // dentro turnRadioOn(), e senza un'ora valida un task giornaliero
          // non registrerebbe il giorno del proprio slot.
          runPhase(true, aheadMs, localNow(localBuffer));
        }
        else
        {
          radioRetryAtMs = millis() + WIFI_RITENTO_MS;
          radioBackoffActive = true;
          LOG("sched", "connessione fallita: nessun tentativo di rete per %lu s",
              (unsigned long)(WIFI_RITENTO_MS / 1000UL));
        }
        if (turnRadioOff) turnRadioOff();
      }
    }
    else if (WiFi.status() == WL_CONNECTED)
    {
      runPhase(true, aheadMs, localNow(localBuffer));
    }

    // I task locali non dipendono dalla radio e non sono anticipabili: il
    // display è guidato dai dati nuovi, il sensore da un timer che non è
    // nostro.
    runPhase(false, 0, localNow(localBuffer));
  }

  /** Vero se nell'ultimo giro almeno un task ha prodotto un esito: il .ino lo
   *  usa per scrivere la diagnostica di memoria solo nei giri che hanno fatto
   *  qualcosa. */
  inline bool ranTasksThisRound() { return detail::ranTaskThisRound; }

  /** Vero quando il primo frame può essere disegnato: o la rete ha dato un
   *  esito qualsiasi, o è passato troppo tempo dal boot per aspettarla. */
  inline bool isFirstFrameAllowed()
  {
    return detail::networkOutcomeSeen ||
           detail::hasElapsed(millis(), detail::bootMs, PRIMO_FRAME_ATTESA_MS);
  }

  /** Millisecondi al prossimo evento, già clampati ai limiti dello sleep. */
  inline uint32_t msUntilNextEvent(const char** who)
  {
    struct tm localBuffer;
    const struct tm* today = detail::localNow(localBuffer);
    const uint32_t nowMs = millis();
    uint32_t earliest = NEVER;
    *who = "nessuno";
    for (uint8_t i = 0; i < detail::taskCount; ++i)
    {
      const uint32_t s = msUntilDueWithRadioLimits(detail::tasks[i], nowMs, today);
      if (s < earliest) { earliest = s; *who = detail::tasks[i].tag; }
    }
    if (earliest == NEVER) earliest = (uint32_t)SLEEP_MAX_S * 1000UL;
    if (earliest < (uint32_t)SLEEP_MIN_S * 1000UL) earliest = (uint32_t)SLEEP_MIN_S * 1000UL;
    if (earliest > (uint32_t)SLEEP_MAX_S * 1000UL) earliest = (uint32_t)SLEEP_MAX_S * 1000UL;
    return earliest;
  }

  /**
   * Light sleep fino al prossimo evento. Prima di dormire verifica che la
   * radio sia davvero giù: nel percorso nominale l'ha già spenta il giro, e
   * se questa riga logga significa che l'invariante è stata rotta altrove.
   */
  inline void sleepUntilNextEvent()
  {
    if (WiFi.getMode() != WIFI_OFF)
    {
      LOG("sched", "radio ancora accesa prima del light sleep: la spengo");
      if (detail::turnRadioOff) detail::turnRadioOff();
    }
    const char* who = nullptr;
    const uint32_t ms = msUntilNextEvent(&who);
    if (detail::ranTaskThisRound) LOG("sched", "sleep %lu s (prossimo: %s)", (unsigned long)(ms / 1000UL), who);
    else LOGV("sched", "sleep %lu s (prossimo: %s)", (unsigned long)(ms / 1000UL), who);
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
  inline void begin(Task* table, uint8_t count, bool (*radioOn)(), void (*radioOff)())
  {
    detail::tasks = table;
    detail::taskCount = count;
    detail::turnRadioOn = radioOn;
    detail::turnRadioOff = radioOff;
    detail::bootMs = millis();
    uint8_t networkTasks = 0;
    for (uint8_t i = 0; i < count; ++i) if (table[i].needsRadio) ++networkTasks;
    LOG("sched", "%u task (%u di rete), coalescing %lu s",
        (unsigned)count, (unsigned)networkTasks, (unsigned long)(Timings::ms(Timings::get().coalesceMin) / 1000UL));
  }

  /**
   * Dump dello stato, per rispondere alla domanda "perchè questo task non
   * parte". Con lo scheduler il motivo non è più una riga di codice da
   * leggere ma la combinazione di più campi, quindi serve poterla stampare.
   */
  inline void printStatus(Print& out)
  {
    struct tm localBuffer;
    const struct tm* today = detail::localNow(localBuffer);
    const uint32_t nowMs = millis();
    out.printf("giri: %lu  orologio: %s  fascia: %s\n", (unsigned long)detail::radioRounds,
               Clock::valido() ? "sincronizzato" : "non sincronizzato",
               isInsideRadioHours() ? "aperta" : "chiusa");
    // Separa la riga dei giri dalla tabella dei task, larga quanto la tabella.
    out.printf("-----------------------------------------------------\n");
    out.printf("%-9s %-6s %-9s %8s %8s %8s\n", "task", "radio", "prossimo", "esiti", "ok", "falliti");
    for (uint8_t i = 0; i < detail::taskCount; ++i)
    {
      const Task& t = detail::tasks[i];
      const uint32_t s = msUntilDueWithRadioLimits(t, nowMs, today);
      char when[16];
      if (s == NEVER) snprintf(when, sizeof when, "sospeso");
      else snprintf(when, sizeof when, "%lus", (unsigned long)(s / 1000UL));
      out.printf("%-9s %-6s %-9s %8lu %8lu %8lu\n", t.tag, t.needsRadio ? "si" : "no",
                 when, (unsigned long)t.state.outcomeCount, (unsigned long)t.state.okCount,
                 (unsigned long)t.state.failedCount);
    }
  }
}

#endif // SCHEDULER_H

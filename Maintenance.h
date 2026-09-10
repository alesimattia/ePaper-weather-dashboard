#ifndef MAINTENANCE_H
#define MAINTENANCE_H

/**
 * Finestra di manutenzione: aggiornamento del firmware e configurazione dei
 * tempi, per pochi minuti dopo ogni avvio.
 *
 * Il server sta preferibilmente sulla rete di casa (STA), cosi' ci si arriva
 * dal proprio PC senza cambiare WiFi, all'indirizzo IP o al nome mDNS. Se la
 * STA non sale entro MAINT_STA_TIMEOUT_S si accende l'access point dedicato:
 * e' l'unica via di recupero per un pannello a muro con credenziali WiFi
 * sbagliate, e per questo il fallback esiste.
 *
 * La finestra possiede la radio finche' e' aperta: lo scheduler la usa se e
 * quando la STA e' connessa, ma non la accende ne' la spegne. Alla chiusura
 * la radio viene consegnata spenta.
 *
 * Quattro trappole, tutte verificate sul core, che rendono l'upload fragile
 * se toccate:
 *   - non chiamare mai server.collectHeaders(): la chiama updater.setup()
 *     con Origin e Host per il controllo CSRF, e una seconda chiamata
 *     sostituirebbe la lista facendo fallire ogni upload;
 *   - l'action del form deve restare relativa, per lo stesso controllo;
 *   - l'autenticazione dell'upload va passata a updater.setup(): il
 *     callback scrive la partizione durante il parsing, quindi un controllo
 *     a valle arriverebbe troppo tardi;
 *   - lo schema di partizioni deve avere due slot applicative.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <Update.h>

#include "Env.h"
#include "Timings.h"
#include "Log.h"
#include "Scheduler.h"

/** Nome con cui il dispositivo si annuncia su mDNS e al DHCP del router. */
#ifndef MAINT_HOSTNAME
  #define MAINT_HOSTNAME "epd-dashboard"
#endif

/** L'annuncio mDNS costa una cinquantina di KB di flash: si puo' spegnere e
 *  raggiungere il dispositivo per indirizzo IP. */
#ifndef MAINT_MDNS
  #define MAINT_MDNS 1
#endif

/** Utente della Basic Auth. Attiva solo se Env.h definisce anche
 *  MAINT_HTTP_PASSWORD. */
#ifndef MAINT_HTTP_USER
  #define MAINT_HTTP_USER "admin"
#endif

#if MAINT_MDNS
  #include <ESPmDNS.h>
#endif

namespace Maintenance
{
  /** Dove sta il server, e quindi di chi e' la radio. */
  enum class Stato : uint8_t
  {
    Inattiva,   // prima di begin() o dopo chiudi()
    AttesaSta,  // la STA sta salendo, nessun indirizzo ancora
    ServerSta,  // raggiungibile sulla rete di casa
    ServerAp    // STA non salita: access point di emergenza
  };

  namespace detail
  {
    inline WebServer        server(80);
    inline HTTPUpdateServer updater;

    inline Stato    stato            = Stato::Inattiva;
    inline bool     conclusa         = false;
    inline bool     mdnsAttivo       = false;
    inline bool     cadutaLoggata    = false;
    inline uint32_t inizioAttesaMs   = 0;
    inline uint32_t apertaMs         = 0;   // istante in cui il server e' diventato raggiungibile
    inline uint32_t scadenzaMs       = 0;

    /**
     * Ogni richiesta servita sposta la scadenza in avanti, con un tetto
     * assoluto: senza, una pagina aperta a ridosso della fine porterebbe a un
     * upload rifiutato a meta' compilazione del form.
     */
    inline constexpr uint32_t PROROGA_MS  = 60UL * 1000UL;
    inline constexpr uint8_t  TETTO_FATT  = 2;

    inline uint32_t finestraMs() { return Timings::ms(Timings::get().finestraManutenzioneMin); }
    inline uint32_t attesaStaMs() { return (uint32_t)MAINT_STA_TIMEOUT_S * 1000UL; }

    /** Secondi che restano alla chiusura, 0 se gia' scaduta. */
    inline uint32_t secondiResidui()
    {
      const int32_t d = (int32_t)(scadenzaMs - millis());
      return d <= 0 ? 0 : (uint32_t)d / 1000UL;
    }

    /**
     * Basic Auth, attiva solo se Env.h definisce una password. Sulla rete di
     * casa la pagina e' raggiungibile da chiunque sia sulla LAN, e /update
     * accetta un firmware qualunque: e' la mitigazione proporzionata a un
     * dispositivo domestico, non una difesa completa.
     */
    inline bool autorizzato()
    {
    #ifdef MAINT_HTTP_PASSWORD
      if (!server.authenticate(MAINT_HTTP_USER, MAINT_HTTP_PASSWORD))
      {
        server.requestAuthentication(BASIC_AUTH, "ePaper");
        return false;
      }
    #endif
      return true;
    }

    /**
     * Stesso criterio del controllo CSRF di HTTPUpdateServer, sugli header
     * che updater.setup() gia' raccoglie. Blocca una POST inviata da una
     * pagina esterna aperta nel browser durante la finestra.
     */
    inline bool origineValida()
    {
      const String origine = server.header("Origin");
      if (origine.length() == 0) return true;   // richiesta non originata da una pagina
      return origine == String("http://") + server.header("Host");
    }

    inline void proroga()
    {
      const uint32_t nuova = millis() + PROROGA_MS;
      const uint32_t tetto = apertaMs + finestraMs() * TETTO_FATT;
      const uint32_t limite = ((int32_t)(nuova - tetto) > 0) ? tetto : nuova;
      if ((int32_t)(limite - scadenzaMs) > 0) scadenzaMs = limite;
    }

    /** Dopo un salvataggio la nuova durata vale anche per la finestra in corso. */
    inline void ricalcolaScadenza()
    {
      const uint32_t nuova = apertaMs + finestraMs();
      if ((int32_t)(nuova - scadenzaMs) > 0) scadenzaMs = nuova;
    }

    inline void avviaMdns()
    {
    #if MAINT_MDNS
      if (mdnsAttivo) return;
      if (MDNS.begin(MAINT_HOSTNAME))
      {
        MDNS.addService("http", "tcp", 80);
        mdnsAttivo = true;
      }
      else
      {
        LOG("MAINT", "mDNS non avviato: raggiungibile solo per indirizzo IP");
      }
    #endif
    }

    inline void apri(Stato s)
    {
      stato = s;
      apertaMs = millis();
      scadenzaMs = apertaMs + finestraMs();
      if (s == Stato::ServerSta)
      {
        avviaMdns();
        LOG("MAINT", "server su rete di casa: http://%s/ oppure http://%s.local/ per %lu s",
            WiFi.localIP().toString().c_str(), MAINT_HOSTNAME,
            (unsigned long)(finestraMs() / 1000UL));
      }
      else
      {
        LOG("MAINT", "STA non salita in %lu s: access point %s, http://%s/ per %lu s",
            (unsigned long)(attesaStaMs() / 1000UL), OTA_AP_SSID,
            WiFi.softAPIP().toString().c_str(), (unsigned long)(finestraMs() / 1000UL));
      }
    }

    // -----------------------------------------------------------------------
    // Pagine. I pezzi fissi stanno in PROGMEM; quelle con valori correnti si
    // compongono a blocchi (Transfer-Encoding chunked) per non costruire una
    // String grande in RAM.
    // -----------------------------------------------------------------------

    static const char PAGINA_UPDATE[] PROGMEM =
        "<!DOCTYPE html>"
        "<title>Firmware</title>"
        "<meta name=viewport content=\"width=device-width\">"
        "<h2>Aggiornamento firmware</h2>"
        "<form method=POST action=/update enctype=multipart/form-data>"
        "<input type=file name=update accept=.bin required>"
        "<button>Carica</button>"
        "</form>"
        "<p><a href=/>Home</a>";

    static const char INDICE_FMT[] PROGMEM =
        "<!DOCTYPE html>"
        "<title>%s</title>"
        "<meta name=viewport content=\"width=device-width\">"
        "<h2>%s</h2>"
        "<p>Manutenzione via %s, indirizzo %s"
        "<p>Finestra aperta ancora %lu s; ogni richiesta la prolunga."
        "<p><a href=/config>Tempi</a> | <a href=/update>Firmware</a> | <a href=/status>Stato</a>";

    static const char CONFIG_TESTA[] PROGMEM =
        "<!DOCTYPE html>"
        "<title>Tempi</title>"
        "<meta name=viewport content=\"width=device-width\">"
        "<h2>Tempi</h2>"
        "<form method=POST action=/config>";

    static const char CONFIG_RIGA_FMT[] PROGMEM =
        "<label>%s <input type=number name=%s value=%u min=%u max=%u required> %s"
        " (minimo %u, predefinito %u)</label><br>";

    static const char CONFIG_CODA[] PROGMEM =
        "<p><button>Salva</button></form>"
        "<form method=POST action=/config/reset><button>Ripristina predefiniti</button></form>"
        "<p><a href=/>Home</a> | <a href=/update>Firmware</a> | <a href=/status>Stato</a>";

    static const char ESITO_TESTA[] PROGMEM =
        "<!DOCTYPE html><title>Tempi</title>"
        "<meta name=viewport content=\"width=device-width\"><h2>Esito</h2><ul>";

    static const char ESITO_CODA[] PROGMEM =
        "</ul><p><a href=/config>Torna ai tempi</a> | <a href=/>Home</a>";

    inline void handleIndice()
    {
      if (!autorizzato()) return;
      proroga();
      const char* via = (stato == Stato::ServerAp) ? "access point" : "rete di casa";
      const String ip = (stato == Stato::ServerAp) ? WiFi.softAPIP().toString()
                                                   : WiFi.localIP().toString();
      char buf[512];
      snprintf_P(buf, sizeof buf, INDICE_FMT, MAINT_HOSTNAME, MAINT_HOSTNAME, via, ip.c_str(),
                 (unsigned long)secondiResidui());
      server.send(200, "text/html", buf);
    }

    inline void handleUpdate()
    {
      if (!autorizzato()) return;
      proroga();
      server.send_P(200, PSTR("text/html"), PAGINA_UPDATE);
    }

    inline void handleConfigGet()
    {
      if (!autorizzato()) return;
      proroga();
      server.setContentLength(CONTENT_LENGTH_UNKNOWN);
      server.send_P(200, PSTR("text/html"), CONFIG_TESTA);
      char riga[256];
      for (size_t i = 0; i < Timings::nCampi(); ++i)
      {
        const Timings::Campo& c = Timings::campi()[i];
        const int n = snprintf_P(riga, sizeof riga, CONFIG_RIGA_FMT,
                                 c.descrizione, c.chiave,
                                 (unsigned)(Timings::get().*(c.campo)),
                                 (unsigned)c.minimo, (unsigned)c.massimo, c.unita,
                                 (unsigned)c.minimo, (unsigned)c.predefinito);
        server.sendContent(riga, (size_t)n);
      }
      server.sendContent_P(CONFIG_CODA);
      server.sendContent("", 0);
    }

    /** Solo cifre: strtoul accetterebbe segno e spazi iniziali. */
    inline bool leggiNumero(const String& s, uint16_t& out)
    {
      if (s.length() == 0 || s.length() > 5) return false;
      uint32_t v = 0;
      for (size_t i = 0; i < s.length(); ++i)
      {
        if (!isdigit((unsigned char)s[i])) return false;
        v = v * 10 + (uint32_t)(s[i] - '0');
      }
      if (v > 0xFFFF) return false;
      out = (uint16_t)v;
      return true;
    }

    inline void handleConfigPost()
    {
      if (!autorizzato()) return;
      if (!origineValida()) { server.send(403, "text/plain", "origine non valida"); return; }
      proroga();
      server.setContentLength(CONTENT_LENGTH_UNKNOWN);
      server.send_P(200, PSTR("text/html"), ESITO_TESTA);

      char riga[224];
      uint8_t proposti = 0, malformati = 0;
      /**
       * Si costruisce un candidato completo e lo si applica in blocco: i
       * vincoli legano piu' campi fra loro, quindi applicarli uno alla volta
       * rifiuterebbe combinazioni valide a seconda dell'ordine.
       *
       * Si itera sui campi noti cercando l'argomento, non sugli argomenti
       * ricevuti: cosi' nella pagina finiscono solo stringhe nostre e il
       * numero appena parsato, mai testo arrivato dal client.
       */
      Timings::Valori candidato = Timings::get();
      for (size_t i = 0; i < Timings::nCampi(); ++i)
      {
        const Timings::Campo& c = Timings::campi()[i];
        if (!server.hasArg(c.chiave)) continue;
        uint16_t v = 0;
        if (!leggiNumero(server.arg(c.chiave), v))
        {
          ++malformati;
          const int n = snprintf(riga, sizeof riga, "<li>%s: valore non numerico, ignorato",
                                 c.descrizione);
          server.sendContent(riga, (size_t)n);
          continue;
        }
        candidato.*(c.campo) = v;
        ++proposti;
        const int n = snprintf(riga, sizeof riga, "<li>%s: %u %s",
                               c.descrizione, (unsigned)v, c.unita);
        server.sendContent(riga, (size_t)n);
      }

      bool salvato = false;
      const char* motivo = nullptr;
      if (proposti && Timings::applica(candidato, &motivo))
      {
        salvato = Timings::save();
        server.sendContent_P(salvato
            ? PSTR("</ul><p>Salvato, attivo dal prossimo giro senza riavvio<ul>")
            : PSTR("</ul><p><b>Salvataggio fallito</b>: valori attivi solo fino al riavvio<ul>"));
        ricalcolaScadenza();
      }
      else if (proposti)
      {
        const int n = snprintf(riga, sizeof riga,
                               "</ul><p><b>Nessuna modifica applicata</b>: %s<ul>",
                               motivo ? motivo : "combinazione non valida");
        server.sendContent(riga, (size_t)n);
      }
      server.sendContent_P(ESITO_CODA);
      server.sendContent("", 0);
      LOG("MAINT", "config: %u campi proposti, %u malformati, esito %s",
          (unsigned)proposti, (unsigned)malformati,
          salvato ? "salvato" : (proposti ? "rifiutato" : "niente da fare"));
    }

    inline void handleConfigReset()
    {
      if (!autorizzato()) return;
      if (!origineValida()) { server.send(403, "text/plain", "origine non valida"); return; }
      proroga();
      const bool ok = Timings::ripristinaDefault();
      ricalcolaScadenza();
      server.send_P(200, PSTR("text/html"), ok
          ? PSTR("<!DOCTYPE html><title>Tempi</title><p>Predefiniti ripristinati"
                 "<p><a href=/config>Torna ai tempi</a>")
          : PSTR("<!DOCTYPE html><title>Tempi</title><p><b>Ripristino fallito</b>"
                 "<p><a href=/config>Torna ai tempi</a>"));
      LOG("MAINT", "config: ripristino predefiniti %s", ok ? "riuscito" : "fallito");
    }

    /** Manda su HTTP, a blocchi, cio' che lo scheduler scrive su un Print. */
    struct UscitaHttp : public Print
    {
      size_t write(uint8_t c) override { return write(&c, 1); }
      size_t write(const uint8_t* b, size_t n) override
      {
        server.sendContent((const char*)b, n);
        return n;
      }
    };

    inline void handleStato()
    {
      if (!autorizzato()) return;
      proroga();
      server.setContentLength(CONTENT_LENGTH_UNKNOWN);
      server.send(200, "text/plain", "");
      UscitaHttp out;
      out.printf("hostname: %s\n", MAINT_HOSTNAME);
      out.printf("uptime: %lu s\n", (unsigned long)(millis() / 1000UL));
      out.printf("heap libero: %lu byte\n", (unsigned long)ESP.getFreeHeap());
      out.printf("finestra: ancora %lu s\n",
                 (unsigned long)secondiResidui());
      out.print("\n");
      Scheduler::stampaStato(out);
      server.sendContent("", 0);
    }
  }

  /**
   * Avvia la STA e il server. Non blocca: la connessione la completa il core
   * in background e la macchina a stati di servi() decide se il server finira'
   * sulla rete di casa o sull'access point di emergenza.
   *
   * Da chiamare in setup() dopo Timings::begin(), perche' la durata della
   * finestra e' una cadenza configurabile.
   */
  inline void begin()
  {
    using namespace detail;
    if (stato != Stato::Inattiva || conclusa) return;

    // setHostname() va prima di mode(): il nome viene applicato all'interfaccia
    // quando la STA viene abilitata, non quando la variabile viene scritta.
    WiFi.setHostname(MAINT_HOSTNAME);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // La nostra pagina di /update va registrata PRIMA di updater.setup():
    // il WebServer sceglie il primo handler che combacia.
    server.on("/", HTTP_GET, handleIndice);
    server.on("/config", HTTP_GET, handleConfigGet);
    server.on("/config", HTTP_POST, handleConfigPost);
    server.on("/config/reset", HTTP_POST, handleConfigReset);
    server.on("/status", HTTP_GET, handleStato);
    server.on("/update", HTTP_GET, handleUpdate);
  #ifdef MAINT_HTTP_PASSWORD
    updater.setup(&server, "/update", MAINT_HTTP_USER, MAINT_HTTP_PASSWORD);
  #else
    updater.setup(&server, "/update");
  #endif
    Update.onProgress([](size_t fatto, size_t totale) {
      static uint8_t ultimo = 255;
      const uint8_t pct = totale ? (uint8_t)((uint64_t)fatto * 100ULL / totale) : 0;
      if (pct / 10 != ultimo) { ultimo = (uint8_t)(pct / 10); LOG("OTA", "upload %u%%", (unsigned)pct); }
    });
    // Il server ascolta su tutte le interfacce: serve la STA o l'AP, quale
    // delle due abbia un indirizzo. Nessun rebind al cambio di stato.
    server.begin();

    inizioAttesaMs = millis();
    stato = Stato::AttesaSta;
    LOG("MAINT", "STA in risalita, attendo fino a %lu s (hostname %s)",
        (unsigned long)(attesaStaMs() / 1000UL), MAINT_HOSTNAME);
  }

  /** Dove sta il server in questo momento. */
  inline Stato statoCorrente() { return detail::stato; }

  inline bool finestraAperta()
  {
    using namespace detail;
    if (stato == Stato::Inattiva) return false;
    if (stato == Stato::AttesaSta) return true;   // il timeout lo gestisce servi()
    return (int32_t)(millis() - scadenzaMs) < 0;
  }

  /** Fa avanzare la macchina a stati e serve una richiesta se ce n'e' una. */
  inline void servi()
  {
    using namespace detail;
    switch (stato)
    {
      case Stato::Inattiva:
        return;

      case Stato::AttesaSta:
        if (WiFi.status() == WL_CONNECTED) apri(Stato::ServerSta);
        else if ((uint32_t)(millis() - inizioAttesaMs) >= attesaStaMs())
        {
          // AP_STA e non AP: la STA continua a tentare, cosi' un router piu'
          // lento del dispositivo (tipico dopo un blackout) non condanna il
          // giro alla sola rete di emergenza.
          WiFi.mode(WIFI_AP_STA);
          WiFi.softAP(OTA_AP_SSID, OTA_AP_PASSWORD);
          apri(Stato::ServerAp);
        }
        return;   // senza indirizzo non c'e' niente da servire

      case Stato::ServerSta:
        if (WiFi.status() != WL_CONNECTED && !cadutaLoggata)
        {
          cadutaLoggata = true;
          LOG("MAINT", "STA caduta: riconnessione automatica, server ancora in ascolto");
        }
        break;

      case Stato::ServerAp:
        if (WiFi.status() == WL_CONNECTED && !mdnsAttivo)
        {
          avviaMdns();
          LOG("MAINT", "STA salita: raggiungibile anche su http://%s/",
              WiFi.localIP().toString().c_str());
        }
        break;
    }
    server.handleClient();
  }

  /**
   * Chiude la finestra e consegna la radio spenta allo scheduler.
   * Idempotente. La finestra non si riapre senza riavvio.
   */
  inline void chiudi()
  {
    using namespace detail;
    if (stato == Stato::Inattiva) return;
    const char* via = (stato == Stato::ServerAp) ? "access point"
                    : (stato == Stato::ServerSta ? "rete di casa" : "mai aperta");
    const uint32_t durata = apertaMs ? (millis() - apertaMs) / 1000UL : 0;
    server.stop();
  #if MAINT_MDNS
    if (mdnsAttivo) { MDNS.end(); mdnsAttivo = false; }
  #endif
    if (stato == Stato::ServerAp) WiFi.softAPdisconnect(false);
    // Stessa sequenza di wifiOff(): eraseap a false conserva BSSID e canale,
    // che fanno risparmiare tempo alla prima riconnessione dello scheduler.
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    stato = Stato::Inattiva;
    conclusa = true;
    LOG("MAINT", "finestra chiusa (%s) dopo %lu s, radio spenta", via, (unsigned long)durata);
  }
}

#endif // MAINTENANCE_H

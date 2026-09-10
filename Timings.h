#ifndef TIMINGS_H
#define TIMINGS_H

#include <Arduino.h>
#include <Preferences.h>
#include <stdint.h>

#include "Log.h"

/**
 * Tutti i tempi del firmware in un posto solo.
 *
 * Tre sezioni, con criteri diversi:
 *
 *   1. MODI HARDWARE - valori imposti da una libreria o dalla fisica del
 *      pannello. Non sono intervalli liberi e non si configurano.
 *   2. CADENZE - i tempi che l'utente regola. Il default sta qui ed e'
 *      sovrascrivibile a runtime (namespace Timings, persistenza NVS);
 *      accanto a ogni default c'e' il PAVIMENTO, cioe' il valore sotto il
 *      quale interrogare piu' spesso non produce informazione nuova.
 *   3. COMPILE-TIME - timeout, budget e ritenti: dipendono da protocollo,
 *      libreria o hardware, non da una preferenza, e non sono configurabili.
 *
 * I default della sezione 2 restano #ifndef cosi' che un modulo compilato da
 * solo li raccolga; per cambiarli si edita questo file o si usa la pagina di
 * configurazione, non si ridefiniscono altrove.
 */

// ---------------------------------------------------------------------------
// 1. Modi hardware
// ---------------------------------------------------------------------------

/**
 * Periodo di campionamento del BME680 in modalita' ULP (BSEC_SAMPLE_RATE_ULP
 * = 1/300 Hz). Non e' un intervallo scelto da noi: e' il modo con cui BSEC e'
 * sottoscritto. Gli altri modi della libreria sono LP (3 s) e CONT (1 s), che
 * imporrebbero un polling incompatibile con il light sleep di questo firmware.
 */
#define BSEC_PERIODO_ULP_S 300

/** Durata misurata di un refresh pieno del pannello. Nessuna cadenza di
 *  ridisegno puo' scendere sotto questo valore. */
#define DISPLAY_REFRESH_PIENO_S 24

/** Pavimento e tetto del light sleep dinamico. Il tetto e' il periodo ULP:
 *  piu' alto perderebbe campioni, piu' basso sveglierebbe a vuoto. Il
 *  pavimento evita che un errore di calcolo produca un ciclo di risvegli. */
#define SLEEP_MIN_S 30
#define SLEEP_MAX_S BSEC_PERIODO_ULP_S

// ---------------------------------------------------------------------------
// 2. Cadenze configurabili (minuti, salvo dove indicato)
// ---------------------------------------------------------------------------

/** Distanza MINIMA fra due refresh del pannello. Il ridisegno avviene solo
 *  quando un fetch ha portato dati nuovi, mai piu' spesso di cosi'. */
#ifndef DISPLAY_REFRESH_MIN
  #define DISPLAY_REFRESH_MIN 5
#endif
#define DISPLAY_REFRESH_MIN_FLOOR 1

/** Meteo. Pavimento pari alla cadenza con cui One Call aggiorna i dati:
 *  sotto i 10 min si riscarica lo stesso contenuto. Il limite duro e' invece
 *  la quota del piano (1000 chiamate/giorno = una ogni 1,44 min). */
#ifndef WEATHER_FORECAST_FETCH_MIN
  #define WEATHER_FORECAST_FETCH_MIN 10
#endif
#define WEATHER_FORECAST_FETCH_MIN_FLOOR 10

/** Calendari e posta: nessun pavimento sui dati (un evento o una mail possono
 *  arrivare in qualsiasi momento). Il costo e' energetico, ~5-10 s di radio
 *  accesa per fetch. */
#ifndef CAL_OUTLOOK_FETCH_MIN
  #define CAL_OUTLOOK_FETCH_MIN 10
#endif
#define CAL_OUTLOOK_FETCH_MIN_FLOOR 1

#ifndef CAL_GOOGLE_FETCH_MIN
  #define CAL_GOOGLE_FETCH_MIN 10
#endif
#define CAL_GOOGLE_FETCH_MIN_FLOOR 1

#ifndef MAIL_GOOGLE_FETCH_MIN
  #define MAIL_GOOGLE_FETCH_MIN 10
#endif
#define MAIL_GOOGLE_FETCH_MIN_FLOOR 1

/**
 * Finestra di coalescing: con la radio gia' accesa vengono eseguiti anche i
 * task che scadrebbero entro questo margine, cosi' scadenze vicine
 * condividono una sola accensione. Un task puo' quindi partire in anticipo,
 * mai in ritardo. 0 disattiva il meccanismo.
 */
#ifndef FETCH_COALESCE_MIN
  #define FETCH_COALESCE_MIN 2
#endif
#define FETCH_COALESCE_MIN_FLOOR 0

/** Durata della finestra di manutenzione (pagina di aggiornamento firmware e
 *  configurazione) aperta a ogni boot. */
#ifndef OTA_WINDOW_MIN
  #define OTA_WINDOW_MIN 3
#endif
#define OTA_WINDOW_MIN_FLOOR 1

/** Attesa della connessione alla rete di casa prima di ripiegare
 *  sull'access point di emergenza. Coerente con WIFI_CONNECT_TIMEOUT_MS. */
#ifndef MAINT_STA_TIMEOUT_S
  #define MAINT_STA_TIMEOUT_S 15
#endif

/** Fascia oraria locale in cui la radio puo' accendersi per i fetch. */
#ifndef WIFI_ACTIVE_HOUR_START
  #define WIFI_ACTIVE_HOUR_START 7
#endif
#ifndef WIFI_ACTIVE_HOUR_END
  #define WIFI_ACTIVE_HOUR_END 23
#endif

/**
 * Ora locale del fetch giornaliero dell'immagine cinema. Il palinsesto cambia
 * a giorni, quindi la cadenza e' giornaliera e non periodica; il server
 * mantiene comunque una cache di un'ora. Tenuta separata da
 * WIFI_ACTIVE_HOUR_START cosi' i due possono spostarsi indipendentemente,
 * purche' l'ora del fetch resti dentro la fascia.
 */
#ifndef CINEMA_DAILY_FETCH_HOUR
  #define CINEMA_DAILY_FETCH_HOUR 7
#endif

/** Tetto comune alle cadenze: oltre le 24 h il confronto signed su millis()
 *  perde significato e una cadenza piu' lunga non ha senso in questo firmware. */
#define CADENZA_MAX_MIN 1440

// ---------------------------------------------------------------------------
// 3. Timeout, ritenti e budget (non configurabili)
// ---------------------------------------------------------------------------

/** Distanza minima fra due tentativi dello stesso slot. E' cio' che impedisce
 *  alla finestra di manutenzione, dove il loop gira ogni ~10 ms, di ripetere
 *  cento volte al secondo un tentativo appena fallito. */
#define FETCH_RITENTO_MS 30000UL

/** Tentativi consecutivi falliti oltre i quali un task consuma lo slot e
 *  attende la cadenza piena. 0 significa politica pessimista: lo slot e'
 *  speso prima ancora di eseguire. */
#define FETCH_MAX_TENTATIVI 2

/** Nome con cui il backoff interno di Mail e Calendar conosce la stessa
 *  soglia. Sparisce quando quel backoff passa allo scheduler. */
#define MAX_CALENDAR_ATTEMPTS FETCH_MAX_TENTATIVI

/** Timeout di un singolo tentativo di connessione della STA e passo del
 *  polling di attesa. */
#define WIFI_CONNECT_TIMEOUT_MS 15000UL
#define WIFI_ATTESA_POLL_MS 100

/** Dopo un tentativo di connessione fallito nessun task di rete riprova
 *  prima di questo tempo: evita di pagare 15 s di attesa a ogni risveglio
 *  quando la rete non c'e'. */
#define WIFI_RITENTO_MS 300000UL

/** Tempo massimo dal boot entro cui il primo frame viene comunque disegnato,
 *  con i placeholder "--" al posto dei dati non ancora arrivati. */
#define PRIMO_FRAME_ATTESA_MS 15000UL

/** Timeout HTTP del download dell'immagine cinema: margine per il cold start
 *  del free tier render.com, misurato in una ventina di secondi. */
#define CINEMA_HTTP_TIMEOUT_MS 45000UL

/** Attesa massima della RISPOSTA al ping di sveglia del server cinema, non
 *  della connessione: l'handshake TLS ha il suo timeout separato, lasciato al
 *  default di HTTPClient, perche' senza handshake completo la richiesta non
 *  parte affatto. Va tenuto sotto 65535: setTimeout() prende un uint16_t. */
#define CINEMA_PREWARM_TIMEOUT_MS 1500

/** Budget wall-clock di UNA esecuzione del fetch mail, non una cadenza:
 *  limita quanto a lungo la sequenza token + list + batch puo' occupare la
 *  finestra radio. */
#define MAIL_FETCH_BUDGET_MS 10000UL

/** Persistenza dello stato di calibrazione BSEC in NVS. */
#define BSEC_STATE_SAVE_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL)

/** Margine con cui il risveglio per il poll BSEC cade DOPO il campione
 *  atteso, mai prima. */
#define BSEC_MARGINE_POLL_MS 1000UL


/** Distanza minima fra due esecuzioni di un task giornaliero. Evita il
 *  doppione quando l'orologio diventa valido a meta' giornata e l'ora target
 *  risulta gia' passata. */
#define GIORNALIERA_DISTANZA_MIN_MS (12UL * 60UL * 60UL * 1000UL)

/** Passo del loop durante la finestra di manutenzione: serve a
 *  WebServer::handleClient(), che non e' asincrono. */
#define MAINTENANCE_LOOP_DELAY_MS 10

/**
 * Orologio di sistema.
 *
 * TIME_VALID_EPOCH_MIN e' il pavimento oltre il quale time() si considera
 * sincronizzato: prima di SNTP restituisce l'uptime contato dal 1970, che
 * dopo 27,7 h di accensione diventa indistinguibile da un'ora reale.
 *
 * TIME_SYNC_TIMEOUT_MS non va abbassato sotto i 10 s: lwIP ritarda la prima
 * richiesta SNTP di alcuni secondi. TIME_SYNC_RETRY_MS distanzia i tentativi
 * falliti, cosi' la finestra di manutenzione, che gira ogni ~10 ms, non
 * riavvia SNTP a ogni giro.
 */
#define TIME_VALID_EPOCH_MIN 1700000000L
#define TIME_SYNC_TIMEOUT_MS 12000UL
#define TIME_SYNC_RETRY_MS 60000UL
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"

// ---------------------------------------------------------------------------
// Validazione dei default a compile-time
// ---------------------------------------------------------------------------

static_assert(DISPLAY_REFRESH_MIN >= DISPLAY_REFRESH_MIN_FLOOR &&
                  DISPLAY_REFRESH_MIN <= CADENZA_MAX_MIN,
              "DISPLAY_REFRESH_MIN fuori dai limiti");
static_assert(DISPLAY_REFRESH_MIN * 60 >= DISPLAY_REFRESH_PIENO_S,
              "due refresh piu' vicini della durata di un refresh si accavallerebbero");
static_assert(WEATHER_FORECAST_FETCH_MIN >= WEATHER_FORECAST_FETCH_MIN_FLOOR &&
                  WEATHER_FORECAST_FETCH_MIN <= CADENZA_MAX_MIN,
              "sotto il pavimento One Call restituisce gli stessi dati");
static_assert(CAL_OUTLOOK_FETCH_MIN >= CAL_OUTLOOK_FETCH_MIN_FLOOR &&
                  CAL_OUTLOOK_FETCH_MIN <= CADENZA_MAX_MIN,
              "CAL_OUTLOOK_FETCH_MIN fuori dai limiti");
static_assert(CAL_GOOGLE_FETCH_MIN >= CAL_GOOGLE_FETCH_MIN_FLOOR &&
                  CAL_GOOGLE_FETCH_MIN <= CADENZA_MAX_MIN,
              "CAL_GOOGLE_FETCH_MIN fuori dai limiti");
static_assert(MAIL_GOOGLE_FETCH_MIN >= MAIL_GOOGLE_FETCH_MIN_FLOOR &&
                  MAIL_GOOGLE_FETCH_MIN <= CADENZA_MAX_MIN,
              "MAIL_GOOGLE_FETCH_MIN fuori dai limiti");
static_assert(OTA_WINDOW_MIN >= OTA_WINDOW_MIN_FLOOR && OTA_WINDOW_MIN <= CADENZA_MAX_MIN,
              "OTA_WINDOW_MIN fuori dai limiti");
static_assert(FETCH_COALESCE_MIN >= FETCH_COALESCE_MIN_FLOOR,
              "FETCH_COALESCE_MIN negativo");
static_assert(FETCH_COALESCE_MIN < WEATHER_FORECAST_FETCH_MIN &&
                  FETCH_COALESCE_MIN < CAL_OUTLOOK_FETCH_MIN &&
                  FETCH_COALESCE_MIN < CAL_GOOGLE_FETCH_MIN &&
                  FETCH_COALESCE_MIN < MAIL_GOOGLE_FETCH_MIN,
              "coalescing >= cadenza: il task partirebbe a ogni accensione della radio");
static_assert(WIFI_ACTIVE_HOUR_START <= WIFI_ACTIVE_HOUR_END && WIFI_ACTIVE_HOUR_END <= 23,
              "fascia oraria WiFi non valida");
static_assert(CINEMA_DAILY_FETCH_HOUR >= WIFI_ACTIVE_HOUR_START &&
                  CINEMA_DAILY_FETCH_HOUR <= WIFI_ACTIVE_HOUR_END,
              "il fetch giornaliero cinema deve cadere dentro la fascia radio");
static_assert(SLEEP_MIN_S <= SLEEP_MAX_S, "pavimento di sleep sopra il tetto");
static_assert(SLEEP_MAX_S == BSEC_PERIODO_ULP_S,
              "il tetto dello sleep e' il periodo ULP: piu' alto perde campioni");
static_assert(FETCH_RITENTO_MS >= SLEEP_MIN_S * 1000UL,
              "un ritento piu' vicino del pavimento di sleep non e' onorabile");
static_assert(CINEMA_PREWARM_TIMEOUT_MS < 65535,
              "HTTPClient::setTimeout() prende un uint16_t");
static_assert(PRIMO_FRAME_ATTESA_MS < (uint32_t)OTA_WINDOW_MIN_FLOOR * 60UL * 1000UL,
              "l'attesa del primo frame deve scadere dentro la finestra di manutenzione, "
              "che e' l'unico momento in cui il loop gira senza dormire");

// ---------------------------------------------------------------------------
// Valori a runtime: default sopra, override persistiti in NVS
// ---------------------------------------------------------------------------

/**
 * Cadenze modificabili senza riflashare. I default sono i #define qui sopra;
 * gli override vivono in NVS e vengono riletti al boot, sempre clampati al
 * proprio pavimento.
 *
 * Lo scheduler legge get() a ogni valutazione e ricalcola le scadenze da
 * `ultimo + intervallo`, senza mai memorizzare una scadenza assoluta: cosi'
 * una modifica ha effetto dal giro successivo, senza riavvio.
 */
namespace Timings
{
  /** Tutti i campi uint16_t: rende la tabella dei descrittori omogenea e
   *  permette di indirizzarli con un puntatore a membro. */
  struct Valori
  {
    uint16_t displayRefreshMin;
    uint16_t owmMin;
    uint16_t outlookMin;
    uint16_t googleMin;
    uint16_t mailMin;
    uint16_t coalesceMin;
    uint16_t finestraManutenzioneMin;
    uint16_t wifiOraInizio;
    uint16_t wifiOraFine;
    uint16_t cinemaOra;
  };

  inline constexpr Valori PREDEFINITI = {
      DISPLAY_REFRESH_MIN,
      WEATHER_FORECAST_FETCH_MIN,
      CAL_OUTLOOK_FETCH_MIN,
      CAL_GOOGLE_FETCH_MIN,
      MAIL_GOOGLE_FETCH_MIN,
      FETCH_COALESCE_MIN,
      OTA_WINDOW_MIN,
      WIFI_ACTIVE_HOUR_START,
      WIFI_ACTIVE_HOUR_END,
      CINEMA_DAILY_FETCH_HOUR,
  };

  /**
   * Descrittore di un campo configurabile: quanto basta alla pagina web per
   * costruire il proprio form senza sapere niente dei singoli tempi.
   * `chiave` e' anche il nome NVS, quindi al massimo 15 caratteri.
   */
  struct Campo
  {
    const char* chiave;
    const char* descrizione;
    const char* unita;
    uint16_t Valori::*campo;
    uint16_t minimo;
    uint16_t massimo;
    uint16_t predefinito;
  };

  namespace detail
  {
    inline constexpr const char* NVS_NAMESPACE = "timings";
    inline constexpr const char* NVS_CHIAVE_SCHEMA = "schema";

    /** Si alza SOLO quando una chiave cambia significato o unita': in quel
     *  caso il namespace viene azzerato e si riparte dai default. */
    inline constexpr uint16_t SCHEMA = 1;

    inline Valori valori = PREDEFINITI;

    inline constexpr Campo CAMPI[] = {
        {"disp_min",   "Refresh display (minimo fra due ridisegni)", "min",
         &Valori::displayRefreshMin, DISPLAY_REFRESH_MIN_FLOOR, CADENZA_MAX_MIN, DISPLAY_REFRESH_MIN},
        {"owm_min",    "Meteo",                                     "min",
         &Valori::owmMin, WEATHER_FORECAST_FETCH_MIN_FLOOR, CADENZA_MAX_MIN, WEATHER_FORECAST_FETCH_MIN},
        {"outl_min",   "Calendario Outlook",                        "min",
         &Valori::outlookMin, CAL_OUTLOOK_FETCH_MIN_FLOOR, CADENZA_MAX_MIN, CAL_OUTLOOK_FETCH_MIN},
        {"goog_min",   "Calendario Google",                         "min",
         &Valori::googleMin, CAL_GOOGLE_FETCH_MIN_FLOOR, CADENZA_MAX_MIN, CAL_GOOGLE_FETCH_MIN},
        {"mail_min",   "Posta Gmail",                               "min",
         &Valori::mailMin, MAIL_GOOGLE_FETCH_MIN_FLOOR, CADENZA_MAX_MIN, MAIL_GOOGLE_FETCH_MIN},
        {"coal_min",   "Coalescing (anticipo a radio accesa)",      "min",
         &Valori::coalesceMin, FETCH_COALESCE_MIN_FLOOR, CADENZA_MAX_MIN, FETCH_COALESCE_MIN},
        {"maint_min",  "Finestra di manutenzione",                  "min",
         &Valori::finestraManutenzioneMin, OTA_WINDOW_MIN_FLOOR, CADENZA_MAX_MIN, OTA_WINDOW_MIN},
        {"wifi_h_ini", "Inizio fascia WiFi",                        "ora",
         &Valori::wifiOraInizio, 0, 23, WIFI_ACTIVE_HOUR_START},
        {"wifi_h_fin", "Fine fascia WiFi",                          "ora",
         &Valori::wifiOraFine, 0, 23, WIFI_ACTIVE_HOUR_END},
        {"cine_h",     "Ora del fetch cinema",                      "ora",
         &Valori::cinemaOra, 0, 23, CINEMA_DAILY_FETCH_HOUR},
    };

    inline constexpr size_t N_CAMPI = sizeof(CAMPI) / sizeof(CAMPI[0]);

    /** Lunghezza a compile-time: Preferences tronca in silenzio oltre i 15
     *  caratteri, quindi una chiave troppo lunga sarebbe un bug muto. */
    inline constexpr size_t lunghezza(const char* s) { return *s ? 1 + lunghezza(s + 1) : 0; }

    inline constexpr bool chiaviValide()
    {
      for (size_t i = 0; i < N_CAMPI; ++i)
        if (lunghezza(CAMPI[i].chiave) > 15) return false;
      return true;
    }
    static_assert(chiaviValide(), "una chiave NVS supera i 15 caratteri e verrebbe troncata");

    /**
     * Vincoli che legano piu' campi fra loro e che quindi non possono essere
     * espressi come minimo/massimo del singolo campo.
     * @return nullptr se `v` e' coerente, altrimenti il motivo del rifiuto.
     */
    inline const char* incoerenza(const Valori& v)
    {
      if (v.wifiOraInizio > v.wifiOraFine)
        return "l'inizio della fascia WiFi supera la fine";
      if (v.cinemaOra < v.wifiOraInizio || v.cinemaOra > v.wifiOraFine)
        return "l'ora del fetch cinema cade fuori dalla fascia WiFi";
      if (v.coalesceMin >= v.owmMin || v.coalesceMin >= v.outlookMin ||
          v.coalesceMin >= v.googleMin || v.coalesceMin >= v.mailMin)
        return "il coalescing e' maggiore o uguale a una cadenza di fetch";
      if ((uint32_t)v.displayRefreshMin * 60UL < DISPLAY_REFRESH_PIENO_S)
        return "il refresh del display e' piu' breve della durata di un refresh";
      return nullptr;
    }
  }

  /** Valori correnti. Riferimento stabile: chi lo tiene vede subito le
   *  modifiche fatte dalla pagina di configurazione. */
  inline const Valori& get() { return detail::valori; }

  /** Elenco dei campi configurabili, per la pagina web e per i log. */
  inline constexpr const Campo* campi() { return detail::CAMPI; }
  inline constexpr size_t nCampi() { return detail::N_CAMPI; }

  /** Conversione delle cadenze da minuti a millisecondi. */
  inline constexpr uint32_t ms(uint16_t minuti) { return (uint32_t)minuti * 60UL * 1000UL; }

  /**
   * Scrive in NVS i soli campi che differiscono da quanto gia' memorizzato:
   * riscrivere valori identici consumerebbe cicli di flash per niente.
   * @return false se il namespace NVS non e' apribile in scrittura.
   */
  inline bool save()
  {
    Preferences prefs;
    if (!prefs.begin(detail::NVS_NAMESPACE, false))
    {
      LOG("timings", "NVS non apribile in scrittura: valori attivi solo fino al riavvio");
      return false;
    }
    prefs.putUShort(detail::NVS_CHIAVE_SCHEMA, detail::SCHEMA);
    for (size_t i = 0; i < detail::N_CAMPI; ++i)
    {
      const Campo& c = detail::CAMPI[i];
      const uint16_t v = detail::valori.*(c.campo);
      if (prefs.getUShort(c.chiave, 0xFFFF) != v) prefs.putUShort(c.chiave, v);
    }
    prefs.end();
    return true;
  }

  /**
   * Carica gli override da NVS e li porta dentro i limiti. Da chiamare in
   * setup() PRIMA di qualunque modulo che legga una cadenza.
   *
   * Uno schema diverso da quello atteso azzera il namespace: e' il caso in cui
   * una chiave ha cambiato significato e i valori vecchi non sono piu'
   * interpretabili. Una chiave assente ricade sul proprio default, quindi
   * aggiungere un campo in un firmware successivo non richiede migrazione.
   *
   * Ogni valore corretto viene loggato e riscritto, cosi' la pagina di
   * configurazione mostra il valore realmente in uso e non uno fantasma.
   */
  inline void begin()
  {
    using namespace detail;
    valori = PREDEFINITI;

    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true))
    {
      LOG("timings", "NVS non disponibile: uso i valori predefiniti");
      return;
    }
    const uint16_t schema = prefs.getUShort(NVS_CHIAVE_SCHEMA, 0);
    if (schema != SCHEMA)
    {
      prefs.end();
      if (schema != 0)
      {
        LOG("timings", "schema %u diverso da %u: azzero gli override",
            (unsigned)schema, (unsigned)SCHEMA);
        Preferences p2;
        if (p2.begin(NVS_NAMESPACE, false)) { p2.clear(); p2.end(); }
      }
      save();
      return;
    }
    for (size_t i = 0; i < N_CAMPI; ++i)
    {
      const Campo& c = CAMPI[i];
      valori.*(c.campo) = prefs.getUShort(c.chiave, c.predefinito);
    }
    prefs.end();

    // Clamp per campo: un pavimento puo' essere stato alzato da un
    // aggiornamento firmware successivo alla scrittura del valore.
    uint8_t corretti = 0;
    for (size_t i = 0; i < N_CAMPI; ++i)
    {
      const Campo& c = CAMPI[i];
      const uint16_t letto = valori.*(c.campo);
      uint16_t v = letto;
      if (v < c.minimo) v = c.minimo;
      if (v > c.massimo) v = c.massimo;
      if (v != letto)
      {
        LOG("timings", "%s=%u fuori dai limiti [%u..%u]: uso %u",
            c.chiave, (unsigned)letto, (unsigned)c.minimo, (unsigned)c.massimo, (unsigned)v);
        valori.*(c.campo) = v;
        ++corretti;
      }
    }
    // I vincoli incrociati non sono clampabili campo per campo: se la
    // combinazione non regge si torna interamente ai default, che per
    // costruzione sono coerenti (lo garantiscono le static_assert).
    const char* motivo = incoerenza(valori);
    if (motivo)
    {
      LOG("timings", "combinazione incoerente (%s): torno ai valori predefiniti", motivo);
      valori = PREDEFINITI;
      ++corretti;
    }
    if (corretti) save();
    LOG("timings", "%u cadenze caricate, %u corrette", (unsigned)N_CAMPI, (unsigned)corretti);
  }

  /**
   * Applica in blocco una nuova configurazione, senza clamp: se anche un solo
   * valore e' fuori dai limiti, o se la combinazione e' incoerente, non
   * cambia niente.
   *
   * E' in blocco e non campo per campo perche' i vincoli legano piu' valori
   * fra loro: allargare la fascia WiFi e spostare l'ora del cinema sono
   * legittimi insieme e illegittimi presi uno alla volta, e applicarli in
   * sequenza rifiuterebbe una richiesta valida a seconda dell'ordine.
   *
   * Il clamp esiste solo al boot, dove serve a recuperare valori diventati
   * illegali; qui chi scrive e' una persona e merita di sapere che il valore
   * non e' stato accettato.
   *
   * Non salva: il chiamante decide quando chiamare save().
   * @param motivo se non nullptr, riceve la ragione del rifiuto.
   */
  inline bool applica(const Valori& nuovi, const char** motivo = nullptr)
  {
    using namespace detail;
    for (size_t i = 0; i < N_CAMPI; ++i)
    {
      const Campo& c = CAMPI[i];
      const uint16_t v = nuovi.*(c.campo);
      if (v < c.minimo || v > c.massimo)
      {
        if (motivo) *motivo = "valore fuori dai limiti";
        return false;
      }
    }
    const char* err = incoerenza(nuovi);
    if (err)
    {
      if (motivo) *motivo = err;
      return false;
    }
    valori = nuovi;
    return true;
  }

  /** Cancella gli override e torna ai valori compilati nel firmware. */
  inline bool ripristinaDefault()
  {
    detail::valori = PREDEFINITI;
    Preferences prefs;
    if (!prefs.begin(detail::NVS_NAMESPACE, false)) return false;
    prefs.clear();
    prefs.putUShort(detail::NVS_CHIAVE_SCHEMA, detail::SCHEMA);
    prefs.end();
    LOG("timings", "override cancellati, valori predefiniti ripristinati");
    return true;
  }
}

#endif // TIMINGS_H

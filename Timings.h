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

/**
 * Ciclo del dispositivo: un'interrogazione del sensore, i fetch dovuti e un
 * frame. Coincide con il periodo ULP perchè BSEC è l'unico orologio che non si
 * può anticipare nè ritardare oltre il 6,25% (guida Bosch AN011), quindi tutto
 * il resto si conta in cicli. Le cadenze configurabili sono multipli di
 * CYCLE_MIN, che deve essere un numero intero di minuti.
 */
#define CYCLE_S BSEC_PERIODO_ULP_S
#define CYCLE_MIN (CYCLE_S / 60)

/** Durata misurata di un refresh pieno del pannello. Il ciclo non può
 *  essere più breve, perchè a ogni ciclo si disegna un frame. */
#define DISPLAY_REFRESH_PIENO_S 24

/** Tetto di sicurezza di un light sleep. Con il sensore attivo il sonno dura
 *  fino alla sua scadenza esatta, poco oltre un ciclo; il tetto serve solo
 *  alla notte senza sensore, che dorme fino all'apertura della fascia WiFi. */
#define SLEEP_MAX_S (24UL * 60UL * 60UL)

// ---------------------------------------------------------------------------
// 2. Cadenze configurabili (minuti, salvo dove indicato)
//
// Ogni cadenza di un task di rete è un multiplo di CYCLE_MIN: il task gira
// in un ciclo sì e in (cadenza / CYCLE_MIN - 1) no, sempre prima del frame
// dello stesso ciclo, quindi il dato compare appena prodotto. Il pannello si
// ridisegna a ogni ciclo della fascia WiFi e non ha una cadenza propria.
// ---------------------------------------------------------------------------

/** Meteo. Pavimento pari alla cadenza con cui One Call aggiorna i dati:
 *  sotto i 10 min si riscarica lo stesso contenuto. Il limite duro e' invece
 *  la quota del piano (1000 chiamate/giorno = una ogni 1,44 min). */
#ifndef WEATHER_FORECAST_FETCH_MIN
  #define WEATHER_FORECAST_FETCH_MIN 10
#endif
#define WEATHER_FORECAST_FETCH_MIN_FLOOR 10

/** Calendari e posta: nessun pavimento sui dati (un evento o una mail possono
 *  arrivare in qualsiasi momento), ma sotto un ciclo un fetch non verrebbe mai
 *  mostrato prima del successivo. Il costo è energetico, ~5-10 s di radio
 *  accesa per fetch. */
#ifndef CAL_OUTLOOK_FETCH_MIN
  #define CAL_OUTLOOK_FETCH_MIN 10
#endif
#define CAL_OUTLOOK_FETCH_MIN_FLOOR CYCLE_MIN

#ifndef CAL_GOOGLE_FETCH_MIN
  #define CAL_GOOGLE_FETCH_MIN 10
#endif
#define CAL_GOOGLE_FETCH_MIN_FLOOR CYCLE_MIN

#ifndef MAIL_GOOGLE_FETCH_MIN
  #define MAIL_GOOGLE_FETCH_MIN 10
#endif
#define MAIL_GOOGLE_FETCH_MIN_FLOOR CYCLE_MIN

/** Telemetria Tuya. Il pavimento e' il periodo di campionamento del BME680:
 *  sotto quello si ripubblica lo stesso campione, perche' la cache di Indoor
 *  non ne ha uno nuovo da dare. */
#ifndef TUYA_PUBLISH_MIN
  #define TUYA_PUBLISH_MIN 5
#endif
#define TUYA_PUBLISH_MIN_FLOOR CYCLE_MIN

/** Percorso del mattino da routefinder, solo dentro la fascia percorso: a
 *  ogni ciclo, così ogni frame della fascia ha il traffico ricalcolato. Il
 *  pavimento è il ciclo, che tiene le chiamate alla Routes API a 12 l'ora. */
#ifndef ROUTE_FETCH_MIN
  #define ROUTE_FETCH_MIN 5
#endif
#define ROUTE_FETCH_MIN_FLOOR CYCLE_MIN

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

/** Fascia oraria locale in cui si accende la radio e si disegna il pannello.
 *  Fuori, cioè di notte, restano solo i risvegli del sensore. */
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

/** Tentativi consecutivi falliti oltre i quali un task consuma lo slot e
 *  attende la cadenza piena; un tentativo per ciclo, quindi il ritento cade
 *  al ciclo successivo. 0 significa politica pessimista: lo slot è speso
 *  prima ancora di eseguire. */
#define FETCH_MAX_TENTATIVI 2

/** Timeout di un singolo tentativo di connessione della STA e passo del
 *  polling di attesa. */
#define WIFI_CONNECT_TIMEOUT_MS 15000UL
#define WIFI_ATTESA_POLL_MS 100

/** Parte di WIFI_CONNECT_TIMEOUT_MS concessa al tentativo veloce sul canale e
 *  sul BSSID dell'ultima connessione: se il router ha cambiato canale si
 *  ripiega sulla scansione completa nel tempo che resta. */
#define WIFI_FAST_CONNECT_TIMEOUT_MS 5000UL

/** Tetto dell'handshake TLS di ogni connessione HTTPS, in secondi. Il default
 *  del core è 120 s, e con i produttori in sequenza nello stesso loop un server
 *  che accetta la connessione senza completare la negoziazione fermerebbe tutti
 *  gli altri. A 80 MHz un handshake reale costa da 1 a 4 s. */
#define TLS_HANDSHAKE_TIMEOUT_S 15

/** Tempo massimo dal boot entro cui il primo frame viene comunque disegnato,
 *  con i placeholder "--" al posto dei dati non ancora arrivati. Nella
 *  finestra di manutenzione è anche l'attesa massima dell'ora valida prima
 *  che i fetch partano comunque. */
#define PRIMO_FRAME_ATTESA_MS 15000UL

/** Timeout HTTP del download dell'immagine cinema: margine per il cold start
 *  del free tier render.com, misurato in una ventina di secondi. */
#define CINEMA_HTTP_TIMEOUT_MS 45000UL

/** Timeout HTTP del download del percorso: stesso free tier render.com del
 *  cinema, più la latenza della Routes API in TRAFFIC_AWARE_OPTIMAL. */
#define ROUTE_HTTP_TIMEOUT_MS 45000UL

/** Età oltre la quale un percorso non si mostra, in multipli della cadenza con
 *  la sorgente Automatica e in secondi con la sorgente forzata a Percorso: un
 *  tempo stimato vecchio è un'informazione sbagliata, e i termini Google non
 *  consentono di conservare a lungo i contenuti della mappa. */
#define ROUTE_MAX_AGE_CADENCES 3
#define ROUTE_FORCED_MAX_AGE_S 3600L

/** Attesa massima della RISPOSTA al ping di sveglia del server cinema, non
 *  della connessione: l'handshake ha il suo tetto, TLS_HANDSHAKE_TIMEOUT_S,
 *  perchè senza handshake completo la richiesta non parte affatto. Va tenuto
 *  sotto 65535: setTimeout() prende un uint16_t. */
#define CINEMA_PREWARM_TIMEOUT_MS 1500

/** Inattività dopo la quale render.com spegne un servizio del free tier: il
 *  ping di sveglia si manda solo se l'ultima risposta di quel server è più
 *  vecchia di così, cioè quando l'istanza dorme davvero. */
#define RENDER_SPINDOWN_MS (15UL * 60UL * 1000UL)

/** Telemetria Tuya: attesa del CONNACK, che comprende l'handshake TLS, e del
 *  PUBACK in QoS 1, che vale anche come attesa del resto di un pacchetto
 *  iniziato. Il terzo e' l'attesa della conferma applicativa del cloud e vale
 *  solo con TUYA_REQUEST_ACK alzato, quindi a regime non si paga. */
#define TUYA_CONNECT_TIMEOUT_MS 8000UL
#define TUYA_PUBACK_TIMEOUT_MS 3000UL
#define TUYA_ACK_TIMEOUT_MS 2000UL

/** Keepalive dichiarato nel CONNECT verso Tuya, in secondi. La connessione
 *  resta aperta e muta fino allo spegnimento della radio: il dispositivo non
 *  manda mai un PINGREQ nè un DISCONNECT, e il broker lo considera online per
 *  1,5 volte questo valore dall'ultimo traffico. 1200 è il massimo ammesso
 *  (30..1200), quindi la telemetria resta online per 30 minuti dopo ogni
 *  publish. */
#define TUYA_KEEPALIVE_S 1200

/** Budget wall-clock di UNA esecuzione del fetch mail, non una cadenza:
 *  limita quanto a lungo la sequenza token + list + batch puo' occupare la
 *  finestra radio. */
#define MAIL_FETCH_BUDGET_MS 10000UL

/** Persistenza dello stato di calibrazione BSEC in NVS. */
#define BSEC_STATE_SAVE_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL)

/** Margine con cui il risveglio per il poll BSEC cade DOPO il campione
 *  atteso, mai prima. */
#define BSEC_MARGINE_POLL_MS 1000UL

/** Età oltre la quale il campione del sensore non si mostra nè si pubblica:
 *  un campione perso è tollerato, al secondo il dato vale come assente. */
#define INDOOR_SAMPLE_MAX_AGE_MS (2UL * BSEC_PERIODO_ULP_S * 1000UL + BSEC_MARGINE_POLL_MS)

/** Età oltre la quale il meteo in cache non si mostra più: con OpenWeather in
 *  errore il banner passa ai placeholder invece di esporre dati vecchi. */
#define WEATHER_MAX_AGE_S 3600L

/** Distanza minima fra due risincronizzazioni SNTP forzate all'apertura della
 *  radio. L'RTC in light sleep gira sull'oscillatore RC interno, che anticipa
 *  di qualche secondo all'ora; il polling di lwIP ogni 3 h riesce solo se cade
 *  in una finestra radio. */
#define CLOCK_RESYNC_INTERVAL_MS (60UL * 60UL * 1000UL)

/** Tetto di un singolo light sleep durante l'attesa del BUSY del pannello: il
 *  risveglio vero è il fronte del BUSY, il timer è solo una guardia. */
#define DISPLAY_BUSY_SLEEP_SLICE_MS 5000UL

/**
 * Politica del refresh differenziale (PartialRefresh.h). Un frame è partial solo
 * se il rosso non è cambiato; questi valori dicono quando farlo pieno comunque.
 *
 * PARTIAL_CHAIN_MAX: partial consecutivi al massimo, poi un pieno che azzera il
 * grigio che il partial lascia dove toglie il nero. La letteratura dice uno
 * ogni 5-10 passate; il tetto pesa poco sulla quota di partial e molto sul
 * grigio, quindi si scende a 3-4 se a vista disturba.
 *
 * PARTIAL_MIN_TEMP_C e PARTIAL_TEMP_HYSTERESIS_C: sotto la soglia si fa un pieno,
 * perchè la LUT del partial ha frame fissi e non compensa il freddo come la
 * waveform dell'OTP. Non è un valore misurato: il BME680 non misura il vetro e il
 * sensore interno del controller ha ±2 °C, quindi la soglia è prudente. Dopo un
 * pieno per freddo il partial riparte solo sopra soglia più isteresi. Senza
 * BME680 collegato la soglia non si applica e il partial si prova comunque.
 *
 * PARTIAL_MAX_GAP_MS: oltre questa distanza dall'ultimo frame il riferimento non
 * si usa (notte, riavvio, finestra di manutenzione). Sono due cicli più il margine
 * del poll, in millisecondi: un frame saltato non fa perdere il riferimento.
 */
#define PARTIAL_CHAIN_MAX 6
#define PARTIAL_MIN_TEMP_C 18.0f
#define PARTIAL_TEMP_HYSTERESIS_C 2.0f
#define PARTIAL_MAX_GAP_MS (2UL * CYCLE_S * 1000UL + BSEC_MARGINE_POLL_MS)

/** Nella finestra di manutenzione lo scheduler aspetta finchè l'ultima
 *  richiesta HTTP servita è più recente di così, per non bloccare il web
 *  server mentre qualcuno usa le pagine. */
#define MAINT_CLIENT_IDLE_MS 30000UL


/** Distanza minima fra due esecuzioni di un task giornaliero. Evita il
 *  doppione quando l'orologio diventa valido a meta' giornata e l'ora target
 *  risulta gia' passata. */
#define GIORNALIERA_DISTANZA_MIN_MS (12UL * 60UL * 60UL * 1000UL)

/** Passo del loop durante la finestra di manutenzione: serve a
 *  WebServer::handleClient(), che non e' asincrono. */
#define MAINTENANCE_LOOP_DELAY_MS 10

/** Distanza minima fra due giri dello scheduler nella finestra di
 *  manutenzione: il loop gira ogni MAINTENANCE_LOOP_DELAY_MS per il web
 *  server, mentre nessun task ha una granularità più fine del secondo. */
#define MAINTENANCE_SCHEDULER_PERIOD_MS 1000UL

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

static_assert(CYCLE_S % 60 == 0 && CYCLE_MIN > 0,
              "il ciclo deve essere un numero intero di minuti: le cadenze ne sono multipli");
static_assert(CYCLE_S >= DISPLAY_REFRESH_PIENO_S,
              "a ogni ciclo si disegna un frame: il ciclo non può durare meno di un refresh");
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
static_assert(TUYA_PUBLISH_MIN >= TUYA_PUBLISH_MIN_FLOOR && TUYA_PUBLISH_MIN <= CADENZA_MAX_MIN,
              "sotto il pavimento il publish Tuya ripubblica lo stesso campione BSEC");
static_assert(TUYA_KEEPALIVE_S >= 30 && TUYA_KEEPALIVE_S <= 1200,
              "Tuya ammette un keepalive da 30 a 1200 secondi");
static_assert((unsigned long)TUYA_PUBLISH_MIN * 60UL < (unsigned long)TUYA_KEEPALIVE_S * 3UL / 2UL,
              "con questa cadenza il broker Tuya vedrebbe il dispositivo offline fra un publish e il successivo");
static_assert(ROUTE_FETCH_MIN >= ROUTE_FETCH_MIN_FLOOR && ROUTE_FETCH_MIN <= CADENZA_MAX_MIN,
              "ROUTE_FETCH_MIN fuori dai limiti");
static_assert(OTA_WINDOW_MIN >= OTA_WINDOW_MIN_FLOOR && OTA_WINDOW_MIN <= CADENZA_MAX_MIN,
              "OTA_WINDOW_MIN fuori dai limiti");
static_assert(WIFI_ACTIVE_HOUR_START <= WIFI_ACTIVE_HOUR_END && WIFI_ACTIVE_HOUR_END <= 23,
              "fascia oraria WiFi non valida");
static_assert(CINEMA_DAILY_FETCH_HOUR >= WIFI_ACTIVE_HOUR_START &&
                  CINEMA_DAILY_FETCH_HOUR <= WIFI_ACTIVE_HOUR_END,
              "il fetch giornaliero cinema deve cadere dentro la fascia radio");
static_assert(SLEEP_MAX_S * 1000ULL >= CYCLE_S * 1000ULL + BSEC_MARGINE_POLL_MS,
              "il tetto dello sleep non deve tagliare l'attesa del sensore: un poll in ritardo "
              "oltre il 6,25% del periodo ULP blocca l'accuratezza di BSEC");
static_assert(BSEC_MARGINE_POLL_MS * 16UL < CYCLE_S * 1000UL,
              "il margine del poll BSEC deve restare ben dentro il 6,25% del periodo");
static_assert(WIFI_FAST_CONNECT_TIMEOUT_MS < WIFI_CONNECT_TIMEOUT_MS,
              "il tentativo veloce deve lasciare tempo al ripiego sulla scansione completa");
static_assert(CINEMA_PREWARM_TIMEOUT_MS < 65535,
              "HTTPClient::setTimeout() prende un uint16_t");
static_assert(PARTIAL_CHAIN_MAX >= 1 && PARTIAL_CHAIN_MAX <= 15,
              "la catena di partial va da 1 a 15: la letteratura dice 5-10 e oltre il grigio si accumula");
static_assert(PARTIAL_MIN_TEMP_C > 0.0f && PARTIAL_MIN_TEMP_C < 40.0f && PARTIAL_TEMP_HYSTERESIS_C >= 0.0f,
              "soglia di temperatura del partial fuori da un intervallo sensato");
static_assert(PARTIAL_MAX_GAP_MS > CYCLE_S * 1000UL + BSEC_MARGINE_POLL_MS,
              "il gap massimo del partial deve superare un ciclo più il margine del poll: "
              "CYCLE_S è in secondi e BSEC_MARGINE_POLL_MS in millisecondi, vanno convertiti");
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
 * Lo scheduler legge get() a ogni valutazione e ricalcola le scadenze dal
 * ciclo dello slot precedente, senza mai memorizzare una scadenza assoluta:
 * così una modifica ha effetto dal ciclo successivo, senza riavvio.
 */
namespace Timings
{
  /** Tutti i campi uint16_t: rende la tabella dei descrittori omogenea e
   *  permette di indirizzarli con un puntatore a membro. */
  struct Valori
  {
    uint16_t owmMin;
    uint16_t outlookMin;
    uint16_t googleMin;
    uint16_t mailMin;
    uint16_t tuyaMin;
    uint16_t finestraManutenzioneMin;
    uint16_t wifiOraInizio;
    uint16_t wifiOraFine;
    uint16_t cinemaOra;
    uint16_t routeMin;
  };

  inline constexpr Valori PREDEFINITI = {
      WEATHER_FORECAST_FETCH_MIN,
      CAL_OUTLOOK_FETCH_MIN,
      CAL_GOOGLE_FETCH_MIN,
      MAIL_GOOGLE_FETCH_MIN,
      TUYA_PUBLISH_MIN,
      OTA_WINDOW_MIN,
      WIFI_ACTIVE_HOUR_START,
      WIFI_ACTIVE_HOUR_END,
      CINEMA_DAILY_FETCH_HOUR,
      ROUTE_FETCH_MIN,
  };

  /**
   * Descrittore di un campo configurabile: quanto basta alla pagina web per
   * costruire il proprio form senza sapere niente dei singoli tempi.
   * `chiave` è anche il nome NVS, quindi al massimo 15 caratteri. `step` è
   * il passo dei valori ammessi: CYCLE_MIN per le cadenze dei task, che lo
   * scheduler conta in cicli, 1 per ore e durate. Tutta la validazione del
   * campo poggia su minimo, massimo e step, senza controlli scritti a mano.
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
    uint16_t step;
  };

  namespace detail
  {
    inline constexpr const char* NVS_NAMESPACE = "timings";
    inline constexpr const char* NVS_CHIAVE_SCHEMA = "schema";

    /** Si alza SOLO quando una chiave cambia significato o unita': in quel
     *  caso il namespace viene azzerato e si riparte dai default. A 2 le
     *  cadenze sono multipli del ciclo e non hanno più coalescing e
     *  intervallo del display. */
    inline constexpr uint16_t SCHEMA = 2;

    inline Valori valori = PREDEFINITI;

    inline constexpr Campo CAMPI[] = {
        {"owm_min",    "Meteo",                                     "min",
         &Valori::owmMin, WEATHER_FORECAST_FETCH_MIN_FLOOR, CADENZA_MAX_MIN, WEATHER_FORECAST_FETCH_MIN, CYCLE_MIN},
        {"outl_min",   "Calendario Outlook",                        "min",
         &Valori::outlookMin, CAL_OUTLOOK_FETCH_MIN_FLOOR, CADENZA_MAX_MIN, CAL_OUTLOOK_FETCH_MIN, CYCLE_MIN},
        {"goog_min",   "Calendario Google",                         "min",
         &Valori::googleMin, CAL_GOOGLE_FETCH_MIN_FLOOR, CADENZA_MAX_MIN, CAL_GOOGLE_FETCH_MIN, CYCLE_MIN},
        {"mail_min",   "Posta Gmail",                               "min",
         &Valori::mailMin, MAIL_GOOGLE_FETCH_MIN_FLOOR, CADENZA_MAX_MIN, MAIL_GOOGLE_FETCH_MIN, CYCLE_MIN},
        {"tuya_min",   "Telemetria Tuya",                           "min",
         &Valori::tuyaMin, TUYA_PUBLISH_MIN_FLOOR, CADENZA_MAX_MIN, TUYA_PUBLISH_MIN, CYCLE_MIN},
        {"maint_min",  "Finestra di manutenzione",                  "min",
         &Valori::finestraManutenzioneMin, OTA_WINDOW_MIN_FLOOR, CADENZA_MAX_MIN, OTA_WINDOW_MIN, 1},
        {"wifi_h_ini", "Inizio fascia WiFi",                        "ora",
         &Valori::wifiOraInizio, 0, 23, WIFI_ACTIVE_HOUR_START, 1},
        {"wifi_h_fin", "Fine fascia WiFi",                          "ora",
         &Valori::wifiOraFine, 0, 23, WIFI_ACTIVE_HOUR_END, 1},
        {"cine_h",     "Ora del fetch cinema (in fascia percorso slitta alla sua fine)", "ora",
         &Valori::cinemaOra, 0, 23, CINEMA_DAILY_FETCH_HOUR, 1},
        {"route_min",  "Percorso, dentro la fascia percorso",       "min",
         &Valori::routeMin, ROUTE_FETCH_MIN_FLOOR, CADENZA_MAX_MIN, ROUTE_FETCH_MIN, CYCLE_MIN},
    };

    inline constexpr size_t N_CAMPI = sizeof(CAMPI) / sizeof(CAMPI[0]);

    /** Vero se ogni campo ha un passo positivo e default, pavimento e tetto
     *  multipli del passo: un default fuori passo sarebbe rifiutato dalla
     *  stessa pagina che lo mostra. */
    inline constexpr bool passiValidi()
    {
      for (size_t i = 0; i < N_CAMPI; ++i)
      {
        const Campo& c = CAMPI[i];
        if (c.step == 0) return false;
        if (c.minimo % c.step || c.massimo % c.step || c.predefinito % c.step) return false;
        if (c.predefinito < c.minimo || c.predefinito > c.massimo) return false;
      }
      return true;
    }
    static_assert(passiValidi(), "un campo di Timings ha passo nullo, o default/limiti fuori passo o fuori range");

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
      return nullptr;
    }

    /** Motivo del rifiuto di un singolo campo, nullptr se il valore è
     *  ammesso. Il testo nomina chiave e limiti, perchè la pagina lo mostra
     *  com'è a chi ha scritto il valore. */
    inline const char* campoNonValido(const Campo& c, uint16_t v)
    {
      static char motivo[96];
      if (v < c.minimo || v > c.massimo)
      {
        snprintf(motivo, sizeof motivo, "%s fuori dai limiti %u..%u %s",
                 c.chiave, (unsigned)c.minimo, (unsigned)c.massimo, c.unita);
        return motivo;
      }
      if (v % c.step)
      {
        snprintf(motivo, sizeof motivo, "%s deve essere multiplo di %u %s (un ciclo del sensore)",
                 c.chiave, (unsigned)c.step, c.unita);
        return motivo;
      }
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
    // aggiornamento firmware successivo alla scrittura del valore. Un valore
    // fuori passo sale al multiplo successivo, che resta entro il tetto
    // perchè anche il tetto è un multiplo del passo.
    uint8_t corretti = 0;
    for (size_t i = 0; i < N_CAMPI; ++i)
    {
      const Campo& c = CAMPI[i];
      const uint16_t letto = valori.*(c.campo);
      uint16_t v = letto;
      if (v < c.minimo) v = c.minimo;
      if (v > c.massimo) v = c.massimo;
      if (v % c.step) v = (uint16_t)(v + c.step - v % c.step);
      if (v != letto)
      {
        LOG("timings", "%s=%u fuori dai limiti [%u..%u] o dal passo %u: uso %u",
            c.chiave, (unsigned)letto, (unsigned)c.minimo, (unsigned)c.massimo,
            (unsigned)c.step, (unsigned)v);
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
      const char* err = campoNonValido(c, nuovi.*(c.campo));
      if (err)
      {
        if (motivo) *motivo = err;
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

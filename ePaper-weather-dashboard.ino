#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

/**
 * Necessario per Adafruit_GFX per disegnare testo e linee
 * Costo: ~15 KB di flash in piu'
 */
#define ENABLE_GxEPD2_GFX 1
/**
 * Il bus HSPI non lo abilita nessuna macro: GxEPD2 e i driver del submodule
 * prendono il bus dall'oggetto passato a selectSPI(), e non leggono simboli di
 * configurazione. Il remap sta quindi tutto in initDisplay(), in due chiamate:
 * hspi.begin(13, 12, 14, 15) e display.epd2.selectSPI(...).
 *
 * Ed è obbligatorio, non stilistico: la Waveshare E-Paper ESP32 Driver Board
 * scambia SCK e MOSI rispetto al default HSPI (SCK sul 13, MOSI sul 14), e il
 * 12 è un MISO fittizio perchè sul FPC a 24 pin del pannello la linea dati di
 * ritorno non esiste. È codice board-specific, quindi vive qui e non nei
 * Layout_*.h, che descrivono il pannello.
 */

/**
 * Selezione del pannello display: scommenta UNA SOLA delle due varianti
 * per scegliere driver, coordinate del layout e font.
 *   - DISPLAY_VARIANT_097C -> Layout_097c.h (SOLUM 9.7" 960w x 672h BWR)
 *   - DISPLAY_VARIANT_122C -> Layout_122c.h (SOLUM 12.2" 960w x 768h BWR)
 *
 * I due Layout_*.h definiscono lo stesso namespace `Layout` con gli stessi
 * simboli (Panel, pin, font, coord), quindi la logica applicativa nei
 * moduli (Weather/Calendar/Graphics/icons) non cambia. Il define va prima
 * di #include "Layout.h" o di qualunque header che lo includa transitivamente.
 */
#define DISPLAY_VARIANT_097C
//#define DISPLAY_VARIANT_122C

/**
 * REFRESH DIFFERENZIALE B/N. 1 = un frame è un partial da ~0,64 s quando il
 * piano rosso non è cambiato rispetto a quello sul vetro, altrimenti un refresh
 * pieno da ~24 s; 0 = ogni frame è un refresh pieno, come prima.
 *
 * Il driver 097c sa fare un partial in bianco e nero, ma un frame partial non
 * può nè aggiungere nè togliere rosso: il rosso già sul vetro resta fermo,
 * perchè la waveform non pilota i pixel il cui bit è uguale nelle due RAM
 * (misurato, sezione PARTIAL del driver). Per questo il firmware disegna il
 * frame come sempre e il driver, che riceve i due piani una pagina alla volta,
 * confronta il piano rosso con quello dell'ultimo frame pieno: se è identico
 * parte il partial, altrimenti il pannello non si tocca e il frame si rifà in
 * pieno (Weather::renderFrame()). Il meccanismo sta nella sezione REFRESH
 * DIFFERENZIALE del driver, la politica (catena massima di partial, soglia di
 * temperatura, distanza massima dall'ultimo frame) in PartialRefresh.h e
 * Timings.h.
 *
 * Gli elementi che cambiano a ogni ciclo, come la temperatura del riquadro
 * Interno, sono in nero perchè un testo rosso che si sposta impedirebbe il
 * partial. Il firmware non chiama le sei API opt-in del vecchio partial,
 * che lavorano fuori dal template: drawImagePartial(), drawImagePartialPart(),
 * refreshPartial(), writeImageToPrevious(), writeScreenBufferPrevious() e
 * setPartialLut().
 *
 * Prima di abilitarlo su un pannello nuovo conviene provare l'esempio
 * GxEPD2_SOLUM_ESL/examples/097c/differential_check, che verifica sul vetro le
 * assunzioni mai misurate (orientamento delle RAM, power off fra due passate,
 * sonno di minuti). A 0 il driver non calcola nessun CRC e il percorso di
 * scrittura resta quello di prima.
 */
#define DISPLAY_PARTIAL_REFRESH 1

/**
 * SALTO DEL REFRESH A FRAME INVARIATO. 1 = se il frame è identico a quello
 * già sul vetro il driver non lancia nessun refresh, né partial né pieno; 0 = ogni
 * frame va al pannello, come prima.
 *
 * Il confronto sta nel driver, nello stesso passaggio paged che manda i piani
 * al controller: un CRC32 concatenato per piano, nero e colore, contro quello
 * dell'ultimo frame effettivamente mandato al vetro. Confronta l'uscita del
 * disegno e non i dati che l'hanno prodotta, quindi nessun ingresso del disegno
 * si può dimenticare, e non costa un disegno in più. Vale per ogni driver che
 * lo dichiara, con o senza il differenziale, e per questo è indipendente da
 * DISPLAY_PARTIAL_REFRESH. Ogni frame saltato non occupa il pannello e non fa
 * crescere la catena dei partial. I valori del riquadro Interno si mostrano
 * arrotondati (Indoor::shown*()) proprio perchè le oscillazioni sotto la
 * risoluzione utile non cambino il frame.
 */
#define DISPLAY_SKIP_UNCHANGED 1

/**
 * Verbosita' dei log seriali (Log.h). 0 = nessun log, 1 = operativi,
 * 2 = 1 + dettaglio per elemento. Va dichiarata qui, prima degli include
 * dei moduli, cosi' che il fallback #ifndef di Log.h la raccolga.
 */
#define LOG_LEVEL 2

/**
 * Conferma applicativa del cloud Tuya, da alzare durante il bring-up e da
 * riabbassare a regime: il PUBACK dice che il broker ha ricevuto il messaggio,
 * non che il cloud ne abbia accettato il contenuto. Costo e meccanismo sono
 * documentati in Tuya.h, dove il valore ha anche il suo default.
 */
#define TUYA_REQUEST_ACK 1

// ---------------------------------------------------------------------------
// Cadenze, timeout e pavimenti stanno tutti in Timings.h, che va incluso PRIMA
// dei moduli perche' i loro #ifndef li raccolgano. Le cadenze sono anche
// modificabili a runtime dalla pagina di configurazione, con persistenza NVS:
// vedi Timings::begin(), chiamata per prima in setup().
// ---------------------------------------------------------------------------
#include "Timings.h"

/**
 * Origine del server cinema, unico punto in cui compare il dominio: l'URL
 * dell'immagine, che serve anche al ping di sveglia, lo compone
 * buildCinemaUrl() dalle misure del layout. Non e' un tempo, quindi non sta in
 * Timings.h; il timeout del ping invece si' (CINEMA_PREWARM_TIMEOUT_MS).
 */
#define CINEMA_ORIGIN      "https://cinema-epd.onrender.com"

/** Origine del servizio routefinder (submodule omonimo), con lo stesso ruolo
 *  di CINEMA_ORIGIN: l'URL dell'immagine lo compone buildRouteUrl(). */
#define ROUTEFINDER_ORIGIN      "https://routefinder-7w0a.onrender.com"

/**
 * Un server render.com del free tier, con l'ultimo contatto che decide se il
 * ping di sveglia serve (vedi prewarmServer()). Sta prima di ogni funzione del
 * file perchè il preprocessore di Arduino ne genera i prototipi in quel punto,
 * e quelli che lo nominano non compilerebbero.
 */
struct RenderServer
{
	const char *flow;
	bool contacted;          // falso finchè il server non ha risposto, o dopo RENDER_SPINDOWN_MS
	uint32_t lastContactMs;  // millis() dell'ultima risposta HTTP, del ping o del download
};

static RenderServer g_cinema_server = {"cinema", false, 0};
static RenderServer g_route_server = {"route", false, 0};

#include <GxEPD2_3C.h>
#include "Layout.h"   // dispatcher: include Layout_097c.h o Layout_122c.h in base al #define DISPLAY_VARIANT_*

/** Weather.h contiene logica, fetch OpenWeather One Call 3.0, cache,
 * rendering banner. Il .ino si limita ad accendere/spegnere il WiFi
 * al momento giusto e a chiamare Weather::render() per aggiornare il
 * display.
 *
 * include transitivamente Calendar.h e Indoor.h
 * => basta questo piu' Maintenance.h per avere tutte le API dei moduli */
#include "Weather.h"
#include "Maintenance.h"
// Telemetria BME680 verso il cloud Tuya (TuyaLink su MQTT). Non disegna nulla:
// il pannello la interroga solo per il badge di guasto del riquadro Indoor.
#include "Tuya.h"
#include "Env.h"
#include "Log.h"
#include "Clock.h"
#include "Net.h"
#include "Scheduler.h"
#include "CinemaStore.h"
#include "WallpaperStore.h"
#include "Routefinder.h"

SPIClass hspi(HSPI);

/**
 * Bluetooth permanentemente disattivo. Il firmware non lo usa, e queste due
 * funzioni, che il core dichiara sovrascrivibili (esp32-hal-bt.h), fanno sì che
 * initArduino() rilasci comunque la memoria del controller BT, ~36 KB: il
 * rilascio è irreversibile fino al riavvio, quindi il controller non può più
 * partire, anche se un domani una libreria BT entrasse nel link.
 */
extern "C" bool btClassicInUse(void) { return false; }
extern "C" bool bleInUse(void) { return false; }

/**
 * Istanza del display. Tipo del pannello (Layout::Panel), altezza della page
 * (Layout::PAGE_HEIGHT) e costruzione del driver (Layout::makePanel) arrivano
 * dal Layout selezionato: i costruttori dei driver hanno arità diversa fra
 * un pannello e l'altro e la factory tiene questa riga indipendente dal
 * pannello montato. Aggiungere un terzo display non tocca il .ino.
 *
 * I pin HSPI del bus (SCK/MISO/MOSI/SS = 13/12/14/15) restano qui perchè sono
 * board-specific (Waveshare ESP32 Driver Board), non display-specific.
 */
GxEPD2_3C<Layout::Panel, Layout::PAGE_HEIGHT> display(Layout::makePanel());

/**
 * Sorveglianza di hasFastPartialUpdate, che resta false con il differenziale
 * acceso o spento.
 *
 * GxEPD2_3C consulta il flag in un solo punto (GxEPD2_3C.h:355), in modalità
 * finestra parziale, e fa ripetere l'intero loop paged dopo il refresh,
 * riscrivendo i due piani senza rinfrescare e senza riallineare 0x26: lavoro
 * buttato. Il differenziale non lo usa, perchè passa da refresh(false) del loop
 * a finestra piena. L'assert resta come tripwire: vederlo a true vorrebbe dire
 * che qualcuno ha creduto che quel flag abiliti il partial.
 */
static_assert(!Layout::Panel::hasFastPartialUpdate,
			  "Il driver dichiara hasFastPartialUpdate = true, ma il partial di questo driver non passa da quel flag: "
			  "ripete il loop paged senza rinfrescare, lavoro buttato. Il refresh differenziale si abilita con "
			  "DISPLAY_PARTIAL_REFRESH. Rimettere false nel driver.");

/**
 * Inizializza il bus HSPI e il display in orientamento landscape fisso.
 * Chiamato una sola volta in setup().
 *
 * Contratto d'uso del driver per tutto il firmware, e qui è il posto giusto
 * perchè è dove si scelgono bus e modalità:
 *   - SOLO full-window. setFullWindow() vale per l'intera sessione, e
 *     Weather::renderFrame() la richiama a ogni frame come difesa. In
 *     partial-window il template chiamerebbe writeImagePart(black, color) e
 *     poi refresh(x, y, w, h), che sul driver 097c fa comunque un refresh
 *     pieno: nessun guadagno e una modalità in più da mantenere. Il partial
 *     passa invece dal refresh differenziale del loop paged a finestra piena
 *     (setDifferentialRefresh() qui sotto, il resto in Weather::renderFrame()).
 *   - le sei API del partial del driver NON vanno chiamate da qui:
 *     drawImagePartial(), drawImagePartialPart(), refreshPartial(),
 *     writeImageToPrevious(), writeScreenBufferPrevious() e setPartialLut(). Sono
 *     opt-in e lavorano fuori dal template, quindi basta non chiamarle; una
 *     scrittura estranea al loop paged spegnerebbe comunque il riferimento del
 *     differenziale.
 *   - il resto lo gestisce il driver da sè: init dei due piani alla prima
 *     scrittura, ricarica della waveform dall'OTP a ogni refresh pieno, e
 *     ripulitura della RAM al risveglio da hibernate(), che con un riferimento
 *     valido non manda il controller in deep sleep.
 */
/**
 * Attesa del BUSY del pannello in light sleep invece che a CPU accesa: un
 * refresh pieno dura ~24 s, ed è la voce più costosa del ciclo dopo la radio.
 * GxEPD2 la chiama in _waitWhileBusy() finchè il BUSY resta attivo e ricontrolla
 * il pin dopo ogni chiamata; il suo timeout resta valido perchè micros() avanza
 * anche durante il light sleep.
 *
 * Il risveglio è il BUSY che torna basso, con un timer di sicurezza. A radio
 * accesa, cioè nella finestra di manutenzione, resta l'attesa attiva: il light
 * sleep staccherebbe access point e web server.
 */
static void waitBusyAsleep(const void*)
{
	if (WiFi.getMode() != WIFI_OFF || Layout::BUSY_WAKE_PIN < 0)
	{
		delay(1);
		return;
	}
	const gpio_num_t busyPin = (gpio_num_t)Layout::BUSY_WAKE_PIN;
	gpio_wakeup_enable(busyPin, GPIO_INTR_LOW_LEVEL);
	esp_sleep_enable_gpio_wakeup();
	esp_sleep_enable_timer_wakeup((uint64_t)DISPLAY_BUSY_SLEEP_SLICE_MS * 1000ULL);
	Serial.flush();
	esp_light_sleep_start();
	// La sorgente GPIO va tolta: lasciata attiva, con il BUSY basso
	// risveglierebbe subito ogni light sleep dello scheduler.
	esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
	gpio_wakeup_disable(busyPin);
}

void initDisplay()
{
	hspi.begin(13, 12, 14, 15); // SCK, MISO, MOSI, SS (HSPI bus, board-specific)
	/**
	 * 10 MHz è il massimo raccomandato per i pannelli SSD1677 sul cablaggio
	 * della Waveshare ESP32 Driver Board senza adattamenti.
	 */
	display.epd2.selectSPI(hspi, SPISettings(10000000, MSBFIRST, SPI_MODE0));
	display.init(115200, true, 2, false);
	display.epd2.setBusyCallback(waitBusyAsleep);
	display.setRotation(Layout::ROTATION);
	display.setFullWindow();
	// Un driver senza il differenziale ignora la chiamata: resta sul refresh pieno.
	PartialRefresh::setEnabled(display.epd2, DISPLAY_PARTIAL_REFRESH != 0);
	// Indipendente dal differenziale; un driver senza l'API ignora la chiamata.
	PartialRefresh::setSkipUnchanged(display.epd2, DISPLAY_SKIP_UNCHANGED != 0);
}

// ===========================================================================
// Riquadro in alto a sinistra: percorso, cinema o wallpaper
//
// L'immagine occupa Layout::CINEMA_W x Layout::CINEMA_H a partire da
// (Layout::CINEMA_X, Layout::CINEMA_Y) e occupa la colonna sinistra dentro i
// margini fino al banner, con la scheda del calendario a destra. Le misure le
// deriva Layout_common.h. Convenzione: NwxMh = N px larghezza x M px altezza.
//
// Percorso e cinema si scaricano nelle loro fasce (Routefinder::scheduledImage)
// in un buffer in RAM, CinemaStore, che non consuma la flash e che un riavvio
// svuota; il wallpaper sta nella partizione in flash, WallpaperStore, e si
// carica dalla manutenzione. Un'immagine programmata assente o non più valida
// lascia il posto al wallpaper.
// ===========================================================================

/**
 * Le dimensioni dell'area sono in Layout::CINEMA_W (larghezza X) / CINEMA_H
 * (altezza Y) / CINEMA_STRIDE / CINEMA_PLANE_SZ / CINEMA_TOTAL_SZ, e gli URL
 * dei server si compongono da quelle: cambiare display regola da solo sia la
 * query string sia la misura dell'immagine attesa.
 *
 * GxEPDImage::showImage() non clippa e disegna dalla width/height del
 * Descriptor: drawTestBackground() taglia le righe all'altezza CINEMA_H.
 * Le immagini scaricate e il wallpaper caricato hanno sempre la misura esatta,
 * perchè download e upload rifiutano ogni altra lunghezza.
 */

static_assert(Layout::CINEMA_PLANES == 2,
			  "Layout::CINEMA_PLANES: il descrittore e l'URL del cinema prevedono black e red");

/**
 * Valore del parametro colors dell'endpoint /cinema/arduino per un numero di
 * piani: un piano solo e' bianco e nero, due sono black e red.
 */
static constexpr const char *cinemaColorsParam(uint8_t planes)
{
	return planes <= 1 ? "bw" : "bwr";
}

/**
 * Compone l'URL dell'immagine cinema dalle misure del layout, così larghezza,
 * altezza e piani chiesti al server non possono divergere dal buffer. Serve al
 * download e al ping di sveglia.
 * @return false se l'URL non entra in `size`: un troncamento manderebbe al
 *         server un'altra misura.
 */
static bool buildCinemaUrl(char *url, size_t size)
{
	const int length = snprintf(url, size,
		CINEMA_ORIGIN "/cinema/arduino?width=%d&height=%d&colors=%s&dither=floyd",
		(int)Layout::CINEMA_W, (int)Layout::CINEMA_H, cinemaColorsParam(Layout::CINEMA_PLANES));
	if (length < 0 || length >= (int)size)
	{
		LOG("cinema", "URL troncato (%d caratteri)", length);
		return false;
	}
	return true;
}

/**
 * Compone l'URL del percorso con partenza, arrivo e carburante di Routefinder e
 * la misura del riquadro. Serve al download e al ping di sveglia.
 * @return false se l'URL non entra in `size`.
 */
static bool buildRouteUrl(char *url, size_t size)
{
	const int length = Routefinder::buildUrl(url, size, ROUTEFINDER_ORIGIN,
		(uint16_t)Layout::CINEMA_W, (uint16_t)Layout::CINEMA_H, cinemaColorsParam(Layout::CINEMA_PLANES));
	if (length < 0 || length >= (int)size)
	{
		LOG("route", "URL troncato (%d caratteri)", length);
		return false;
	}
	return true;
}

/**
 * Vero se il server ha risposto da meno di RENDER_SPINDOWN_MS. Il flag si
 * azzera al primo controllo oltre la soglia: senza, dopo 24,8 giorni senza
 * contatti il confronto signed su millis() si invertirebbe.
 */
static bool isServerAwake(RenderServer &server)
{
	if (!server.contacted) return false;
	if ((int32_t)(millis() - server.lastContactMs) >= (int32_t)RENDER_SPINDOWN_MS)
	{
		server.contacted = false;
		return false;
	}
	return true;
}

/** Registra una risposta HTTP del server, cioè la prova che l'istanza è su. */
static void noteServerContact(RenderServer &server)
{
	server.contacted = true;
	server.lastContactMs = millis();
}

/**
 * Sveglia un'istanza render.com del free tier chiedendo l'immagine vera e
 * abbandonando la risposta. La usano i server del cinema e del percorso.
 *
 * Il free tier sospende il servizio dopo 15 min di inattività, lo risveglia
 * alla prima richiesta in circa un minuto e ne perde il file system locale,
 * cache delle locandine compresa (render.com/docs/free). Per questo il ping non
 * chiede /health, che sveglierebbe solo il processo, ma l'URL del download: il
 * server prepara l'immagine e la mette in cache mentre l'ESP32 fa gli altri
 * fetch, e il download vero la trova pronta. Sul percorso non costa una
 * chiamata in più alla Routes API, perchè routefinder tiene il calcolo in cache
 * per 60 s e il download arriva prima.
 *
 * Su TLS il fire-and-forget puro non esiste: la richiesta arriva al router di
 * render solo a handshake completo, quindi la connessione va portata su per
 * intero, con il tetto comune TLS_HANDSHAKE_TIMEOUT_S. Quello che si abbandona
 * è la RISPOSTA: con setTimeout() breve la GET ritorna appena scaduta l'attesa
 * degli header, end() chiude il socket e l'istanza prosegue per conto suo.
 *
 * Nessun valore di ritorno: il codice HTTP viene solo loggato.
 * HTTPC_ERROR_READ_TIMEOUT (-11) è l'esito nominale, 200 significa che il
 * server era già caldo, un errore di connessione che il ping non è partito.
 * Gira come onRoundStart() del task che scaricherà davvero l'immagine, e solo
 * se l'istanza dorme: un server che ha risposto da meno di RENDER_SPINDOWN_MS
 * è sveglio.
 */
static void prewarmServer(RenderServer &server, const char *url, const char *token)
{
	if (isServerAwake(server))
	{
		LOGV("img", "%s pre-warm non necessario: il server ha risposto da meno di %lu min",
			 server.flow, (unsigned long)(RENDER_SPINDOWN_MS / 60000UL));
		return;
	}
	WiFiClientSecure client;
	HTTPClient http;
	http.setTimeout(CINEMA_PREWARM_TIMEOUT_MS);
	if (!Net::beginHttps(http, client, url))
	{
		LOG("img", "%s pre-warm: http.begin fallita", server.flow);
		return;
	}
	if (token != nullptr) http.addHeader("X-Routefinder-Token", token);
	uint32_t t0 = millis();
	int code = http.GET();
	http.end();
	// Un codice HTTP positivo è una risposta dell'istanza; il timeout di
	// lettura (-11), esito nominale a istanza ferma, non lo è.
	if (code > 0) noteServerContact(server);
	LOG("img", "%s pre-warm -> %d in %lu ms, heap libero %lu", server.flow, code,
		(unsigned long)(millis() - t0), (unsigned long)ESP.getFreeHeap());
}

static void prewarmCinemaServer()
{
	char url[160];
	if (buildCinemaUrl(url, sizeof(url))) prewarmServer(g_cinema_server, url, nullptr);
}

static void prewarmRouteServer()
{
	char url[256];
	if (buildRouteUrl(url, sizeof(url))) prewarmServer(g_route_server, url, ROUTEFINDER_TOKEN);
}

/** Riga di log di un download fallito, con la durata e la memoria: è la
 *  diagnostica che separa un cold start lento da una rete giù o da un heap
 *  esaurito. */
static void logDownloadFailure(const char *flow, const char *what, long value, uint32_t t0)
{
	LOG("img", "%s: %s %ld dopo %lu ms, heap libero %lu, blocco max %lu", flow, what, value,
		(unsigned long)(millis() - t0), (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
}

/**
 * Scarica un'immagine nel formato di /cinema/arduino nel buffer in RAM di
 * CinemaStore; la usano cinema e percorso, che hanno la stessa misura
 * Layout::CINEMA_*. Cadenza e ritenti sono dello scheduler; qui si scarica e
 * basta, con la radio già connessa.
 *
 * Il buffer si apre solo dopo status 200 e Content-Length giusto, quindi un
 * errore prima del corpo lascia valida l'immagine precedente; un corpo
 * interrotto la perde, e il riquadro mostra il wallpaper fino al ritento.
 * Ogni risposta HTTP, anche un errore, conta come contatto con il server: è
 * ciò che evita il ping di sveglia al ciclo successivo.
 * @param server server render.com da cui si scarica, per il ping condizionato.
 * @param token valore dell'header X-Routefinder-Token, nullptr se non serve.
 * @return true se l'immagine è stata scaricata per intero.
 */
static bool downloadImageToStore(RenderServer &server, const char *url, const char *token,
								 uint32_t timeoutMs, CinemaStore::Kind kind)
{
	const char *flow = server.flow;
	LOG("img", "%s fetching %s", flow, url);
	// La sessione Tuya rimasta aperta nella finestra di manutenzione, dove la
	// radio non si spegne mai, occuperebbe un'altra sessione TLS accanto a
	// questa e all'immagine: si chiude qui, il prossimo publish la riapre.
	Tuya::releaseSession();

	// Client esplicito, dichiarato prima di HTTPClient perchè il distruttore di
	// http lo usa: con begin(url) il core ne creerebbe uno interno, senza modo
	// di limitarne l'handshake. I server rispondono con Content-Length anche in
	// HTTP/1.0, quindi il controllo sulla dimensione resta valido.
	WiFiClientSecure client;
	HTTPClient http;
	// Margine per lo spin-up del free tier render.com quando il ping non è
	// bastato. Vale anche come attesa massima senza dati durante il corpo.
	http.setTimeout(timeoutMs);
	const uint32_t t0 = millis();
	if (!Net::beginHttps(http, client, url))
	{
		LOG("img", "%s: HTTP begin fallita", flow);
		return false;
	}
	if (token != nullptr) http.addHeader("X-Routefinder-Token", token);
	int code = http.GET();
	if (code > 0) noteServerContact(server);
	if (code != 200)
	{
		logDownloadFailure(flow, "HTTP status", code, t0);
		http.end();
		return false;
	}
	int size = http.getSize();
	if (size != (int)Layout::CINEMA_TOTAL_SZ)
	{
		logDownloadFailure(flow, "Content-Length", size, t0);
		http.end();
		return false;
	}
	if (!CinemaStore::beginWrite(kind))
	{
		logDownloadFailure(flow, "buffer non allocabile, byte", (long)Layout::CINEMA_TOTAL_SZ, t0);
		http.end();
		return false;
	}

	// BodyStream attende le pause fra i blocchi del corpo: un blocco resta corto
	// solo a connessione chiusa, su errore TLS o dopo timeoutMs senza dati, il
	// timeout che http.setTimeout() ha dato allo stream.
	Net::BodyStream body(http.getStream());
	uint8_t chunk[1024];
	uint32_t received = 0;
	while (received < Layout::CINEMA_TOTAL_SZ)
	{
		const uint32_t remaining = Layout::CINEMA_TOTAL_SZ - received;
		const size_t wanted = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
		const size_t got = body.readBytes(chunk, wanted);
		if (got != wanted || !CinemaStore::writeChunk(chunk, got))
		{
			logDownloadFailure(flow, "corpo interrotto al byte", (long)(received + got), t0);
			http.end();
			CinemaStore::abortWrite();
			return false;
		}
		received += got;
	}
	http.end();

	// L'istante del download decide se l'immagine è ancora mostrabile; i task
	// che scaricano girano solo con l'orologio valido.
	if (!CinemaStore::commitWrite((uint32_t)time(nullptr)))
	{
		LOG("img", "%s: immagine incompleta, scartata", flow);
		return false;
	}
	LOG("img", "%s: download completato in %lu ms, heap libero %lu", flow,
		(unsigned long)(millis() - t0), (unsigned long)ESP.getFreeHeap());
	return true;
}

/**
 * Adattatori fra l'API dei moduli e il vocabolario dello scheduler.
 * Restano nel .ino perche' la tabella nomina anche cinema e percorso, che
 * sono fatti di statici di questo file.
 */
static Scheduler::Outcome runWeatherFetch()
{
	return Weather::runFetch() ? Scheduler::Outcome::OK : Scheduler::Outcome::FAILED;
}

/** Corrente senza previsioni non basta a disegnare il banner: lo scheduler
 *  ritenta invece di attendere la cadenza piena. */
static bool isWeatherIncomplete() { return !Weather::datiCompleti(); }

static Scheduler::Outcome runGoogleCalendarFetch()
{
	return Calendar::Google::runFetch() ? Scheduler::Outcome::OK : Scheduler::Outcome::FAILED;
}

static Scheduler::Outcome runOutlookCalendarFetch()
{
	return Calendar::Outlook::runFetch() ? Scheduler::Outcome::OK : Scheduler::Outcome::FAILED;
}

/**
 * Precondizione dei due fetch calendario: senza orologio le query partono da
 * 1970-01-01 e, ordinando per data, riportano i PRIMI eventi del calendario
 * invece dei prossimi. Una risposta cosi' e' HTTP 200, quindi lo scheduler la
 * registrerebbe come successo e chiuderebbe lo slot per la cadenza piena.
 * Meglio non partire: il task resta sospeso e diventa dovuto da se' appena
 * l'ora e' vera.
 */
static bool isClockValidForCalendars() { return Clock::valido(); }

/**
 * Precondizioni sulle credenziali di Env.h: un produttore con i valori del
 * template resta sospeso invece di spendere un handshake per un rifiuto certo
 * a ogni slot, e non accende la radio per sè. Gli altri proseguono come sempre.
 */
static bool weatherReady() { return Weather::isConfigured(); }
static bool googleReady() { return isClockValidForCalendars() && Calendar::Google::isConfigured(); }
static bool outlookReady() { return isClockValidForCalendars() && Calendar::Outlook::isConfigured(); }

static Scheduler::Outcome runCinemaFetch()
{
	char url[160];
	if (!buildCinemaUrl(url, sizeof(url))) return Scheduler::Outcome::FAILED;
	return downloadImageToStore(g_cinema_server, url, nullptr, CINEMA_HTTP_TIMEOUT_MS, CinemaStore::Kind::Cinema)
		? Scheduler::Outcome::OK : Scheduler::Outcome::FAILED;
}

/**
 * Il cinema si scarica solo quando è l'immagine programmata del riquadro, cioè
 * nella sua fascia o durante una sua forzatura, e con l'orologio valido, perchè
 * l'istante del download ne decide la validità. Il passaggio a vero apre uno
 * slot nuovo nello scheduler, quindi il primo download parte al primo ciclo
 * della fascia.
 */
static bool cinemaReady()
{
	return Routefinder::scheduledImage() == Routefinder::ShownImage::Cinema && Clock::valido();
}

static Scheduler::Outcome runRouteFetch()
{
	char url[256];
	if (!buildRouteUrl(url, sizeof(url))) return Scheduler::Outcome::FAILED;
	return downloadImageToStore(g_route_server, url, ROUTEFINDER_TOKEN, ROUTE_HTTP_TIMEOUT_MS, CinemaStore::Kind::Route)
		? Scheduler::Outcome::OK : Scheduler::Outcome::FAILED;
}

/**
 * Il percorso si scarica solo quando è l'immagine programmata, cioè nella
 * fascia percorso o durante una sua forzatura di un'ora: routefinder non riceve
 * mai richieste fuori da lì. Servono poi l'orologio, perchè l'istante del
 * download decide se l'immagine è ancora mostrabile, e arrivo e token configurati.
 */
static bool routeReady()
{
	return Routefinder::scheduledImage() == Routefinder::ShownImage::Route && Clock::valido() &&
		   Routefinder::hasDestination() && Net::isCredentialSet(ROUTEFINDER_TOKEN);
}

/** Contenuto del buffer in RAM nel vocabolario di Routefinder. */
static Routefinder::ShownImage storedImage()
{
	if (!CinemaStore::isValid()) return Routefinder::ShownImage::Wallpaper;
	return CinemaStore::kind() == CinemaStore::Kind::Route ? Routefinder::ShownImage::Route
														   : Routefinder::ShownImage::Cinema;
}

static Scheduler::Outcome runTuyaPublish()
{
	return Tuya::runPublish() ? Scheduler::Outcome::OK : Scheduler::Outcome::FAILED;
}

/** Senza credenziali, senza un campione del sensore o senza orologio non c'e'
 *  niente da pubblicare: il task resta sospeso, non accende la radio e non
 *  brucia il suo unico tentativo per cadenza. */
static bool tuyaReady() { return Tuya::readyToPublish(); }

/**
 * Tutti i flussi di rete del dispositivo. L'ordine delle righe è l'ordine di
 * esecuzione dentro il passo di rete del ciclo, che viene dopo il sensore e
 * prima del frame, e non è arbitrario:
 *
 *   - percorso e cinema dopo meteo e calendari, perchè sono gli unici che
 *     possono pagare lo spin-up di render.com, e il tempo di rete degli altri
 *     è la copertura di quel risveglio; i loro onRoundStart() mandano il ping
 *     all'inizio del passo, solo se l'istanza dorme. Non girano mai nello
 *     stesso ciclo, perchè le loro fasce non si sovrappongono;
 *   - la telemetria Tuya per ultima: la sua sessione TLS resta aperta fino allo
 *     spegnimento della radio, e aperta prima del download occuperebbe l'heap
 *     accanto all'immagine in RAM e alla sessione del download.
 *
 * Le cadenze sono multipli del ciclo (Timings.h, campo `step`): meteo e
 * calendari a 10 minuti cadono un ciclo sì e uno no, Tuya e percorso a ogni
 * ciclo, il cinema ogni ora. Nessun task chiede ridisegni: il frame si disegna
 * a ogni ciclo della fascia WiFi e legge da sè i dati, badge di Tuya compreso.
 *
 * Tuya è l'unico task con maxAttempts a 0, cioè un tentativo per slot comunque
 * vada: i suoi modi di fallimento sono permanenti sulla scala dei minuti
 * (credenziali, region, device model) e ogni ritento costerebbe un altro
 * handshake TLS senza cambiare l'esito.
 */
static Scheduler::Task TASK_TABLE[] = {
	{"meteo", &Timings::Valori::owmMin, FETCH_MAX_TENTATIVI,
	 runWeatherFetch, weatherReady, isWeatherIncomplete, nullptr},

	{"google", &Timings::Valori::googleMin, FETCH_MAX_TENTATIVI,
	 runGoogleCalendarFetch, googleReady, nullptr, nullptr},

	{"outlook", &Timings::Valori::outlookMin, FETCH_MAX_TENTATIVI,
	 runOutlookCalendarFetch, outlookReady, nullptr, nullptr},

	{"route", &Timings::Valori::routeMin, FETCH_MAX_TENTATIVI,
	 runRouteFetch, routeReady, nullptr, prewarmRouteServer},

	{"cinema", &Timings::Valori::cinemaMin, FETCH_MAX_TENTATIVI,
	 runCinemaFetch, cinemaReady, nullptr, prewarmCinemaServer},

	{"tuya", &Timings::Valori::tuyaMin, 0,
	 runTuyaPublish, tuyaReady, nullptr, nullptr},
};

static constexpr uint8_t TASK_COUNT = sizeof(TASK_TABLE) / sizeof(TASK_TABLE[0]);

/**
 * Invito a caricare il wallpaper, al centro del riquadro, quando la partizione
 * non ne contiene uno valido: l'indirizzo è l'ultimo con cui il dispositivo è
 * stato raggiungibile, perchè a radio spenta non ce n'è uno corrente.
 */
static void drawMissingWallpaperNotice()
{
	const int16_t centerY = Layout::CINEMA_Y + Layout::CINEMA_H / 2;
	if (!Graphics::intersectsPage(centerY - Layout::CINEMA_H / 4, Layout::CINEMA_H / 2)) return;
	char notice[64];
	const char *ip = Net::lastKnownIp();
	snprintf(notice, sizeof(notice), "No img. Check %s/wallpaper", ip[0] != '\0' ? ip : MAINT_HOSTNAME ".local");
	display.setFont(Layout::FONT_SMALL);
	Graphics::drawCentered(notice, Layout::CINEMA_X + Layout::CINEMA_W / 2, centerY, GxEPD_BLACK);
}

/**
 * Disegna, dentro il loop paged di Weather::renderFrame(), le sole righe del
 * riquadro che cadono nella pagina corrente: il percorso o il cinema in RAM
 * quando sono l'immagine programmata e ancora valida, altrimenti il wallpaper
 * della flash, e senza wallpaper l'invito a caricarlo. La scelta si rifà a ogni
 * frame, quindi un cambio di fascia, di forzatura o di validità compare al
 * ciclo successivo. Le righe si tagliano all'altezza CINEMA_H, e il
 * descrittore ridotto punta agli stessi dati, senza copie.
 */
void drawTestBackground()
{
	const Routefinder::ShownImage shown =
		Routefinder::shownImage(storedImage(), CinemaStore::downloadedAt(), CinemaStore::downloadedMs());
	GxEPDImage::Descriptor downloaded = {
		GxEPDImage::FORMAT_BWR_1BPP, Layout::CINEMA_W, Layout::CINEMA_H,
		CinemaStore::plane(0), CinemaStore::plane(1), nullptr,
	};
	const GxEPDImage::Descriptor *source =
		shown != Routefinder::ShownImage::Wallpaper ? &downloaded : WallpaperStore::image();
	if (source == nullptr)
	{
		drawMissingWallpaperNotice();
		return;
	}
	const GxEPDImage::Descriptor &image = *source;
	int16_t top = Layout::CINEMA_Y;
	int16_t height = (int16_t)(image.height < Layout::CINEMA_H ? image.height : Layout::CINEMA_H);
	if (!Graphics::clipToPage(top, height)) return;

	const uint32_t skippedBytes = (uint32_t)(top - Layout::CINEMA_Y) * ((image.width + 7) / 8);
	const GxEPDImage::Descriptor rows = {
		image.format, image.width, (uint16_t)height,
		image.data0 + skippedBytes,
		image.data1 ? image.data1 + skippedBytes : nullptr,
		image.data2 ? image.data2 + skippedBytes : nullptr,
	};
	GxEPDImage::showImage(display, rows, Layout::CINEMA_X, top);
}

/** Canale e BSSID dell'ultima connessione riuscita. Stanno in RAM, che il
 *  light sleep conserva; un riavvio li azzera e la prima connessione scandisce. */
static bool g_wifi_ap_known = false;
static int32_t g_wifi_channel = 0;
static uint8_t g_wifi_bssid[6] = {};

/** Attende WL_CONNECTED fino a deadlineMs, con il passo di polling comune. */
static bool waitWifiConnected(uint32_t t0, uint32_t deadlineMs)
{
	while (WiFi.status() != WL_CONNECTED && (millis() - t0) < deadlineMs)
	{
		delay(WIFI_ATTESA_POLL_MS);
	}
	return WiFi.status() == WL_CONNECTED;
}

/**
 * Accende il WiFi in modalita' STA e attende la connessione fino a
 * WIFI_CONNECT_TIMEOUT_MS.
 * La radio resta accesa solo per la finestra di fetch: viene spenta
 * da wifiOff() subito dopo.
 *
 * Dal secondo giro prova prima sul canale e sul BSSID dell'ultima connessione,
 * che saltano la scansione dei canali: senza, il core li prova a partire dal
 * primo finchè trova la rete. Se il router ha cambiato canale il tentativo
 * veloce scade dopo WIFI_FAST_CONNECT_TIMEOUT_MS e si ripiega sulla connessione
 * normale nel tempo che resta.
 *
 * A connessione riuscita sincronizza l'orologio con Clock::sincronizza(), che al
 * primo boot puo' aggiungere fino a TIME_SYNC_TIMEOUT_MS di attesa, e poi lo
 * risincronizza ogni ora con Clock::resyncIfStale(), che non blocca.
 * @return true se connesso entro il timeout.
 */
static bool wifiOn()
{
	WiFi.mode(WIFI_STA);
	const uint32_t t0 = millis();
	bool connected = false;
	if (g_wifi_ap_known)
	{
		WiFi.begin(WIFI_SSID, WIFI_PASSWORD, g_wifi_channel, g_wifi_bssid);
		connected = waitWifiConnected(t0, WIFI_FAST_CONNECT_TIMEOUT_MS);
		if (!connected)
		{
			LOG("WiFi", "connessione veloce sul canale %ld fallita: scansione completa", (long)g_wifi_channel);
			g_wifi_ap_known = false;
			WiFi.disconnect(false, false);
		}
	}
	if (!connected)
	{
		WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
		connected = waitWifiConnected(t0, WIFI_CONNECT_TIMEOUT_MS);
	}
	if (connected)
	{
		g_wifi_channel = WiFi.channel();
		const uint8_t* bssid = WiFi.BSSID();
		if (bssid != nullptr)
		{
			memcpy(g_wifi_bssid, bssid, sizeof(g_wifi_bssid));
			g_wifi_ap_known = true;
		}
		LOG("WiFi", "connected in %lu ms, canale %ld, IP=%s",
			(unsigned long)(millis() - t0), (long)g_wifi_channel, WiFi.localIP().toString().c_str());
		Net::rememberIp(WiFi.localIP());
		/**
		 * Orologio sincronizzato nello stesso punto in cui la radio diventa
		 * disponibile: tutti i fetch del ramo normale stanno dentro questo
		 * if (wifiOn()), quindi da qui in avanti vedono l'ora vera. Bloccante,
		 * perche' qui si puo' attendere: nella finestra di manutenzione, dove
		 * non si puo', ci pensa Maintenance con la forma non bloccante.
		 */
		Clock::sincronizza();
		Clock::resyncIfStale();
		return true;
	}
	LOG("WiFi", "connection timeout");
	return false;
}

/**
 * Spegne completamente il WiFi (disconnect + WIFI_OFF) per minimizzare il
 * consumo fra un fetch e l'altro: ~120 mA attivo vs ~30 mA a radio spenta.
 *
 * disconnect(true, false) [wifioff=true, eraseap=false]: spegne la radio ma
 * non cancella la configurazione salvata. Da sola quella configurazione non
 * evita la scansione, perchè WiFi.begin(ssid, pass) senza canale fa scandire
 * i canali al core: il salto della scansione lo fa wifiOn() con canale e BSSID
 * memorizzati. Le credenziali in NVS non vanno comunque cancellate: ad ogni
 * begin() vengono ripassate dai #define di Env.h.
 */
static void wifiOff()
{
	WiFi.disconnect(true, false);
	WiFi.mode(WIFI_OFF);

	// La connessione Tuya resta aperta dopo il publish e si rilascia solo a
	// radio spenta: con la radio su la chiusura farebbe segnare offline il device.
	Tuya::releaseSession();
}

void setup()
{
	Serial.begin(115200);
	delay(1000);

	// Prima di ogni modulo: le cadenze salvate in NVS vanno lette e portate
	// dentro i limiti prima che qualcuno le usi.
	Timings::begin();
	// Dopo Timings: le fasce di percorso e cinema si validano contro la fascia WiFi.
	Routefinder::begin();
	/**
	 * Buffer in RAM di percorso e cinema, allocato prima di tutto il resto,
	 * quando l'heap ha ancora i blocchi contigui più grandi. Un fallimento qui
	 * non è definitivo: si ritenta prima di ogni download.
	 */
	CinemaStore::begin(Layout::CINEMA_PLANE_SZ);
	initDisplay();
	/**
	 * TZ Europe/Rome con DST automatico: va fatto PRIMA di qualsiasi
	 * modulo che formatti orari locali (Weather/Calendar usano localtime_r).
	 */
	Clock::begin();
	Weather::begin();
	Calendar::Outlook::begin();
	Calendar::Google::begin();
	/**
	 * Sensore ambientale BME680 via I2C (Bosch BSEC2 in ULP, sample ogni 5 min). 
	 * Lo stato del calibratore, se presente in NonVolatileStorage, viene ripristinato qui dentro.
	 */
	Indoor::begin();
	/**
	 * Telemetria Tuya: compone topic e client id dal DeviceID di Env.h e resta
	 * disattivata se le credenziali non ci sono. Nessuna rete qui dentro.
	 */
	Tuya::begin();
	// Wallpaper nella partizione in flash, a schermo già dal primo frame.
	WallpaperStore::begin();
	LOG("BT", "memoria del controller Bluetooth rilasciata: %s", btMemReleased(BT_MODE_BTDM) ? "si" : "NO");
	/**
	 * Apre la finestra di manutenzione sulla rete di casa in sola STA, e accende
	 * l'access point di emergenza solo se la STA non sale entro
	 * MAINT_STA_TIMEOUT_S. Alla scadenza loop() chiama Maintenance::chiudi(), che
	 * consegna la radio spenta allo scheduler e il ciclo passa al light sleep.
	 *
	 * Di notte la radio non si accende: con l'ora già valida, cioè dopo un
	 * riavvio software (OTA, crash, watchdog) perchè il timer RTC la conserva,
	 * la finestra non si apre. Dopo un'interruzione di alimentazione l'ora non
	 * è nota, e solo la rete può dirla: la finestra si apre come sempre.
	 */
	if (Clock::valido() && !Scheduler::isInsideRadioHours())
		LOG("MAINT", "fuori dalla fascia WiFi con l'ora valida: finestra di manutenzione non aperta");
	else
		Maintenance::begin();

	Scheduler::Hooks hooks;
	hooks.radioOn = wifiOn;
	hooks.radioOff = wifiOff;
	hooks.sensorActive = Indoor::attivo;
	hooks.msUntilSensorPoll = Indoor::msUntilNextPoll;
	hooks.pollSensor = Indoor::poll;
	hooks.drawFrame = Weather::render;
	Scheduler::begin(TASK_TABLE, TASK_COUNT, hooks);
}

/**
 * Ciclo principale a due rami.
 *
 * Durante la finestra di manutenzione la radio è della finestra, in STA o in
 * AP_STA: il web server processa gli upload e lo scheduler, a STA connessa e
 * ora assestata, esegue i task dovuti e disegna il primo frame.
 * Niente light sleep qui, altrimenti il WebServer non risponderebbe.
 *
 * Scaduta la finestra, Maintenance::chiudi() spegne AP e WiFi e il ciclo torna al
 * comportamento energetico ordinario: a ogni ciclo del sensore la STA si accende
 * solo per i fetch dovuti e si spegne subito dopo, poi il frame, poi il light
 * sleep fino alla scadenza esatta del sensore. Di notte solo il sensore.
 */
void loop()
{
	if (Maintenance::finestraAperta())
	{
		Maintenance::servi();
		/**
		 * La radio e' della finestra OTA, in AP_STA: lo scheduler esegue i task
		 * dovuti quando la STA e' su, senza accenderla ne' spegnerla (wifiOff()
		 * butterebbe giu' anche l'AP). Nessun light sleep finche' la finestra
		 * e' aperta, altrimenti il web server non risponderebbe.
		 *
		 * Mentre qualcuno usa le pagine lo scheduler aspetta: un fetch o un
		 * refresh lascerebbero il web server muto per decine di secondi. Il
		 * sensore no: costa ~50 ms, e BSEC perde l'accuratezza se il poll
		 * ritarda oltre il 6,25% del suo periodo.
		 */
		if (!Maintenance::clientAttivo())
			Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE);
		else
			Scheduler::serviceSensor();
		delay(MAINTENANCE_LOOP_DELAY_MS);
		return;
	}

	// Finestra chiusa: consegna la radio spenta allo scheduler (idempotente).
	Maintenance::chiudi();

	Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);

	/**
	 * Pannello a riposo prima del light sleep dell'MCU. Senza un riferimento
	 * del refresh differenziale è il deep sleep: porta il controller da
	 * standby a 1-5 uA, ed è quasi gratis perchè il refresh pieno chiude con
	 * 0x22 = 0xF7, che ha già disabilitato analog e clock, quindi il
	 * _PowerOff() interno a hibernate() è un no-op e resta solo il comando di
	 * deep sleep. Con un riferimento valido il controller resta invece in
	 * Sleep (25-40 uA) e conserva le due RAM, che il frame successivo usa per
	 * il partial. Idempotente: nei giri senza ridisegno la chiamata non fa
	 * niente.
	 *
	 * Al risveglio dal deep sleep non serve fare niente da qui: alla prima
	 * scrittura il driver esegue reset hardware, SWRESET e init, e ripulisce i
	 * due piani perchè il deep sleep NON ritiene la RAM del controller. Sono
	 * ~60 ms sul primo frame utile; ogni frame pieno riparte comunque da reset
	 * e init, anche se il controller non dormiva.
	 *
	 * Sta solo in questo ramo: nella finestra OTA il loop gira ogni ~10 ms e
	 * il pannello verrebbe addormentato e risvegliato in continuazione.
	 */
	display.hibernate();

	// Margini di memoria misurati, nei soli giri che hanno lavorato: sono la
	// verifica sul campo delle stime sul picco di heap e sullo stack del loop.
	if (Scheduler::ranTasksThisRound())
		LOGV("mem", "heap min %lu, blocco max %lu, stack loop residuo %lu",
			 (unsigned long)ESP.getMinFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(),
			 (unsigned long)uxTaskGetStackHighWaterMark(nullptr));

	Scheduler::sleepUntilNextEvent();
}

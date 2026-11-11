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
 * REFRESH PARZIALE: DISATTIVATO, ed è una scelta di questo firmware e non un
 * limite del pannello.
 *
 * Il driver 097c sa fare un partial in bianco e nero da 639 ms contro i ~24 s
 * del refresh pieno, ma sotto la sua waveform la RAM 0x26 del controller
 * diventa il FRAME PRECEDENTE invece del piano accent. Conseguenza: un frame
 * aggiornato in partial è per forza SENZA ROSSO, e la scelta è per frame e non
 * per pixel, quindi non esiste il caso "aggiorno in partial una zona e tengo
 * il rosso nel resto dello schermo". Questa dashboard il rosso lo usa (celle
 * del calendario, riquadri, icone meteo), quindi il partial non è applicabile
 * così come è.
 *
 * A 0 il firmware lavora solo in full-window e non chiama nessuna delle sei
 * API opt-in del driver: drawImagePartial(), drawImagePartialPart(),
 * refreshPartial(), writeImagePrevious(), writeScreenBufferPrevious() e
 * setPartialLut().
 * Lo static_assert dopo la costruzione di `display` sorveglia l'unica strada
 * per cui il partial potrebbe attivarsi da sè, cioè il flag
 * hasFastPartialUpdate del driver.
 *
 * Per riabilitarlo in futuro NON basta mettere 1: vedi il messaggio dell'#error
 * accanto allo static_assert, che elenca cosa va implementato.
 */
#define DISPLAY_PARTIAL_REFRESH 0

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
 * Origine del server cinema, unico punto in cui compare il dominio: il ping
 * di sveglia e l'URL dell'immagine, composto in fetchCinemaImage() dalle
 * misure del layout, ci aggiungono solo il percorso. Non e' un tempo, quindi
 * non sta in Timings.h; il timeout del ping invece si' (CINEMA_PREWARM_TIMEOUT_MS).
 */
#define CINEMA_ORIGIN      "https://cinema-epd.onrender.com"
#define CINEMA_PREWARM_URL CINEMA_ORIGIN "/health"

/** Origine del servizio routefinder (submodule omonimo), con lo stesso ruolo
 *  di CINEMA_ORIGIN: il percorso dell'immagine lo compone fetchRouteImage(). */
#define ROUTEFINDER_ORIGIN      "https://routefinder-7w0a.onrender.com"
#define ROUTEFINDER_PREWARM_URL ROUTEFINDER_ORIGIN "/health"

/**
 * Un server render.com del free tier, con l'ultimo contatto che decide se il
 * ping di sveglia serve (vedi prewarmServer()). Sta prima di ogni funzione del
 * file perchè il preprocessore di Arduino ne genera i prototipi in quel punto,
 * e quelli che lo nominano non compilerebbero.
 */
struct RenderServer
{
	const char *flow;
	const char *healthUrl;
	bool contacted;          // falso finchè il server non ha risposto, o dopo RENDER_SPINDOWN_MS
	uint32_t lastContactMs;  // millis() dell'ultima risposta HTTP, del ping o del download
};

static RenderServer g_cinema_server = {"cinema", CINEMA_PREWARM_URL, false, 0};
static RenderServer g_route_server = {"route", ROUTEFINDER_PREWARM_URL, false, 0};

#include <GxEPD2_3C.h>
#include "Layout.h"   // dispatcher: include Layout_097c.h o Layout_122c.h in base al #define DISPLAY_VARIANT_*

// Fallback wallpaper offline: immagine PROGMEM
#include "wallpaper/img_la_grande_onda.h" //img_la_grande_onda_desc

/** Weather.h contiene logica, fetch OpenWeather One Call 3.0, cache,
 * rendering banner. Il .ino si limita ad accendere/spegnere il WiFi
 * al momento giusto e a chiamare Weather::render() per aggiornare il
 * display.
 *
 * include transitivamente Calendar.h e Indoor.h
 * => basta questo piu' Maintenance.h per avere tutte le API dei moduli */
#include "Weather.h"
#include "Maintenance.h"
#include "Mail.h"
// Telemetria BME680 verso il cloud Tuya (TuyaLink su MQTT). Non disegna nulla:
// il pannello la interroga solo per il badge di guasto del riquadro Indoor.
#include "Tuya.h"
#include "Env.h"
#include "Log.h"
#include "Clock.h"
#include "Net.h"
#include "Scheduler.h"
#include "CinemaStore.h"
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
 * Sorveglianza del refresh parziale, vedi DISPLAY_PARTIAL_REFRESH in testa.
 *
 * Il ramo a 0 controlla il flag del driver, e le due cose che il template fa in
 * modalità finestra parziale vanno tenute distinte, perchè è facile
 * attribuirle allo stesso interruttore:
 *   - GxEPD2_3C scrive il piano accent dentro la RAM 0x26 (GxEPD2_3C.h:340),
 *     che sotto la waveform del partial è il frame precedente. Dipende da
 *     setPartialWindow() e NON dal flag: a proteggere è refresh(x, y, w, h),
 *     che sul driver 097c fa comunque un refresh pieno;
 *   - hasFastPartialUpdate è consultato in un solo punto (GxEPD2_3C.h:355) e fa
 *     ripetere l'intero loop paged dopo il refresh, riscrivendo i due piani
 *     senza rinfrescare e senza riallineare 0x26.
 * Questo firmware lavora in sola full-window, quindi il flag non verrebbe
 * nemmeno letto. L'assert resta come tripwire: vederlo a true vorrebbe dire che
 * qualcuno ha creduto che il template sappia pilotare questo partial.
 *
 * Il ramo a 1 esiste per non far passare il flag come un interruttore che non
 * commuta niente. Riabilitare il partial vuol dire:
 *   1. togliere questo #error;
 *   2. scrivere un percorso di rendering dedicato per le sole zone in bianco e
 *      nero, che chiami epd2.drawImagePartial() FUORI da firstPage()/nextPage():
 *      il partial del driver vive fuori dal template di proposito. Per il testo
 *      si disegna su una GFXcanvas1 di Adafruit_GFX e se ne passa il buffer con
 *      invert = true e pgm = false, vedi il README della libreria;
 *   3. accettare due limiti misurati. Il frame aggiornato in partial non ha
 *      rosso, e dove il nero viene RIMOSSO resta un grigio leggero, perchè su un
 *      film BWR il pigmento rosso è lento e i 560 ms della waveform non gli
 *      bastano. Le aree mai pilotate non degradano, ma il pavimento di grigio
 *      dell'area di lavoro lo azzera solo un refresh pieno: va deciso ogni
 *      quanti partial rifarne uno.
 * NON va alzato hasFastPartialUpdate nel driver: quella è la strada sbagliata,
 * per il motivo scritto sopra.
 */
#if DISPLAY_PARTIAL_REFRESH
#error "DISPLAY_PARTIAL_REFRESH = 1 non è implementato: il partial del driver 097c va chiamato out-of-band con epd2.drawImagePartial(), non dal loop paged, e rende un frame senza rosso. Vedi il commento qui sopra per i tre passi."
#else
static_assert(!Layout::Panel::hasFastPartialUpdate,
			  "Il driver dichiara hasFastPartialUpdate = true, ma DISPLAY_PARTIAL_REFRESH è 0: "
			  "il partial di questo driver non passa dal template e quel flag non lo abilita: "
			  "vederlo a true segnala un fraintendimento. Rimettere false nel driver.");
#endif

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
 *     pieno: nessun guadagno e una modalità in più da mantenere.
 *   - le sei API del partial del driver NON vanno chiamate da qui:
 *     drawImagePartial(), drawImagePartialPart(), refreshPartial(),
 *     writeImagePrevious(), writeScreenBufferPrevious() e setPartialLut(). Sono
 *     opt-in, quindi basta non chiamarle; il perchè sta in
 *     DISPLAY_PARTIAL_REFRESH in testa al file.
 *   - il resto lo gestisce il driver da sè: init dei due piani alla prima
 *     scrittura, ricarica della waveform dall'OTP a ogni refresh pieno, e
 *     ripulitura della RAM al risveglio da hibernate().
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
}

// ===========================================================================
// Background cinema
//
// L'immagine occupa Layout::CINEMA_W x Layout::CINEMA_H a partire da
// (Layout::CINEMA_X, Layout::CINEMA_Y), dentro i margini in cima alla colonna
// sinistra; sotto c'e' il riquadro Posta, a destra la scheda del calendario.
// Le misure le deriva Layout_common.h: 610w x 290h sul 9.7", 610w x 325h sul
// 12.2". Convenzione: NwxMh = N px larghezza x M px altezza.
//
// L'immagine scaricata vive in flash, in uno dei due slot di CinemaStore, e
// sopravvive al reboot; senza un'immagine valida si mostra il fallback PROGMEM.
// Il fetch giornaliero scrive lo slot libero e lo rende attivo solo a download
// completo, quindi un fetch fallito lascia a schermo l'immagine precedente.
// ===========================================================================

/**
 * Le dimensioni dell'area wallpaper sono in Layout::CINEMA_W (larghezza X) /
 * CINEMA_H (altezza Y) / CINEMA_STRIDE / CINEMA_PLANE_SZ / CINEMA_TOTAL_SZ, e
 * l'URL del server si compone da quelle: cambiare display regola da solo sia
 * la query string sia la misura dell'immagine salvata in flash.
 *
 * GxEPDImage::showImage() non clippa e disegna dalla width/height del
 * Descriptor: drawTestBackground() taglia le righe all'altezza CINEMA_H, mentre
 * un'immagine più larga di CINEMA_W sconfina nella colonna destra. Quella
 * scaricata ha sempre la misura esatta; il fallback PROGMEM va generato a
 * CINEMA_W x CINEMA_H.
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
 * Sveglia un'istanza render.com con una GET a /health mandata e abbandonata.
 * La usano i server del cinema e del percorso, entrambi sul free tier.
 *
 * Il free tier sospende il servizio dopo 15 min di inattivita' e il boot
 * successivo costa una ventina di secondi, che senza questo ping il fetch
 * dell'immagine pagherebbe per intero dentro il suo timeout.
 *
 * Su TLS il fire-and-forget puro non esiste: la richiesta arriva al router di
 * render solo a handshake completo, quindi la connessione va portata su per
 * intero. L'handshake ha il tetto comune TLS_HANDSHAKE_TIMEOUT_S, ampio per un
 * edge che risponde anche a istanza ferma, e indispensabile qui: il ping gira
 * prima di tutti gli altri fetch, e un handshake bloccato col default del core
 * (120 s) li fermerebbe tutti.
 * Quello che si abbandona e' la RISPOSTA: con setTimeout() breve la GET
 * ritorna appena scaduta l'attesa degli header, end() chiude il socket e
 * l'istanza prosegue il boot per conto suo. HTTP/1.0 chiude la connessione
 * senza keep-alive. Il chiamante intanto fa meteo, mail e calendari, che e' il
 * tempo di copertura vero di questo meccanismo.
 *
 * Nessun valore di ritorno: l'esito non cambia niente nel resto del giro. Il
 * codice HTTP viene solo loggato ed e' l'unica diagnostica disponibile:
 * HTTPC_ERROR_READ_TIMEOUT (-11) e' l'esito nominale, 200 significa che il
 * server era gia' caldo, un errore di connessione che il ping non e' partito.
 *
 * Gira solo come onRoundStart() del task che scaricherà davvero l'immagine,
 * e solo se l'istanza dorme: render la spegne dopo RENDER_SPINDOWN_MS senza
 * richieste, quindi un server che ha risposto da meno di così è sveglio. Nella
 * fascia percorso le richieste sono a un ciclo l'una dall'altra e il ping
 * parte solo al primo ciclo; il cinema, giornaliero, lo manda sempre.
 */
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

static void prewarmServer(RenderServer &server)
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
	if (!Net::beginHttps(http, client, server.healthUrl))
	{
		LOG("img", "%s pre-warm: http.begin fallita", server.flow);
		return;
	}
	uint32_t t0 = millis();
	int code = http.GET();
	http.end();
	// Un codice HTTP positivo è una risposta dell'istanza; il timeout di
	// lettura (-11), esito nominale a istanza ferma, non lo è.
	if (code > 0) noteServerContact(server);
	LOG("img", "%s pre-warm %s -> %d in %lu ms", server.flow, server.healthUrl, code, (unsigned long)(millis() - t0));
}

static void prewarmCinemaServer() { prewarmServer(g_cinema_server); }
static void prewarmRouteServer() { prewarmServer(g_route_server); }

/**
 * Scarica un'immagine nel formato di /cinema/arduino e la salva nello slot
 * inattivo di CinemaStore; la usano cinema e percorso, che hanno la stessa
 * misura Layout::CINEMA_*. Cadenza e ritenti sono dello scheduler; qui si
 * scarica e basta, con la radio già connessa.
 *
 * Lo slot libero si cancella prima della GET, poi il corpo passa in flash a
 * blocchi da 1 KB letti da Net::BodyStream, senza buffer grandi in heap. Su
 * qualunque errore (HTTP diverso da 200, Content-Length diverso da
 * Layout::CINEMA_TOTAL_SZ, corpo interrotto, scrittura fallita) la scrittura
 * si annulla e l'immagine cinema attiva resta quella di prima.
 * Ogni risposta HTTP, anche un errore, conta come contatto con il server: è
 * ciò che evita il ping di sveglia al ciclo successivo.
 * @param server server render.com da cui si scarica, per il ping condizionato.
 * @param token valore dell'header X-Routefinder-Token, nullptr se non serve.
 * @return true se l'immagine è stata scaricata e salvata.
 */
static bool downloadImageToStore(RenderServer &server, const char *url, const char *token,
								 uint32_t timeoutMs, CinemaStore::Kind kind)
{
	const char *flow = server.flow;
	LOG("img", "%s fetching %s", flow, url);
	// L'erase dello slot libero ferma la cache della flash: avviene prima che
	// la connessione TLS esista.
	if (!CinemaStore::beginWrite(kind))
	{
		LOG("img", "%s: slot in flash non disponibile", flow);
		return false;
	}

	// Client esplicito, dichiarato prima di HTTPClient perchè il distruttore di
	// http lo usa: con begin(url) il core ne creerebbe uno interno, senza modo
	// di limitarne l'handshake. I server rispondono con Content-Length anche in
	// HTTP/1.0, quindi il controllo sulla dimensione resta valido.
	WiFiClientSecure client;
	HTTPClient http;
	// Margine per il cold start del free tier render.com quando il pre-warm non
	// e' bastato a completare il boot dell'istanza prima di arrivare qui. Vale
	// anche come attesa massima senza dati durante la lettura del corpo.
	http.setTimeout(timeoutMs);
	if (!Net::beginHttps(http, client, url))
	{
		LOG("img", "%s: HTTP begin fallita", flow);
		CinemaStore::abortWrite();
		return false;
	}
	if (token != nullptr) http.addHeader("X-Routefinder-Token", token);
	int code = http.GET();
	if (code > 0) noteServerContact(server);
	if (code != 200)
	{
		LOG("img", "%s: HTTP status %d", flow, code);
		http.end();
		CinemaStore::abortWrite();
		return false;
	}
	int size = http.getSize();
	if (size != (int)Layout::CINEMA_TOTAL_SZ)
	{
		LOG("img", "%s: Content-Length %d atteso %u", flow, size, (unsigned)Layout::CINEMA_TOTAL_SZ);
		http.end();
		CinemaStore::abortWrite();
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
			LOG("img", "%s: corpo interrotto a %lu/%lu byte", flow,
				(unsigned long)(received + got), (unsigned long)Layout::CINEMA_TOTAL_SZ);
			http.end();
			CinemaStore::abortWrite();
			return false;
		}
		received += got;
	}
	http.end();

	// Il percorso porta l'istante del download, che decide se è ancora
	// mostrabile; il task che lo scarica gira solo con l'orologio valido.
	const uint32_t downloadedAt = kind == CinemaStore::Kind::Route ? (uint32_t)time(nullptr) : 0;
	if (!CinemaStore::commitWrite(downloadedAt))
	{
		LOG("img", "%s: salvataggio in flash fallito", flow);
		return false;
	}
	LOG("img", "%s: download completato e salvato in flash", flow);
	return true;
}

/**
 * Scarica l'immagine cinema dal server render.com.
 *
 * Ultima chiamata di rete del giro, dopo meteo, mail e calendari: è l'unica
 * che può pagare il cold start di render.com, e il tempo speso dagli altri
 * fetch è il tempo che il server ha per completare il boot avviato da
 * prewarmCinemaServer().
 * @return true se l'immagine è stata scaricata e resa attiva.
 */
static bool fetchCinemaImage()
{
	/**
	 * URL composto dalle misure del layout, così larghezza, altezza e piani
	 * chiesti al server non possono divergere dall'immagine salvata. Un
	 * troncamento manderebbe al server un'altra misura: si ferma il fetch.
	 */
	char url[160];
	const int urlLength = snprintf(url, sizeof(url),
		CINEMA_ORIGIN "/cinema/arduino?width=%d&height=%d&colors=%s&dither=floyd",
		(int)Layout::CINEMA_W, (int)Layout::CINEMA_H, cinemaColorsParam(Layout::CINEMA_PLANES));
	if (urlLength < 0 || urlLength >= (int)sizeof(url))
	{
		LOG("cinema", "URL troncato (%d caratteri), resta l'immagine precedente", urlLength);
		return false;
	}
	return downloadImageToStore(g_cinema_server, url, nullptr, CINEMA_HTTP_TIMEOUT_MS, CinemaStore::Kind::Cinema);
}

/**
 * Scarica il percorso del mattino da routefinder, con partenza, arrivo e
 * carburante di Routefinder e la misura del riquadro cinema, che sostituisce.
 * @return true se il percorso è stato scaricato e salvato.
 */
static bool fetchRouteImage()
{
	char url[256];
	const int urlLength = Routefinder::buildUrl(url, sizeof(url), ROUTEFINDER_ORIGIN,
		(uint16_t)Layout::CINEMA_W, (uint16_t)Layout::CINEMA_H, cinemaColorsParam(Layout::CINEMA_PLANES));
	if (urlLength < 0 || urlLength >= (int)sizeof(url))
	{
		LOG("route", "URL troncato (%d caratteri)", urlLength);
		return false;
	}
	return downloadImageToStore(g_route_server, url, ROUTEFINDER_TOKEN, ROUTE_HTTP_TIMEOUT_MS, CinemaStore::Kind::Route);
}

/**
 * Adattatori fra l'API dei moduli e il vocabolario dello scheduler.
 * Restano nel .ino perche' la tabella nomina anche il cinema, che e' fatto di
 * statici di questo file.
 */
static Scheduler::Outcome runWeatherFetch()
{
	return Weather::runFetch() ? Scheduler::Outcome::OK : Scheduler::Outcome::FAILED;
}

/** Corrente senza previsioni non basta a disegnare il banner: lo scheduler
 *  ritenta invece di attendere la cadenza piena. */
static bool isWeatherIncomplete() { return !Weather::datiCompleti(); }

static Scheduler::Outcome runMailFetch()
{
	return Mail::runFetch() ? Scheduler::Outcome::OK : Scheduler::Outcome::FAILED;
}

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
static bool mailReady() { return Mail::isConfigured(); }
static bool googleReady() { return isClockValidForCalendars() && Calendar::Google::isConfigured(); }
static bool outlookReady() { return isClockValidForCalendars() && Calendar::Outlook::isConfigured(); }

static Scheduler::Outcome runCinemaFetch()
{
	return fetchCinemaImage() ? Scheduler::Outcome::OK : Scheduler::Outcome::FAILED;
}

/**
 * Senza la partizione del wallpaper non c'è dove salvare l'immagine, e mentre
 * il percorso è la sorgente del riquadro il cinema non si scarica, perchè
 * scriverebbe lo slot del percorso: il task resta sospeso e non manda nemmeno
 * il ping di sveglia. La cadenza giornaliera recupera in giornata, quindi un
 * cine_h dentro la fascia percorso slitta alla fine della fascia.
 */
static bool cinemaReady() { return CinemaStore::isAvailable() && !Routefinder::isRouteSourceActive(); }

static Scheduler::Outcome runRouteFetch()
{
	return fetchRouteImage() ? Scheduler::Outcome::OK : Scheduler::Outcome::FAILED;
}

/** Riga del percorso nella tabella dei task, cercata da setup(). */
static Scheduler::Task *g_route_task = nullptr;
static bool g_route_fetch_was_active = false;

/**
 * Precondizione del percorso: sorgente percorso attiva e ora dentro la fascia
 * percorso, qualunque sia la sorgente, così routefinder non riceve richieste
 * fuori fascia nemmeno con la sorgente forzata a Percorso; poi orologio valido
 * perchè l'istante del download decide se l'immagine è ancora mostrabile,
 * arrivo e token configurati, archivio disponibile.
 *
 * All'ingresso nella fascia rimette il task a mai eseguito, così il primo
 * fetch parte al primo ciclo della fascia invece di dipendere dal conteggio
 * dei cicli dell'ultimo slot, di un'altra giornata. È l'unico effetto
 * collaterale ed è idempotente: lo scheduler valuta la precondizione più
 * volte per giro.
 */
static bool routeReady()
{
	const bool active = Routefinder::isRouteSourceActive() && Routefinder::isInsideRouteHours();
	if (active && !g_route_fetch_was_active && g_route_task != nullptr) g_route_task->state.neverRun = true;
	g_route_fetch_was_active = active;
	return active && Clock::valido() && Routefinder::hasDestination() &&
		   Net::isCredentialSet(ROUTEFINDER_TOKEN) && CinemaStore::isAvailable();
}

/** Immagine che il riquadro deve mostrare adesso, dalla sorgente scelta e dal
 *  percorso salvato. La legge drawTestBackground() a ogni frame, quindi un
 *  cambio di fascia, di sorgente o di freschezza compare al ciclo successivo. */
static Routefinder::ShownImage currentShownImage()
{
	return Routefinder::shownImage(CinemaStore::image(CinemaStore::Kind::Route) != nullptr,
								   CinemaStore::routeDownloadedAt());
}

static Scheduler::Outcome runTuyaPublish()
{
	return Tuya::runPublish() ? Scheduler::Outcome::OK : Scheduler::Outcome::FAILED;
}

/** Senza credenziali, senza un campione del sensore o senza orologio non c'e'
 *  niente da pubblicare: il task resta sospeso, non accende la radio e non
 *  brucia il suo unico tentativo per cadenza. */
static bool tuyaReady() { return Tuya::readyToPublish(); }

/** Passo del sensore del ciclo: il valore di ritorno di Indoor::refresh() non
 *  serve, perchè il frame del ciclo si disegna comunque. */
static void pollIndoorSensor() { Indoor::refresh(); }

/**
 * Tutti i flussi di rete del dispositivo. L'ordine delle righe è l'ordine di
 * esecuzione dentro il passo di rete del ciclo, che viene dopo il sensore e
 * prima del frame, e non è arbitrario:
 *
 *   - mail prima di Google perche' i due condividono la cache del token
 *     OAuth: chi gira per primo paga il refresh;
 *   - Google prima di Outlook perche' il token appena rinfrescato e' il suo;
 *   - la telemetria Tuya dopo il sensore, che nello stesso ciclo ha appena
 *     prodotto il campione da pubblicare, e prima del cinema: i secondi del
 *     suo handshake TLS sono altro tempo di copertura del boot di render.com;
 *   - percorso e cinema per ultimi perche' sono gli unici che possono pagare
 *     il cold start di render.com, e il tempo di rete degli altri e' la
 *     copertura di quel boot; i loro onRoundStart() mandano il ping di sveglia
 *     all'inizio del passo, solo se l'istanza dorme. Non girano mai nello
 *     stesso ciclo: il cinema è sospeso finchè il percorso è la sorgente
 *     del riquadro.
 *
 * Le cadenze sono multipli del ciclo (Timings.h, campo `step`): meteo, mail e
 * calendari a 10 minuti cadono un ciclo sì e uno no, Tuya e percorso a ogni
 * ciclo. Nessun task chiede ridisegni: il frame si disegna a ogni ciclo della
 * fascia WiFi e legge da sè i dati, badge di Tuya compreso.
 *
 * Tuya è l'unico task con maxAttempts a 0, cioè un tentativo per slot comunque
 * vada: i suoi modi di fallimento sono permanenti sulla scala dei minuti
 * (credenziali, region, device model) e ogni ritento costerebbe un altro
 * handshake TLS senza cambiare l'esito.
 */
static Scheduler::Task TASK_TABLE[] = {
	{"meteo", Scheduler::Cadence::PERIODIC, &Timings::Valori::owmMin, nullptr, FETCH_MAX_TENTATIVI,
	 runWeatherFetch, weatherReady, isWeatherIncomplete, nullptr},

	{"mail", Scheduler::Cadence::PERIODIC, &Timings::Valori::mailMin, nullptr, FETCH_MAX_TENTATIVI,
	 runMailFetch, mailReady, nullptr, nullptr},

	{"google", Scheduler::Cadence::PERIODIC, &Timings::Valori::googleMin, nullptr, FETCH_MAX_TENTATIVI,
	 runGoogleCalendarFetch, googleReady, nullptr, nullptr},

	{"outlook", Scheduler::Cadence::PERIODIC, &Timings::Valori::outlookMin, nullptr, FETCH_MAX_TENTATIVI,
	 runOutlookCalendarFetch, outlookReady, nullptr, nullptr},

	{"tuya", Scheduler::Cadence::PERIODIC, &Timings::Valori::tuyaMin, nullptr, 0,
	 runTuyaPublish, tuyaReady, nullptr, nullptr},

	{"route", Scheduler::Cadence::PERIODIC, &Timings::Valori::routeMin, nullptr, FETCH_MAX_TENTATIVI,
	 runRouteFetch, routeReady, nullptr, prewarmRouteServer},

	{"cinema", Scheduler::Cadence::DAILY, nullptr, &Timings::Valori::cinemaOra, FETCH_MAX_TENTATIVI,
	 runCinemaFetch, cinemaReady, nullptr, prewarmCinemaServer},
};

static constexpr uint8_t TASK_COUNT = sizeof(TASK_TABLE) / sizeof(TASK_TABLE[0]);

/**
 * Disegna, dentro il loop paged di Weather::renderFrame(), le sole righe del
 * riquadro che cadono nella pagina corrente: il percorso o il cinema in flash
 * secondo la sorgente, altrimenti il fallback PROGMEM, che è anche la sorgente
 * Wallpaper. Le righe si tagliano anche all'altezza CINEMA_H, e il descrittore
 * ridotto punta agli stessi dati, senza copie.
 */
void drawTestBackground()
{
	const Routefinder::ShownImage shown = currentShownImage();
	const GxEPDImage::Descriptor *stored =
		shown == Routefinder::ShownImage::Route    ? CinemaStore::image(CinemaStore::Kind::Route) :
		shown == Routefinder::ShownImage::Cinema   ? CinemaStore::image(CinemaStore::Kind::Cinema) :
													 nullptr;
	const GxEPDImage::Descriptor &image = stored ? *stored : img_la_grande_onda_desc;
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
	// Dopo Timings: la fascia del percorso si valida contro la fascia WiFi.
	Routefinder::begin();
	initDisplay();
	/**
	 * TZ Europe/Rome con DST automatico: va fatto PRIMA di qualsiasi
	 * modulo che formatti orari locali (Weather/Calendar usano localtime_r).
	 */
	Clock::begin();
	Weather::begin();
	Calendar::Outlook::begin();
	Calendar::Google::begin();
	// Cache mail Gmail (vuota al boot). Il fetch parte alla prima finestra WiFi.
	Mail::begin();
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
	/**
	 * Archivio in flash dell'immagine cinema e del percorso: l'ultima immagine
	 * torna a schermo già dal primo frame, il percorso solo se ancora recente.
	 * Nessuna rete qui dentro.
	 */
	CinemaStore::begin();
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
	for (uint8_t i = 0; i < TASK_COUNT; ++i)
		if (strcmp(TASK_TABLE[i].tag, "route") == 0) g_route_task = &TASK_TABLE[i];

	Scheduler::Hooks hooks;
	hooks.radioOn = wifiOn;
	hooks.radioOff = wifiOff;
	hooks.sensorActive = Indoor::attivo;
	hooks.msUntilSensorPoll = Indoor::msUntilNextPoll;
	hooks.pollSensor = pollIndoorSensor;
	hooks.drawFrame = Weather::render;
	Scheduler::begin(TASK_TABLE, TASK_COUNT, hooks);
}

/**
 * Ciclo principale a due rami.
 *
 * Durante la finestra OTA (OTA_WINDOW_MIN dal boot) la radio resta accesa in
 * AP_STA: il web server processa eventuali upload firmware,
 * e in parallelo non appena la STA è WL_CONNECTED,
 * Weather::runFetch() scarica il meteo.
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
	 * Pannello in deep sleep prima del light sleep dell'MCU: porta il
	 * controller da standby a 1-5 uA, ed è quasi gratis perchè il refresh
	 * pieno chiude con 0x22 = 0xF7, che ha già disabilitato analog e clock:
	 * il _PowerOff() interno a hibernate() è quindi un no-op e resta solo il
	 * comando di deep sleep. Idempotente: nei giri senza ridisegno il
	 * pannello è già addormentato e la chiamata non fa nulla.
	 *
	 * Al risveglio non serve fare niente da qui: alla prima scrittura il
	 * driver esegue reset hardware, SWRESET e init, e ripulisce i due piani
	 * perchè il deep sleep NON ritiene la RAM del controller. Sono ~60 ms sul
	 * primo frame utile.
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

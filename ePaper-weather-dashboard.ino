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
 * A 1 il publish della telemetria Tuya puo' accendere la radio anche fuori
 * dalla fascia WiFi, cosi' l'app riceve dati 24 ore su 24 mentre meteo,
 * calendari, mail e refresh del pannello restano confinati alla finestra. A 0
 * di notte non si pubblica e nell'app resta visibile l'ultimo dato della sera.
 *
 * Alimenta il campo `runsOutsideRadioHours` della tabella dei task: è l'unico task che
 * lo usa, tutti gli altri stanno dentro la fascia.
 */
#define TUYA_IGNORE_ACTIVE_HOUR 0

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

SPIClass hspi(HSPI);

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
 * Sveglia l'istanza render.com del server cinema con una GET a /health
 * mandata e abbandonata.
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
 * Stessi due gate di fetchCinemaImage(), e per lo stesso motivo: il ping ha
 * senso solo nel giro in cui l'immagine verra' davvero scaricata. Svegliare
 * render a ogni wake da DISPLAY_REFRESH_MIN terrebbe l'istanza sempre accesa,
 * bruciando le ore-istanza del free tier senza servire a niente.
 */
static void prewarmCinemaServer()
{
	WiFiClientSecure client;
	HTTPClient http;
	http.setTimeout(CINEMA_PREWARM_TIMEOUT_MS);
	if (!Net::beginHttps(http, client, CINEMA_PREWARM_URL))
	{
		LOG("cinema", "pre-warm: http.begin fallita");
		return;
	}
	uint32_t t0 = millis();
	int code = http.GET();
	http.end();
	LOG("cinema", "pre-warm %s -> %d in %lu ms",
		CINEMA_PREWARM_URL, code, (unsigned long)(millis() - t0));
}

/**
 * Scarica l'immagine cinema dal server render.com e la salva in flash.
 *
 * Ultima chiamata di rete del giro, dopo meteo, mail e calendari: è l'unica
 * che può pagare il cold start di render.com, e il tempo speso dagli altri
 * fetch è il tempo che il server ha per completare il boot avviato da
 * prewarmCinemaServer(). Cadenza (giornaliera) e ritenti sono dello
 * scheduler; qui si scarica e basta, con la radio già connessa.
 *
 * Lo slot libero di CinemaStore si cancella prima della GET, poi il corpo
 * passa in flash a blocchi da 1 KB letti da Net::BodyStream, senza buffer
 * grandi in heap. Su qualunque errore (HTTP diverso da 200, Content-Length
 * diverso da Layout::CINEMA_TOTAL_SZ, corpo interrotto, scrittura fallita) la
 * scrittura si annulla e resta l'immagine precedente.
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

	LOG("cinema", "fetching %s", url);
	// L'erase dello slot libero ferma la cache della flash: avviene prima che
	// la connessione TLS esista.
	if (!CinemaStore::beginWrite())
	{
		LOG("cinema", "slot in flash non disponibile, resta l'immagine precedente");
		return false;
	}

	// Client esplicito, dichiarato prima di HTTPClient perchè il distruttore di
	// http lo usa: con begin(url) il core ne creerebbe uno interno, senza modo
	// di limitarne l'handshake. Il server risponde con Content-Length anche in
	// HTTP/1.0, quindi il controllo sulla dimensione resta valido.
	WiFiClientSecure client;
	HTTPClient http;
	// 45s: margine per il cold start del free tier render.com (una ventina di
	// secondi) quando il ping di prewarmCinemaServer() non e' bastato a
	// completare il boot dell'istanza prima di arrivare qui. Vale anche come
	// attesa massima senza dati durante la lettura del corpo.
	http.setTimeout(CINEMA_HTTP_TIMEOUT_MS);
	if (!Net::beginHttps(http, client, url))
	{
		LOG("cinema", "HTTP begin fallita");
		CinemaStore::abortWrite();
		return false;
	}
	int code = http.GET();
	if (code != 200)
	{
		LOG("cinema", "HTTP status %d, resta l'immagine precedente", code);
		http.end();
		CinemaStore::abortWrite();
		return false;
	}
	int size = http.getSize();
	if (size != (int)Layout::CINEMA_TOTAL_SZ)
	{
		LOG("cinema", "Content-Length %d atteso %u, resta l'immagine precedente",
			size, (unsigned)Layout::CINEMA_TOTAL_SZ);
		http.end();
		CinemaStore::abortWrite();
		return false;
	}

	// BodyStream attende le pause fra i blocchi del corpo: un blocco resta corto
	// solo a connessione chiusa, su errore TLS o dopo CINEMA_HTTP_TIMEOUT_MS
	// senza dati, il timeout che http.setTimeout() ha dato allo stream.
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
			LOG("cinema", "corpo interrotto a %lu/%lu byte, resta l'immagine precedente",
				(unsigned long)(received + got), (unsigned long)Layout::CINEMA_TOTAL_SZ);
			http.end();
			CinemaStore::abortWrite();
			return false;
		}
		received += got;
	}
	http.end();

	if (!CinemaStore::commitWrite())
	{
		LOG("cinema", "salvataggio in flash fallito, resta l'immagine precedente");
		return false;
	}
	LOG("cinema", "download completato e salvato in flash");
	return true;
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

/** Senza la partizione del wallpaper non c'è dove salvare l'immagine: il task
 *  resta sospeso e non manda nemmeno il ping di sveglia. */
static bool cinemaReady() { return CinemaStore::isAvailable(); }

static Scheduler::Outcome runTuyaPublish()
{
	return Tuya::runPublish() ? Scheduler::Outcome::OK : Scheduler::Outcome::FAILED;
}

/** Senza credenziali, senza un campione del sensore o senza orologio non c'e'
 *  niente da pubblicare: il task resta sospeso, non accende la radio e non
 *  brucia il suo unico tentativo per cadenza. */
static bool tuyaReady() { return Tuya::readyToPublish(); }

/** Stato del badge di guasto come si trova sul pannello disegnato. */
static bool g_tuya_badge_shown = false;

/**
 * Ridisegno solo quando il badge [TUYA ERR] compare o sparisce: un refresh
 * pieno costa ~24 s e la telemetria, a parte quel badge, non cambia niente a
 * schermo. Non passa da markDirtyOnOk perche' qui conta la transizione, non
 * l'esito del singolo giro.
 */
static void onTuyaOutcome(Scheduler::Outcome)
{
	if (Tuya::hasFailed() == g_tuya_badge_shown) return;
	g_tuya_badge_shown = Tuya::hasFailed();
	Weather::markDirty();
}

/** Interrogazione del sensore: OK quando il dato da mostrare è cambiato, cioè
 *  per un campione nuovo o per uno appena scaduto. */
static Scheduler::Outcome runIndoorSample()
{
	return Indoor::refresh() ? Scheduler::Outcome::OK : Scheduler::Outcome::SKIPPED;
}

/** Scadenza custom del task del sensore. La scadenza custom precede ogni altra
 *  regola dello scheduler, mai eseguito compreso, quindi deve valere 0 al boot:
 *  è Indoor a garantirlo. */
static uint32_t msUntilIndoorPoll() { return Indoor::msUntilNextPoll(); }

/**
 * Il dato del sensore chiede il ridisegno solo dentro la fascia WiFi: di notte
 * un refresh pieno ogni 5 minuti non servirebbe a nessuno. Il campione viene
 * comunque acquisito, e il primo ridisegno del mattino mostra il più recente.
 */
static void onIndoorOutcome(Scheduler::Outcome e)
{
	if (e == Scheduler::Outcome::OK && Scheduler::isInsideRadioHours()) Weather::markDirty();
}

/** Chiede il ridisegno quando il task ha portato dati nuovi. */
static void markDirtyOnOk(Scheduler::Outcome e)
{
	if (e == Scheduler::Outcome::OK) Weather::markDirty();
}

/** Il pannello si ridisegna solo con dati nuovi da mostrare, e non prima che
 *  la rete abbia dato un esito o sia scaduta l'attesa del primo frame. */
static bool displayReady()
{
	return Weather::sporco() && Scheduler::isFirstFrameAllowed();
}

/** render() azzera da se' il flag dei dati nuovi. */
static Scheduler::Outcome runDisplayRender()
{
	Weather::render();
	return Scheduler::Outcome::OK;
}

/** Il display non si auto-sporca: senza questo hook il ridisegno si
 *  richiederebbe da solo a ogni giro. */
static void onDisplayOutcome(Scheduler::Outcome) {}

/**
 * Tutti i flussi temporizzati del dispositivo. L'ordine delle righe e'
 * l'ordine di esecuzione dentro la rispettiva fase, e non e' arbitrario:
 *
 *   - mail prima di Google perche' i due condividono la cache del token
 *     OAuth: chi gira per primo paga il refresh;
 *   - Google prima di Outlook perche' il token appena rinfrescato e' il suo;
 *   - la telemetria Tuya prima del cinema: i secondi del suo handshake TLS
 *     sono altro tempo di copertura del boot di render.com;
 *   - cinema per ultimo perche' e' l'unico che puo' pagare il cold start di
 *     render.com, e il tempo di rete degli altri e' la copertura di quel
 *     boot; il suo onRoundStart() manda il ping di sveglia all'inizio del giro;
 *   - il sensore precede il display cosi' un campione appena prodotto entra
 *     nel frame dello stesso giro.
 *
 * Tuya è l'unico task di rete con maxAttempts a 0, cioè un tentativo per slot
 * comunque vada: i suoi modi di fallimento sono permanenti sulla scala dei
 * minuti (credenziali, region, device model) e ogni ritento costerebbe un
 * altro handshake TLS senza cambiare l'esito. Ed è l'unico con runsOutsideRadioHours
 * potenzialmente a 1, vedi TUYA_IGNORE_ACTIVE_HOUR in testa al file.
 */
static Scheduler::Task TASK_TABLE[] = {
	{"meteo", Scheduler::Cadence::PERIODIC, true, false,
	 &Timings::Valori::owmMin, nullptr,
	 FETCH_RITENTO_MS, FETCH_MAX_TENTATIVI,
	 runWeatherFetch, weatherReady, isWeatherIncomplete, nullptr, markDirtyOnOk, nullptr},

	{"mail", Scheduler::Cadence::PERIODIC, true, false,
	 &Timings::Valori::mailMin, nullptr,
	 FETCH_RITENTO_MS, FETCH_MAX_TENTATIVI,
	 runMailFetch, mailReady, nullptr, nullptr, markDirtyOnOk, nullptr},

	{"google", Scheduler::Cadence::PERIODIC, true, false,
	 &Timings::Valori::googleMin, nullptr,
	 FETCH_RITENTO_MS, FETCH_MAX_TENTATIVI,
	 runGoogleCalendarFetch, googleReady, nullptr, nullptr, markDirtyOnOk, nullptr},

	{"outlook", Scheduler::Cadence::PERIODIC, true, false,
	 &Timings::Valori::outlookMin, nullptr,
	 FETCH_RITENTO_MS, FETCH_MAX_TENTATIVI,
	 runOutlookCalendarFetch, outlookReady, nullptr, nullptr, markDirtyOnOk, nullptr},

	{"tuya", Scheduler::Cadence::PERIODIC, true, TUYA_IGNORE_ACTIVE_HOUR,
	 &Timings::Valori::tuyaMin, nullptr,
	 FETCH_RITENTO_MS, 0,
	 runTuyaPublish, tuyaReady, nullptr, nullptr, onTuyaOutcome, nullptr},

	{"cinema", Scheduler::Cadence::DAILY, true, false,
	 nullptr, &Timings::Valori::cinemaOra,
	 FETCH_RITENTO_MS, FETCH_MAX_TENTATIVI,
	 runCinemaFetch, cinemaReady, nullptr, prewarmCinemaServer, markDirtyOnOk, nullptr},

	{"bsec", Scheduler::Cadence::PERIODIC, false, false,
	 &Timings::Valori::displayRefreshMin, nullptr,
	 FETCH_RITENTO_MS, 0,
	 runIndoorSample, Indoor::attivo, nullptr, nullptr, onIndoorOutcome, msUntilIndoorPoll},

	{"display", Scheduler::Cadence::PERIODIC, false, false,
	 &Timings::Valori::displayRefreshMin, nullptr,
	 FETCH_RITENTO_MS, 0,
	 runDisplayRender, displayReady, nullptr, nullptr, onDisplayOutcome, nullptr},
};

static constexpr uint8_t TASK_COUNT = sizeof(TASK_TABLE) / sizeof(TASK_TABLE[0]);

/**
 * Disegna, dentro il loop paged di Weather::renderFrame(), le sole righe del
 * wallpaper che cadono nella pagina corrente: l'immagine in flash se c'è,
 * altrimenti il fallback PROGMEM. Le righe si tagliano anche all'altezza
 * CINEMA_H, e il descrittore ridotto punta agli stessi dati, senza copie.
 */
void drawTestBackground()
{
	const GxEPDImage::Descriptor *stored = CinemaStore::image();
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
}

void setup()
{
	Serial.begin(115200);
	delay(1000);

	// Prima di ogni modulo: le cadenze salvate in NVS vanno lette e portate
	// dentro i limiti prima che qualcuno le usi.
	Timings::begin();
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
	 * Archivio in flash dell'immagine cinema: l'ultima scaricata torna a schermo
	 * già dal primo frame. Nessuna rete qui dentro.
	 */
	CinemaStore::begin();
	/**
	 * Apre subito la finestra OTA (OTA_WINDOW_MIN): AP per l'aggiornamento firmware +
	 * STA in parallelo per il fetch meteo. Scaduta la finestra, loop() chiamera'
	 * Maintenance::chiudi() che spegne AP e WiFi, ripristinando
	 * il ciclo originario STA on-demand + light sleep DISPLAY_REFRESH_MIN.
	 */
	Maintenance::begin();
	Scheduler::begin(TASK_TABLE, TASK_COUNT, wifiOn, wifiOff);
	// Riferimento temporale per il timeout di boot WiFi (vedi PRIMO_FRAME_ATTESA_MS).
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
 * comportamento energetico ordinario: STA acceso solo per i fetch dovuti e
 * spento subito dopo, con light sleep fino al prossimo evento dello scheduler.
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
		 * refresh lascerebbero il web server muto per decine di secondi.
		 */
		if (!Maintenance::clientAttivo())
			Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE);
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

#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_sleep.h>
#include <esp_heap_caps.h>

/**
 * Necessario per Adafruit_GFX per disegnare testo e linee
 * Costo: ~15 KB di flash in piu'
 */
#define ENABLE_GxEPD2_GFX 1
/**
 * Necessaria per GxEPD2 per indicare che il display usa il bus HSPI
 * (non il VSPI di default su ESP32).
 * Obbligatorio perchè la Waveshare E-Paper ESP32 Driver Board collega SCK/MISO/MOSI ai pin HSPI (13/12/14).
 */
#define USE_HSPI_FOR_EPD

// ---------------------------------------------------------------------------
// Cadenze operative del dispositivo. Tutti i valori sono in minuti interi
// (WIFI_ACTIVE_HOUR_* sono ore locali).
//
// Il sampling BME680 (BSEC ULP, 5 min) NON è configurabile da qui: fissato nel modulo Indoor.h
// I #define vanno dichiarati PRIMA degli include di Weather.h/Calendar.h/Ota.h
// cosi' che i loro fallback #ifndef li raccolgano.
// ---------------------------------------------------------------------------

// Refresh display (light sleep del loop principale)
#define DISPLAY_REFRESH_MIN 5

// Cadenze fetch API (minuti)
#define WEATHER_FORECAST_FETCH_MIN 10
#define CAL_OUTLOOK_FETCH_MIN 10
#define CAL_GOOGLE_FETCH_MIN 10
#define MAIL_GOOGLE_FETCH_MIN 10 // Cadenza separata e indipendente dai calendari. Lasciata uguale a CAL_GOOGLE_FETCH_MIN per non moltiplicare i risvegli WiFi
#define OTA_WINDOW_MIN 3

/**
 * Tentativi consecutivi falliti oltre i quali un fetch calendario
 * (Outlook/Google) "consuma" lo slot e attende CAL_*_FETCH_MIN prima
 * di ritentare. Evita hammering degli endpoint OAuth durante la
 * finestra OTA, dove il loop gira ogni ~10ms.
 */
#define MAX_CALENDAR_ATTEMPTS 2

// Local timezone WiFi window
#define WIFI_ACTIVE_HOUR_START 7 // 7:00
#define WIFI_ACTIVE_HOUR_END 23	 // 23:59

/**
 * Timeout di boot per la connessione WiFi (millisecondi). Se entro questo
 * tempo dal setup() la STA non è WL_CONNECTED, il primo refresh del display
 * viene comunque sbloccato con i soli dati gia' disponibili (BME680 indoor +
 * placeholder "--" per meteo/calendari/cinema). Coerente col timeout di
 * wifiOn() usato fuori finestra OTA.
 */
#define BOOT_WIFI_TIMEOUT_MS 15000UL

/** Ora locale del fetch giornaliero immagine cinema. Pensata per cadere
 *  alla prima connessione utile della mattina, ma intenzionalmente separata
 *  da WIFI_ACTIVE_HOUR_START cosi' fetch cinema e finestra WiFi possono
 *  essere spostati indipendentemente. */
#define CINEMA_DAILY_FETCH_HOUR 7 // 7:00

#include <GxEPD2_3C.h>
#include "GxEPD2_SOLUM_097c_960x672/GxEPD2_SOLUM_097c_960x672.h" //Custom driver

// Fallback wallpaper offline: immagine PROGMEM
#include "img_wallpaper/img_apple_bwry.h" //img_apple_bwry_desc

/** Weather.h contiene logica, fetch OpenWeather One Call 3.0, cache,
 * rendering banner. Il .ino si limita ad accendere/spegnere il WiFi
 * al momento giusto e a chiamare Weather::render() per aggiornare il
 * display.
 *
 * include transitivamente Calendar.h e Indoor.h
 * => basta questo piu' Ota.h  per avere tutte le API dei moduli */
#include "Weather.h"
#include "Ota.h"
#include "Mail.h"
#include "Env.h"

SPIClass hspi(HSPI);

GxEPD2_3C<GxEPD2_SOLUM_097c_960x672, GxEPD2_SOLUM_097c_960x672::HEIGHT / 8> display(
	GxEPD2_SOLUM_097c_960x672(15, 27, 26, 25));

// Inizializza il bus HSPI e il display in orientamento landscape fisso.
// Chiamato una sola volta in setup().
void initDisplay()
{
	hspi.begin(13, 12, 14, 15); // SCK, MISO, MOSI, SS
	/**
	 * 10 MHz è il massimo raccomandato per i pannelli SSD1677 sul cablaggio
	 * della Waveshare ESP32 Driver Board senza adattamenti.
	 */
	display.epd2.selectSPI(hspi, SPISettings(10000000, MSBFIRST, SPI_MODE0));
	display.init(115200, true, 2, false);
	display.setRotation(0); // landscape 960x672 nativo
	display.setFullWindow();
}

// ===========================================================================
// Background cinema
//
// Area disponibile per l'immagine: 620x440 (a sinistra della sidebar del
// calendario x=620..960, 20px di margine verticale rispetto al banner
// previsioni che inizia a y=460). L'immagine va disegnata a (0,0).
//
// Flusso:
//   1. Boot: g_cinema_desc punta a img_apple_bwry_desc (fallback PROGMEM).
//   2. Al primo ciclo con WiFi connesso, dopo il fetch meteo e prima di
//      quello dei calendari, fetchCinemaImage() scarica i 3 piani BWRY
//      dall'endpoint render.com e li mette in RAM (o PSRAM se disponibile).
//   3. g_cinema_desc viene riassegnato al descrittore dinamico che punta ai
//      buffer RAM: da qui in poi ogni refresh del display mostra l'immagine
//      scaricata, senza ulteriori chiamate HTTP.
//   4. Se il fetch fallisce, g_cinema_desc resta sul fallback PROGMEM.
// ===========================================================================

static constexpr const char *CINEMA_URL =
	"https://cinema-epd.onrender.com/cinema/arduino?width=620&height=440&colors=bwry&dither=floyd";

/**
 * Dimensioni dell'area wallpaper. NON sono un viewport che ritaglia:
 * GxEPDImage::showImage() disegna pixel per pixel da (0,0) usando la
 * width/height del Descriptor, senza alcun clipping rispetto a queste
 * costanti. Significa che la sorgente (sia il dinamico fetchato dal
 * server cinema sia il fallback PROGMEM in img_wallpaper/img_apple_bwry.h)
 * deve essere generata gia' a 620x440 esatti.
 * Una sorgente piu' alta di 440 invade la fascia bianca 440..460 di
 * separazione e arriva fino al banner (BANNER_Y=460); una sorgente piu'
 * larga di 620 entra nella sidebar del calendario (SIDEBAR_X=620), che
 * la copre con fillRect bianco solo fino a y=460.
 */
static constexpr int16_t CINEMA_W = 620;
static constexpr int16_t CINEMA_H = 440;
static constexpr uint16_t CINEMA_STRIDE = (CINEMA_W + 7) / 8;					// 78
static constexpr uint32_t CINEMA_PLANE_SZ = (uint32_t)CINEMA_STRIDE * CINEMA_H; // 34320
static constexpr uint32_t CINEMA_TOTAL_SZ = CINEMA_PLANE_SZ * 3;				// 102960

// Buffer dinamici dei 3 piani scaricati (nullptr finchè il fetch non riesce).
static uint8_t *g_cinema_black = nullptr;
static uint8_t *g_cinema_red = nullptr;
static uint8_t *g_cinema_yellow = nullptr;

// Descrittore dinamico che punta ai buffer sopra. Popolato quando il fetch
// ha successo.
static GxEPDImage::Descriptor g_cinema_dynamic_desc = {
	GxEPDImage::FORMAT_BWRY_1BPP,
	CINEMA_W,
	CINEMA_H,
	nullptr,
	nullptr,
	nullptr,
};

// Puntatore all'immagine correntemente visualizzata: fallback PROGMEM al
// boot, rimappato al descrittore dinamico dopo un fetch riuscito.
static const GxEPDImage::Descriptor *g_cinema_desc = &img_apple_bwry_desc;

// Stato del fetch cinema:
//   - g_cinema_attempted: true dopo il primo tentativo (successo o errore).
//     Gate il trigger "primo boot".
//   - g_cinema_last_fetch_day: day-of-year (tm_yday, 0..365) dell'ultimo
//     tentativo registrato con NTP attivo. Gate il trigger "daily
//     CINEMA_DAILY_FETCH_HOUR": nessun fetch due volte nello stesso giorno.
//
// Entrambi i flag vengono settati SUBITO dopo il check WiFi dentro
// fetchCinemaImage(), PRIMA di qualunque operazione che possa fallire.
// Motivazioni:
//   - durante la finestra OTA il loop() gira ogni ~10ms (delay(10) alla
//     fine del ramo Ota::windowOpen()). Se gestissimo i flag solo sul
//     successo, un fallimento del fetch causerebbe retry ogni 10ms
//     hammerando render.com.
//   - se il fetch fallisce si mostra il fallback PROGMEM fino al prossimo
//     trigger valido (daily al prossimo CINEMA_DAILY_FETCH_HOUR local,
//     oppure reboot).
//   - se WiFi non si connette mai, i flag restano invariati e
//     fetchCinemaImage() early-return sul check WiFi: appena la radio
//     sale, il tentativo parte regolarmente.
static bool g_cinema_attempted = false;
static int g_cinema_last_fetch_day = -1;

// Timestamp di boot (millis() al termine di setup()): usato per misurare il
// timeout BOOT_WIFI_TIMEOUT_MS oltre il quale, se WiFi non si è ancora
// connesso, si sblocca comunque il primo refresh del display.
static uint32_t g_boot_start_ms = 0;

/**
 * Alloca un buffer da CINEMA_PLANE_SZ byte, preferendo PSRAM se disponibile.
 * Ritorna nullptr su OOM. Logga la provenienza del buffer per diagnostica.
 */
static uint8_t *allocPlaneBuffer(const char *label)
{
	uint8_t *p = nullptr;
	if (psramFound())
	{
		p = (uint8_t *)heap_caps_malloc(CINEMA_PLANE_SZ, MALLOC_CAP_SPIRAM);
		if (p)
		{
			Serial.printf("[cinema] %s: alloc %u byte in PSRAM\n", label, (unsigned)CINEMA_PLANE_SZ);
			return p;
		}
		Serial.printf("[cinema] %s: PSRAM alloc fallita, provo heap interno\n", label);
	}
	p = (uint8_t *)malloc(CINEMA_PLANE_SZ);
	if (p)
		Serial.printf("[cinema] %s: alloc %u byte in heap interno (free: %u)\n",
					  label, (unsigned)CINEMA_PLANE_SZ, (unsigned)ESP.getFreeHeap());
	else
		Serial.printf("[cinema] %s: allocazione fallita (%u byte richiesti, %u disponibili)\n",
					  label, (unsigned)CINEMA_PLANE_SZ, (unsigned)ESP.getFreeHeap());
	return p;
}

/**
 * Libera tutti i buffer dei piani. Chiamata in caso di errore HTTP/parsing
 * per non tenere memoria occupata inutilmente.
 */
static void freeCinemaBuffers()
{
	free(g_cinema_black);
	g_cinema_black = nullptr;
	free(g_cinema_red);
	g_cinema_red = nullptr;
	free(g_cinema_yellow);
	g_cinema_yellow = nullptr;
	g_cinema_dynamic_desc.data0 = nullptr;
	g_cinema_dynamic_desc.data1 = nullptr;
	g_cinema_dynamic_desc.data2 = nullptr;
}

/**
 * Decide se è il momento di fare un fetch dell'immagine cinema.
 *
 * Due trigger possibili:
 *   1. PRIMO BOOT: se non abbiamo mai tentato (g_cinema_attempted = false)
 *      in questa sessione, fetch immediato al primo giro con WiFi up.
 *   2. DAILY REFRESH: una volta al giorno, alla prima finestra WiFi
 *      dell'hour CINEMA_DAILY_FETCH_HOUR local (apertura della finestra
 *      WiFi mattutina), cosi' la locandina è fresca dal primo accesso
 *      del giorno.
 *
 * Richiede NTP sincronizzato per il trigger daily: senza data/ora corrette
 * non possiamo sapere se siamo nell'ora target e se abbiamo gia' fetchato oggi.
 */
static bool shouldFetchCinema()
{
	if (!g_cinema_attempted)
		return true;
	time_t now = time(nullptr);
	if (now < 100000L)
		return false; // NTP non ancora pronto
	struct tm t;
	localtime_r(&now, &t);
	// Trigger daily spostato dalla sera (23:00) al mattino (CINEMA_DAILY_FETCH_HOUR):
	// pre-warm render.com allineato alla prima connessione utile della giornata.
	if (t.tm_hour != CINEMA_DAILY_FETCH_HOUR)
		return false;
	if (t.tm_yday == g_cinema_last_fetch_day)
		return false; // gia' fatto oggi
	return true;
}

/**
 * Scarica l'immagine cinema dal server render.com. Due trigger (vedi
 * shouldFetchCinema): primo boot + daily refresh all'ora
 * CINEMA_DAILY_FETCH_HOUR local.
 * Chiamata DOPO il fetch meteo e PRIMA dei fetch calendari.
 *
 * Sequenza:
 *   1. Early return se shouldFetchCinema() nega (gia' tentato / non è il
 *      momento / NTP non pronto).
 *   2. Early return se WiFi non connesso: fallback PROGMEM fino al prossimo
 *      giro con WiFi up.
 *   3. Aggiorna g_cinema_attempted e g_cinema_last_fetch_day SUBITO -> da
 *      qui in poi nessun retry nello stesso giorno, indipendentemente
 *      dall'esito (vedi commento sui flag).
 *   4. Libera i buffer dell'immagine precedente (se presenti da un fetch
 *      riuscito in un giro precedente) e riporta g_cinema_desc al fallback
 *      PROGMEM: se il fetch fallisce o il refresh avviene durante un
 *      render, il display mostra il fallback invece di un'immagine corrotta.
 *   5. Alloca 3 buffer da CINEMA_PLANE_SZ byte (PSRAM preferita, heap
 *      interno come fallback).
 *   6. HTTP GET -> verifica status 200 e Content-Length == CINEMA_TOTAL_SZ.
 *   7. Legge in stream i 3 piani in sequenza (black, red, yellow) via
 *      readBytes, direttamente nei buffer.
 *   8. Ripuntamento di g_cinema_desc al descrittore dinamico.
 *
 * In caso di qualunque errore (OOM, HTTP != 200, size mismatch, read short)
 * libera i buffer e mantiene il fallback PROGMEM fino al prossimo trigger
 * valido (daily al prossimo CINEMA_DAILY_FETCH_HOUR local, oppure reboot).
 */
static void fetchCinemaImage()
{
	if (!shouldFetchCinema())
		return;
	if (WiFi.status() != WL_CONNECTED)
		return;
	// Marca il tentativo SUBITO (prima di qualunque alloc/HTTP): se qualcosa
	// fallisce nelle righe successive, non si ritenta nello stesso giorno.
	g_cinema_attempted = true;
	{
		time_t now = time(nullptr);
		if (now > 100000L)
		{
			struct tm t;
			localtime_r(&now, &t);
			g_cinema_last_fetch_day = t.tm_yday;
		}
	}
	// Libera l'immagine precedente (no-op al primo boot) e ripristina il
	// fallback PROGMEM come "immagine corrente" finchè il nuovo download
	// non completa con successo.
	freeCinemaBuffers();
	g_cinema_desc = &img_apple_bwry_desc;

	Serial.printf("[cinema] fetching %s\n", CINEMA_URL);
	Serial.printf("[cinema] PSRAM %s, free heap: %u byte\n",
				  psramFound() ? "presente" : "assente (uso heap interno)",
				  (unsigned)ESP.getFreeHeap());

	g_cinema_black = allocPlaneBuffer("black");
	g_cinema_red = allocPlaneBuffer("red");
	g_cinema_yellow = allocPlaneBuffer("yellow");
	if (!g_cinema_black || !g_cinema_red || !g_cinema_yellow)
	{
		Serial.println(F("[cinema] allocazione buffer fallita, fallback PROGMEM"));
		freeCinemaBuffers();
		return;
	}

	HTTPClient http;
	// 45s: margine per cold start render.com free tier in caso il cron
	// GitHub Actions di pre-warm non sia stato eseguito (vedi
	// webapp/.github/workflows/keep-warm.yml). Cold start tipico 10-30s.
	http.setTimeout(45000);
	if (!http.begin(CINEMA_URL))
	{
		Serial.println(F("[cinema] HTTP begin fallita"));
		freeCinemaBuffers();
		return;
	}
	int code = http.GET();
	if (code != 200)
	{
		Serial.printf("[cinema] HTTP status %d, fallback PROGMEM\n", code);
		http.end();
		freeCinemaBuffers();
		return;
	}
	int size = http.getSize();
	if (size != (int)CINEMA_TOTAL_SZ)
	{
		Serial.printf("[cinema] Content-Length %d atteso %u, fallback PROGMEM\n",
					  size, (unsigned)CINEMA_TOTAL_SZ);
		http.end();
		freeCinemaBuffers();
		return;
	}

	WiFiClient *stream = http.getStreamPtr();
	// readBytes() blocca fino a riempimento del buffer richiesto, timeout
	// (controllato da setTimeout) o EOF. Niente polling manuale di available()
	// con delay(1): readBytes lo fa gia' internamente in modo equivalente.
	stream->setTimeout(45000);
	uint8_t *const planes[3] = {g_cinema_black, g_cinema_red, g_cinema_yellow};
	const char *names[3] = {"black", "red", "yellow"};
	for (int p = 0; p < 3; ++p)
	{
		size_t read = 0;
		uint32_t t0 = millis();
		// Wall-clock guard 45s per piano: se readBytes ritorna in modo
		// frazionario (n>0 ma < richiesto) e la rete è lenta, evita di
		// accumulare timeout >45s totali sul singolo piano.
		while (read < CINEMA_PLANE_SZ && (millis() - t0) < 45000UL)
		{
			int n = stream->readBytes(planes[p] + read, CINEMA_PLANE_SZ - read);
			if (n <= 0)
				break; // timeout interno o connessione chiusa
			read += n;
		}
		if (read != CINEMA_PLANE_SZ)
		{
			Serial.printf("[cinema] piano %s letto parzialmente (%u/%u)\n",
						  names[p], (unsigned)read, (unsigned)CINEMA_PLANE_SZ);
			http.end();
			freeCinemaBuffers();
			return;
		}
	}
	http.end();

	g_cinema_dynamic_desc.data0 = g_cinema_black;
	g_cinema_dynamic_desc.data1 = g_cinema_red;
	g_cinema_dynamic_desc.data2 = g_cinema_yellow;
	g_cinema_desc = &g_cinema_dynamic_desc;
	Weather::markDirty();
	Serial.println(F("[cinema] download completato, immagine remappata"));
}

/**
 * Disegna il background corrente (PROGMEM fallback o dinamico scaricato)
 * dentro il paged loop di Weather::renderFrame().
 *
 * GxEPDImage::showImage() è compatibile con entrambi i tipi di buffer:
 * su ESP32 pgm_read_byte() è una normale dereferenza, funziona identica
 * su PROGMEM e su RAM/PSRAM.
 *
 * --- ESEMPIO USO CON IMMAGINE PROGMEM (storica, commentato) --------------
 *   // Se in futuro vuoi tornare a un'immagine PROGMEM hardcoded (es.
 *   // slideshow multi-immagine generato da epd_image_converter.pyw):
 *   //
 *   // #include "img_wallpaper/my_image.h"   // genera my_image_desc
 *   // GxEPDImage::showImage(display, my_image_desc);
 *   //
 *   // Per piu' immagini in rotazione:
 *   // static const GxEPDImage::Descriptor* const IMGS[] = {
 *   //   &img1_desc, &img2_desc, &img3_desc,
 *   // };
 *   // GxEPDImage::showImage(display, *IMGS[idx % 3]);
 * -------------------------------------------------------------------------
 */
void drawTestBackground()
{
	GxEPDImage::showImage(display, *g_cinema_desc);
}

/**
 * Accende il WiFi in modalita' STA e attende la connessione con timeout di 15 s.
 * La radio resta accesa solo per la finestra di fetch: viene spenta
 * da wifiOff() subito dopo.
 * @return true se connesso entro il timeout.
 */
static bool wifiOn()
{
	WiFi.mode(WIFI_STA);
	WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
	uint32_t t0 = millis();
	while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 15000UL)
	{
		delay(100);
	}
	if (WiFi.status() == WL_CONNECTED)
	{
		Serial.print(F("[WiFi] connected, IP="));
		Serial.println(WiFi.localIP());
		return true;
	}
	Serial.println(F("[WiFi] connection timeout"));
	return false;
}

/**
 * Spegne completamente il WiFi (disconnect + WIFI_OFF) per minimizzare il
 * consumo fra un fetch e l'altro: ~120 mA attivo vs ~30 mA a radio spenta.
 *
 * disconnect(true, false) [wifioff=true, eraseap=false]: spegne la radio ma
 * NON cancella SSID/BSSID/channel salvati. Il prossimo wifiOn() puo' usare
 * la cache per riconnettersi al BSSID noto saltando lo scan completo
 * (potenziale risparmio 0-1500 ms su WiFi.begin()). Le credenziali in NVS
 * non vanno comunque cancellate: ad ogni begin() vengono ripassate dai
 * #define di Env.h.
 */
static void wifiOff()
{
	WiFi.disconnect(true, false);
	WiFi.mode(WIFI_OFF);
}

/**
 * Restituisce true se l'ora locale rientra nella finestra di fetch
 * (WIFI_ACTIVE_HOUR_START..WIFI_ACTIVE_HOUR_END). Fuori da questa fascia
 * il WiFi resta spento. Se NTP non è ancora sincronizzato lascia passare
 * per permettere il primo sync all'avvio.
 */
static bool isActiveHour()
{
	time_t now = time(nullptr);
	if (now < 100000L)
		return true; // NTP non ancora pronto
	struct tm t;
	localtime_r(&now, &t);
	return t.tm_hour >= WIFI_ACTIVE_HOUR_START && t.tm_hour <= WIFI_ACTIVE_HOUR_END;
}

void setup()
{
	Serial.begin(115200);
	delay(1000);
	initDisplay();
	/**
	 * TZ Europe/Rome con DST automatico: va fatto PRIMA di qualsiasi
	 * modulo che formatti orari locali (Weather/Calendar usano localtime_r).
	 */
	Calendar::initTimezone();
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
	 * Apre subito la finestra OTA (OTA_WINDOW_MIN): AP per l'aggiornamento firmware +
	 * STA in parallelo per il fetch meteo. Scaduta la finestra, loop() chiamera'
	 * Ota::endNow() che spegne l'AP e mette WiFi in WIFI_OFF, ripristinando
	 * il ciclo originario STA on-demand + light sleep DISPLAY_REFRESH_MIN.
	 */
	Ota::begin();
	// Riferimento temporale per il timeout di boot WiFi (vedi BOOT_WIFI_TIMEOUT_MS).
	g_boot_start_ms = millis();
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
 * Scaduta la finestra, Ota::endNow() spegne AP e WiFi e il ciclo torna al
 * comportamento energetico originale: STA acceso solo poco prima del fetch
 * e spento subito dopo, con light sleep DISPLAY_REFRESH_MIN minuti fra un
 * giro e l'altro (tempo minimo fra un refresh del display e il successivo).
 *
 * @modified 21/04/26 light sleep 60 s -> 5 min + Indoor::refresh() agganciato
 */
void loop()
{
	if (Ota::windowOpen())
	{
		Ota::handle();
		/**
		 * La STA è gia' in risalita grazie ad AP_STA: quando è connessa, smaltiamo
		 * i fetch pendenti senza passare per wifiOn()/wifiOff()
		 * (che spegnerebbero anche l'AP in corso)
		 */
		if (WiFi.status() == WL_CONNECTED)
		{
			Weather::FetchKind need = Weather::pendingFetch();
			if (need != Weather::FETCH_NONE)
				Weather::runFetch(need);

			fetchCinemaImage();

			/**
			 * Mail PRIMA dei calendari. Best-effort: se Mail::runFetch() fallisce
			 * (WiFi cade, batch HTTP error, budget esaurito) o se l'inbox e'
			 * vuota, il flusso prosegue normalmente con i fetch calendario:
			 * un problema mail NON deve impattare meteo/calendari/cinema.
			 * Niente markDirty: per ora le mail non sono renderizzate.
			 */
			if (Mail::pendingFetch())
				Mail::runFetch();

			// Aggancio Outlook + Google al fetch corrente del meteo (stessa finestra WiFi)
			if ((need & Weather::FETCH_CURRENT_WEATHER) || Calendar::Outlook::pendingFetch())
				if (Calendar::Outlook::runFetch())
					Weather::markDirty();
			if ((need & Weather::FETCH_CURRENT_WEATHER) || Calendar::Google::pendingFetch())
				if (Calendar::Google::runFetch())
					Weather::markDirty();
			/**
			 * Sblocca il gate del primo refresh dopo il primo tentativo di fetch:
			 * cosi' il display viene disegnato non appena il meteo viene scaricato, anche
			 * se la chiamata One Call è fallita (placeholder "--" per i dati mancanti).
			 * No-op dopo il primo refresh.
			 */
			Weather::forceFirstRender();
		}
		else if ((millis() - g_boot_start_ms) >= BOOT_WIFI_TIMEOUT_MS)
		{
			/**
			 * WiFi non connesso entro BOOT_WIFI_TIMEOUT_MS dal boot:
			 * primo refresh con i soli dati disponibili
			 * dati indoor del BME680 + placeholder "--" per meteo / calendari / cinema.
			 * e immagine offline al posto di quella scaricata dal server cinema.
			 */
			Weather::forceFirstRender();
		}
		// BME680: se arriva un nuovo sample ULP durante la finestra OTA
		// (tipicamente solo uno a ridosso del minuto 5) forziamo il refresh.
		if (Indoor::refresh())
			Weather::markDirty();
		Weather::render();
		delay(10); // necessario per handleClient(); nessun light sleep durante OTA
		return;
	}

	// Finestra chiusa: garantisce che AP e WebServer siano giu' (idempotente).
	Ota::endNow();

	Weather::FetchKind need = Weather::pendingFetch();
	bool needOutlook = Calendar::Outlook::pendingFetch();
	bool needGoogle = Calendar::Google::pendingFetch();
	// Mail aggiunto al gate: se solo le mail sono in scadenza, accendiamo
	// comunque WiFi per scaricarle (best-effort, non blocca gli altri fetch).
	bool needMail = Mail::pendingFetch();
	if (need != Weather::FETCH_NONE || needOutlook || needGoogle || needMail)
	{
		if (isActiveHour())
		{
			if (wifiOn())
			{
				if (need != Weather::FETCH_NONE)
					Weather::runFetch(need);
				// Cinema: una tantum, tra meteo e calendari.
				fetchCinemaImage();
				/**
				 * Mail PRIMA dei calendari. Best-effort: se Mail::runFetch()
				 * fallisce (WiFi cade, batch HTTP error, budget esaurito) o
				 * se l'inbox e' vuota, il flusso prosegue normalmente con i
				 * fetch calendario: un problema mail NON deve impattare
				 * meteo/calendari/cinema. Niente markDirty: no UI per ora.
				 */
				if (needMail)
					Mail::runFetch();
				// Outlook + Google: stessa finestra WiFi del fetch meteo corrente
				if ((need & Weather::FETCH_CURRENT_WEATHER) || needOutlook)
					if (Calendar::Outlook::runFetch())
						Weather::markDirty();
				if ((need & Weather::FETCH_CURRENT_WEATHER) || needGoogle)
					if (Calendar::Google::runFetch())
						Weather::markDirty();
			}
			else
			{
				/**
				 * Timeout WiFi: sblocca il gate del primo refresh cosi' il display
				 * viene comunque disegnato (con placeholder "--" dove mancano dati)
				 * invece di restare nello stato lasciato da display.init(). No-op
				 * dopo il primo refresh.
				 */
				Weather::forceFirstRender();
			}
			wifiOff(); // chiamata anche su fallimento: assicura radio spenta
		}
	}

	// BME680: BSEC2 in ULP produce un sample ogni 5 min. Chiamato ad ogni
	// wake, ritorna true solo al tick dovuto; quando scatta forziamo il
	// refresh del display per far ridisegnare i dati indoor appena saranno
	// aggiunti alla UI.
	if (Indoor::refresh())
		Weather::markDirty();

	Weather::render();

	/**
	 * Light sleep DISPLAY_REFRESH_MIN minuti: preserva RAM e stato dei moduli
	 * (Weather, BSEC, TZ). Il BSEC ULP campiona ogni 5 min indipendentemente
	 * dal valore scelto qui: se DISPLAY_REFRESH_MIN < 5 alcuni wake non
	 * produrranno nuovi sample indoor; se > 5 il sample piu' recente viene
	 * comunque raccolto al wake successivo. Le cadenze fetch (meteo/calendari,
	 * tipicamente piu' lunghe) vengono valutate ad ogni wake via pendingFetch().
	 */
	esp_sleep_enable_timer_wakeup((uint64_t)DISPLAY_REFRESH_MIN * 60ULL * 1000ULL * 1000ULL); // us
	esp_light_sleep_start();
}

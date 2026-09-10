#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_sleep.h>
#include <esp_heap_caps.h>

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
 * A 0 il firmware lavora solo in full-window e non chiama nessuna delle cinque
 * API opt-in del driver: drawImagePartial(), refreshPartial(),
 * writeImagePrevious(), writeScreenBufferPrevious(), setPartialLut().
 * Lo static_assert dopo la costruzione di `display` sorveglia l'unica strada
 * per cui il partial potrebbe attivarsi da sè, cioè il flag
 * hasFastPartialUpdate del driver.
 *
 * Per riabilitarlo in futuro NON basta mettere 1: vedi il messaggio dell'#error
 * accanto allo static_assert, che elenca cosa va implementato.
 */
#define DISPLAY_PARTIAL_REFRESH 0

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
 * Timeout di un singolo tentativo di connessione della STA (millisecondi),
 * usato da wifiOn(). Oltre questo tempo il giro prosegue a radio spenta e i
 * fetch pendenti restano tali fino al wake successivo.
 */
#define WIFI_CONNECT_TIMEOUT_MS 15000UL

/**
 * Timeout di boot per la connessione WiFi (millisecondi). Se entro questo
 * tempo dal setup() la STA non è WL_CONNECTED, il primo refresh del display
 * viene comunque sbloccato con i soli dati gia' disponibili (BME680 indoor +
 * placeholder "--" per meteo/calendari/cinema).
 *
 * Deliberatamente uguale a WIFI_CONNECT_TIMEOUT_MS: il gate del primo refresh
 * deve scadere insieme al tentativo di connessione, non prima. Restano due
 * costanti perche' misurano cose diverse (un tentativo di wifiOn() contro il
 * tempo trascorso dal boot) e possono divergere se il criterio cambia.
 */
#define BOOT_WIFI_TIMEOUT_MS 15000UL

/** Ora locale del fetch giornaliero immagine cinema. Pensata per cadere
 *  alla prima connessione utile della mattina, ma intenzionalmente separata
 *  da WIFI_ACTIVE_HOUR_START cosi' fetch cinema e finestra WiFi possono
 *  essere spostati indipendentemente. */
#define CINEMA_DAILY_FETCH_HOUR 7 // 7:00

/**
 * Ping di sveglia al server cinema, che gira sul free tier di render.com:
 * l'istanza viene sospesa dopo 15 min di inattivita' e il boot successivo
 * costa una ventina di secondi. La GET parte prima di meteo, mail e
 * calendari e viene abbandonata subito: conta che la richiesta arrivi al
 * router di render, non la risposta.
 *
 * L'host deve restare uguale a quello di Layout::CINEMA_URL (Layout_097c.h,
 * Layout_122c.h). La', l'URL vive nei layout perche' la query string codifica
 * width/height/colors del pannello; /health non dipende dal pannello e sta
 * quindi qui. Cambiando dominio vanno aggiornati tutti e tre i punti.
 *
 * CINEMA_PREWARM_TIMEOUT_MS e' l'attesa massima della RISPOSTA, non della
 * connessione: l'handshake TLS ha il suo timeout separato, lasciato al
 * default di HTTPClient, perche' senza handshake completo la richiesta non
 * parte affatto. Va tenuto sotto 65535: setTimeout() prende un uint16_t.
 */
#define CINEMA_PREWARM_URL "https://cinema-epd.onrender.com/health"
#define CINEMA_PREWARM_TIMEOUT_MS 1500

#include <GxEPD2_3C.h>
#include "Layout.h"   // dispatcher: include Layout_097c.h o Layout_122c.h in base al #define DISPLAY_VARIANT_*

// Fallback wallpaper offline: immagine PROGMEM
#include "wallpaper/img_apple_bwry.h" //img_apple_bwry_desc

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
 * Il ramo a 0 controlla il flag del driver, che è l'unico modo per cui il
 * partial possa attivarsi senza che nessuno lo chiami: con hasFastPartialUpdate
 * a true il template GxEPD2_3C scrive il piano accent dentro la RAM 0x26
 * (GxEPD2_3C.h:340), che sotto la waveform del partial è il frame precedente, e
 * ripete anche l'intero loop paged una seconda volta (GxEPD2_3C.h:354-358). Il
 * rosso della dashboard andrebbe perso in silenzio: meglio fermare la build.
 *
 * Il ramo a 1 esiste per non far passare il flag come un interruttore che non
 * commuta niente. Riabilitare il partial vuol dire:
 *   1. togliere questo #error;
 *   2. scrivere un percorso di rendering dedicato per le sole zone in bianco e
 *      nero, che chiami epd2.drawImagePartial() FUORI da firstPage()/nextPage():
 *      il partial del driver vive fuori dal template di proposito;
 *   3. accettare che il frame aggiornato in partial non abbia rosso, e decidere
 *      ogni quanti partial rifare un frame pieno per rimetterlo (il driver da sè
 *      non ne ha bisogno: undici passate consecutive non degradano il vetro).
 * NON va alzato hasFastPartialUpdate nel driver: quella è la strada sbagliata,
 * per il motivo scritto sopra.
 */
#if DISPLAY_PARTIAL_REFRESH
#error "DISPLAY_PARTIAL_REFRESH = 1 non è implementato: il partial del driver 097c va chiamato out-of-band con epd2.drawImagePartial(), non dal loop paged, e rende un frame senza rosso. Vedi il commento qui sopra per i tre passi."
#else
static_assert(!Layout::Panel::hasFastPartialUpdate,
			  "Il driver dichiara hasFastPartialUpdate = true, ma DISPLAY_PARTIAL_REFRESH è 0: "
			  "il template GxEPD2_3C userebbe la RAM 0x26 come buffer del partial e il rosso della "
			  "dashboard andrebbe perso. Rimettere false nel driver.");
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
 *   - le cinque API del partial del driver NON vanno chiamate da qui:
 *     drawImagePartial(), refreshPartial(), writeImagePrevious(),
 *     writeScreenBufferPrevious(), setPartialLut(). Sono opt-in, quindi basta
 *     non chiamarle; il perchè sta in DISPLAY_PARTIAL_REFRESH in testa al file.
 *   - il resto lo gestisce il driver da sè: init dei due piani alla prima
 *     scrittura, ricarica della waveform dall'OTP a ogni refresh pieno, e
 *     ripulitura della RAM al risveglio da hibernate().
 */
void initDisplay()
{
	hspi.begin(13, 12, 14, 15); // SCK, MISO, MOSI, SS (HSPI bus, board-specific)
	/**
	 * 10 MHz è il massimo raccomandato per i pannelli SSD1677 sul cablaggio
	 * della Waveshare ESP32 Driver Board senza adattamenti.
	 */
	display.epd2.selectSPI(hspi, SPISettings(10000000, MSBFIRST, SPI_MODE0));
	display.init(115200, true, 2, false);
	display.setRotation(Layout::ROTATION);
	display.setFullWindow();
}

// ===========================================================================
// Background cinema
//
// Area disponibile per l'immagine: Layout::CINEMA_W x Layout::CINEMA_H
// (a sinistra della sidebar calendario x=Layout::SIDEBAR_X..960). La fascia
// y=Layout::CINEMA_H..Layout::BANNER_Y resta bianca per ospitare la futura
// UI mail. L'immagine va disegnata a (x=0, y=0).
// Valori attuali per variante:
//   097c (960w x 672h):  CINEMA = 620w x 300h, fascia mail 160h px
//   122c (960w x 768h):  CINEMA = 620w x 335h, fascia mail 221h px
// Convenzione: NwxMh = N px larghezza x M px altezza.
//
// Flusso:
//   1. Boot: g_cinema_desc punta a img_apple_bwry_desc (fallback PROGMEM).
//   2. Al primo ciclo con WiFi connesso, come ultima chiamata di rete del
//      giro (vedi runNetworkFetches()), fetchCinemaImage() scarica i
//      Layout::CINEMA_PLANES piani dall'endpoint render.com e li mette in
//      RAM (o PSRAM se disponibile).
//   3. g_cinema_desc viene riassegnato al descrittore dinamico che punta ai
//      buffer RAM: da qui in poi ogni refresh del display mostra l'immagine
//      scaricata, senza ulteriori chiamate HTTP.
//   4. Se il fetch fallisce, g_cinema_desc resta sul fallback PROGMEM.
// ===========================================================================

/**
 * URL e dimensioni dell'area wallpaper sono in Layout::CINEMA_URL /
 * CINEMA_W (larghezza X) / CINEMA_H (altezza Y) / CINEMA_STRIDE /
 * CINEMA_PLANE_SZ / CINEMA_TOTAL_SZ.
 * Cambiare display (097c <-> 122c) regola automaticamente sia la query
 * string al server cinema sia la dimensione dei buffer in PSRAM/heap.
 *
 * NOTA: Layout::CINEMA_W (larghezza X) / CINEMA_H (altezza Y) NON sono un viewport che ritaglia.
 * GxEPDImage::showImage() disegna pixel per pixel da (0,0) usando la
 * width/height del Descriptor, senza clipping. La sorgente (dinamica
 * dal server cinema o fallback PROGMEM img_apple_bwry_desc) deve essere
 * generata esattamente a Layout::CINEMA_W (X) x Layout::CINEMA_H (Y). Una sorgente
 * piu' alta invade la fascia bianca fino a Layout::BANNER_Y; una piu'
 * larga entra nella sidebar (Layout::SIDEBAR_X), coperta con fillRect
 * bianco solo fino a Layout::BANNER_Y.
 */

// Buffer dinamici dei piani scaricati (nullptr finchè il fetch non riesce).
// Quanti ne vengono davvero allocati e letti lo dice Layout::CINEMA_PLANES,
// che vale 2 sui pannelli a tre colori: così allocazione, free e lettura
// restano un solo pezzo di codice per entrambe le varianti di display.
// L'array è dimensionato al massimo dei formati serviti dall'endpoint, in
// modo che i tre campi del descrittore siano sempre indicizzabili.
static constexpr uint8_t CINEMA_PLANES_MAX = 3;
static uint8_t *g_cinema_planes[CINEMA_PLANES_MAX] = {};

// Nomi dei piani nell'ordine in cui il server li concatena, per i log.
static const char *const g_cinema_plane_names[CINEMA_PLANES_MAX] = {"black", "red", "yellow"};

static_assert(Layout::CINEMA_PLANES >= 2 && Layout::CINEMA_PLANES <= CINEMA_PLANES_MAX,
			  "Layout::CINEMA_PLANES fuori dai formati serviti da /cinema/arduino");

// Descrittore dinamico che punta ai buffer sopra. Popolato quando il fetch
// ha successo.
static GxEPDImage::Descriptor g_cinema_dynamic_desc = {
	Layout::CINEMA_PLANES >= 3 ? GxEPDImage::FORMAT_BWRY_1BPP
							   : GxEPDImage::FORMAT_BWR_1BPP,
	Layout::CINEMA_W,
	Layout::CINEMA_H,
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
//   - prewarmCinemaServer() legge gli stessi flag attraverso
//     shouldFetchCinema(): ping e fetch condividono il trigger, e il fetch
//     lo chiude settando i flag nello stesso giro in cui il ping e' partito.
static bool g_cinema_attempted = false;
static int g_cinema_last_fetch_day = -1;

// Timestamp di boot (millis() al termine di setup()): usato per misurare il
// timeout BOOT_WIFI_TIMEOUT_MS oltre il quale, se WiFi non si è ancora
// connesso, si sblocca comunque il primo refresh del display.
static uint32_t g_boot_start_ms = 0;

/**
 * Alloca un buffer da Layout::CINEMA_PLANE_SZ byte, preferendo PSRAM se
 * disponibile. Ritorna nullptr su OOM. Logga la provenienza del buffer per
 * diagnostica.
 */
static uint8_t *allocPlaneBuffer(const char *label)
{
	uint8_t *p = nullptr;
	if (psramFound())
	{
		p = (uint8_t *)heap_caps_malloc(Layout::CINEMA_PLANE_SZ, MALLOC_CAP_SPIRAM);
		if (p)
		{
			Serial.printf("[cinema] %s: alloc %u byte in PSRAM\n", label, (unsigned)Layout::CINEMA_PLANE_SZ);
			return p;
		}
		Serial.printf("[cinema] %s: PSRAM alloc fallita, provo heap interno\n", label);
	}
	p = (uint8_t *)malloc(Layout::CINEMA_PLANE_SZ);
	if (p)
		Serial.printf("[cinema] %s: alloc %u byte in heap interno (free: %u)\n",
					  label, (unsigned)Layout::CINEMA_PLANE_SZ, (unsigned)ESP.getFreeHeap());
	else
		Serial.printf("[cinema] %s: allocazione fallita (%u byte richiesti, %u disponibili)\n",
					  label, (unsigned)Layout::CINEMA_PLANE_SZ, (unsigned)ESP.getFreeHeap());
	return p;
}

/**
 * Libera tutti i buffer dei piani. Chiamata in caso di errore HTTP/parsing
 * per non tenere memoria occupata inutilmente.
 */
static void freeCinemaBuffers()
{
	for (uint8_t p = 0; p < Layout::CINEMA_PLANES; ++p)
	{
		free(g_cinema_planes[p]);
		g_cinema_planes[p] = nullptr;
	}
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
	// Trigger daily al mattino: la locandina dev'essere fresca dal primo
	// accesso della giornata. Lo stesso predicato gatea prewarmCinemaServer(),
	// cosi' ping e fetch cadono sempre nello stesso giro.
	if (t.tm_hour != CINEMA_DAILY_FETCH_HOUR)
		return false;
	if (t.tm_yday == g_cinema_last_fetch_day)
		return false; // gia' fatto oggi
	return true;
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
 * intero. Il timeout dell'handshake resta quello di default di HTTPClient e
 * NON va accorciato, altrimenti su rete lenta la richiesta non parte nemmeno.
 * Quello che si abbandona e' la RISPOSTA: con setTimeout() breve la GET
 * ritorna appena scaduta l'attesa degli header, end() chiude il socket e
 * l'istanza prosegue il boot per conto suo. Il chiamante intanto fa meteo,
 * mail e calendari, che e' il tempo di copertura vero di questo meccanismo.
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
	if (!shouldFetchCinema())
		return;
	if (WiFi.status() != WL_CONNECTED)
		return;

	WiFiClientSecure client;
	client.setInsecure();

	HTTPClient http;
	http.setTimeout(CINEMA_PREWARM_TIMEOUT_MS);
	// Nessun keep-alive: end() deve chiudere il socket subito invece di
	// tenerlo aperto per un riuso che non arrivera' mai.
	http.setReuse(false);
	if (!http.begin(client, CINEMA_PREWARM_URL))
	{
		Serial.println(F("[cinema] pre-warm: http.begin fallita"));
		return;
	}
	uint32_t t0 = millis();
	int code = http.GET();
	http.end();
	Serial.printf("[cinema] pre-warm %s -> %d in %lu ms\n",
				  CINEMA_PREWARM_URL, code, (unsigned long)(millis() - t0));
}

/**
 * Scarica l'immagine cinema dal server render.com. Due trigger (vedi
 * shouldFetchCinema): primo boot + daily refresh all'ora
 * CINEMA_DAILY_FETCH_HOUR local.
 * Ultima chiamata di rete del giro, dopo meteo, mail e calendari: e' l'unica
 * che puo' pagare il cold start di render.com, e il tempo speso dagli altri
 * fetch e' il tempo che il server ha per completare il boot avviato da
 * prewarmCinemaServer().
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
 *   5. Alloca Layout::CINEMA_PLANES buffer da Layout::CINEMA_PLANE_SZ byte
 *      (PSRAM preferita,
 *      heap interno come fallback).
 *   6. HTTP GET -> verifica status 200 e Content-Length == Layout::CINEMA_TOTAL_SZ.
 *   7. Legge in stream i piani nell'ordine in cui il server li concatena via
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

	Serial.printf("[cinema] fetching %s\n", Layout::CINEMA_URL);
	Serial.printf("[cinema] PSRAM %s, free heap: %u byte\n",
				  psramFound() ? "presente" : "assente (uso heap interno)",
				  (unsigned)ESP.getFreeHeap());

	for (uint8_t p = 0; p < Layout::CINEMA_PLANES; ++p)
	{
		g_cinema_planes[p] = allocPlaneBuffer(g_cinema_plane_names[p]);
		if (!g_cinema_planes[p])
		{
			Serial.println(F("[cinema] allocazione buffer fallita, fallback PROGMEM"));
			freeCinemaBuffers();
			return;
		}
	}

	HTTPClient http;
	// 45s: margine per il cold start del free tier render.com (una ventina di
	// secondi) quando il ping di prewarmCinemaServer() non e' bastato a
	// completare il boot dell'istanza prima di arrivare qui.
	http.setTimeout(45000);
	if (!http.begin(Layout::CINEMA_URL))
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
	if (size != (int)Layout::CINEMA_TOTAL_SZ)
	{
		Serial.printf("[cinema] Content-Length %d atteso %u, fallback PROGMEM\n",
					  size, (unsigned)Layout::CINEMA_TOTAL_SZ);
		http.end();
		freeCinemaBuffers();
		return;
	}

	WiFiClient *stream = http.getStreamPtr();
	// readBytes() blocca fino a riempimento del buffer richiesto, timeout
	// (controllato da setTimeout) o EOF. Niente polling manuale di available()
	// con delay(1): readBytes lo fa gia' internamente in modo equivalente.
	stream->setTimeout(45000);
	for (uint8_t p = 0; p < Layout::CINEMA_PLANES; ++p)
	{
		size_t read = 0;
		uint32_t t0 = millis();
		// Wall-clock guard 45s per piano: se readBytes ritorna in modo
		// frazionario (n>0 ma < richiesto) e la rete è lenta, evita di
		// accumulare timeout >45s totali sul singolo piano.
		while (read < Layout::CINEMA_PLANE_SZ && (millis() - t0) < 45000UL)
		{
			int n = stream->readBytes(g_cinema_planes[p] + read, Layout::CINEMA_PLANE_SZ - read);
			if (n <= 0)
				break; // timeout interno o connessione chiusa
			read += n;
		}
		if (read != Layout::CINEMA_PLANE_SZ)
		{
			Serial.printf("[cinema] piano %s letto parzialmente (%u/%u)\n",
						  g_cinema_plane_names[p], (unsigned)read, (unsigned)Layout::CINEMA_PLANE_SZ);
			http.end();
			freeCinemaBuffers();
			return;
		}
	}
	http.end();

	g_cinema_dynamic_desc.data0 = g_cinema_planes[0];
	g_cinema_dynamic_desc.data1 = g_cinema_planes[1];
	g_cinema_dynamic_desc.data2 = Layout::CINEMA_PLANES >= 3 ? g_cinema_planes[2] : nullptr;
	g_cinema_desc = &g_cinema_dynamic_desc;
	Weather::markDirty();
	Serial.println(F("[cinema] download completato, immagine remappata"));
}

/**
 * Esegue, in un solo giro, tutti i fetch di rete che condividono la finestra
 * WiFi. Presuppone la radio gia' connessa: non la accende ne' la spegne.
 *
 * L'ordine e' il punto della funzione:
 *   ping /health -> meteo -> mail -> Google -> Outlook -> cinema
 * Il ping apre il giro per svegliare render.com, il cinema lo chiude perche'
 * e' l'unico fetch che puo' pagare un cold start, e i fetch in mezzo sono la
 * copertura di quel boot.
 *
 * Chiamata dai due rami di loop() - dentro la finestra OTA, dove la STA
 * risale da se' grazie ad AP_STA, e fuori, dopo wifiOn() - che prima ne
 * tenevano una copia a testa. L'ordine dei fetch vive quindi qui e in un
 * posto solo, invece di poter divergere fra i due rami.
 *
 * Ogni chiamata e' best-effort e indipendente dalle altre: i moduli
 * conservano la cache precedente quando un fetch fallisce, la UI disegna
 * "--" sugli slot senza dati e il cinema ricade sul wallpaper PROGMEM.
 * Nessun fallimento interrompe la sequenza.
 *
 * markDirty() dopo ogni fetch riuscito perche' il frame e' unico e
 * monolitico: qualunque modulo con dati nuovi deve chiedere il ridisegno.
 * Weather non compare perche' alza il proprio flag da se'.
 */
static void runNetworkFetches()
{
	/**
	 * Ping di sveglia al server cinema prima di tutto il resto: il tempo che
	 * meteo, mail e calendari passano in rete e' il tempo che render.com usa
	 * per fare boot, cosi' il fetch dell'immagine lo trova caldo o quasi. Si
	 * autolimita ai soli giri in cui l'immagine verra' davvero scaricata.
	 */
	prewarmCinemaServer();

	/**
	 * L'aggancio dei calendari alla finestra WiFi del meteo guarda il meteo
	 * davvero aggiornato adesso, non il meteo "da aggiornare": pendingFetch()
	 * resta FETCH_BOTH finche' mancano corrente e prima previsione, quindi con
	 * OpenWeather irraggiungibile un gate sul solo need sarebbe sempre vero e
	 * scavalcherebbe il backoff che Outlook e Google si sono appena imposti -
	 * nel ramo OTA, che gira ogni ~10 ms, un fetch per iterazione.
	 */
	const Weather::FetchKind need = Weather::pendingFetch();
	const bool meteoAggiornato = (need != Weather::FETCH_NONE) && Weather::runFetch(need);

	/**
	 * Mail PRIMA dei calendari. Best-effort: se Mail::runFetch() fallisce
	 * (WiFi cade, batch HTTP error, budget esaurito) o se l'inbox e' vuota,
	 * il flusso prosegue normalmente con i fetch calendario: un problema
	 * mail NON deve impattare meteo/calendari/cinema.
	 */
	if (Mail::pendingFetch())
		if (Mail::runFetch())
			Weather::markDirty();

	// Google prima di Outlook: il suo access token e' appena stato rinfrescato
	// da Mail, che condivide la stessa cache token.
	if (meteoAggiornato || Calendar::Google::pendingFetch())
		if (Calendar::Google::runFetch())
			Weather::markDirty();
	if (meteoAggiornato || Calendar::Outlook::pendingFetch())
		if (Calendar::Outlook::runFetch())
			Weather::markDirty();

	/**
	 * Cinema per ultimo: e' l'unica chiamata che puo' pagare il cold start di
	 * render.com, e i fetch qui sopra sono il tempo che il server ha avuto per
	 * completare il boot avviato dal ping.
	 */
	fetchCinemaImage();
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
 *   // #include "wallpaper/my_image.h"   // genera my_image_desc
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
 * Accende il WiFi in modalita' STA e attende la connessione fino a
 * WIFI_CONNECT_TIMEOUT_MS.
 * La radio resta accesa solo per la finestra di fetch: viene spenta
 * da wifiOff() subito dopo.
 * @return true se connesso entro il timeout.
 */
static bool wifiOn()
{
	WiFi.mode(WIFI_STA);
	WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
	uint32_t t0 = millis();
	while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_CONNECT_TIMEOUT_MS)
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
			runNetworkFetches();
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

	/**
	 * Interrogazione dei moduli al solo scopo di decidere se accendere la
	 * radio: i fetch veri li gatea runNetworkFetches(), che rilegge questi
	 * stessi predicati per conto suo. Mail e' nel gate perche' se le uniche
	 * scadenze sono le mail il WiFi va acceso comunque.
	 */
	Weather::FetchKind need = Weather::pendingFetch();
	bool needOutlook = Calendar::Outlook::pendingFetch();
	bool needGoogle = Calendar::Google::pendingFetch();
	bool needMail = Mail::pendingFetch();
	if (need != Weather::FETCH_NONE || needOutlook || needGoogle || needMail)
	{
		if (isActiveHour())
		{
			if (wifiOn())
			{
				runNetworkFetches();
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
	 * Pannello in deep sleep prima del light sleep dell'MCU: porta il
	 * controller da standby a 1-5 uA, ed è quasi gratis perchè il refresh
	 * pieno chiude con 0x22 = 0xF7, che ha già disabilitato analog e clock:
	 * il _PowerOff() interno a hibernate() è quindi un no-op e resta solo il
	 * comando di deep sleep. Idempotente: nei giri in cui Weather::render()
	 * non disegna niente il pannello è già addormentato e la chiamata non fa
	 * nulla.
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

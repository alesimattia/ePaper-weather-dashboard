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

// ---------------------------------------------------------------------------
// Cadenze, timeout e pavimenti stanno tutti in Timings.h, che va incluso PRIMA
// dei moduli perche' i loro #ifndef li raccolgano. Le cadenze sono anche
// modificabili a runtime dalla pagina di configurazione, con persistenza NVS:
// vedi Timings::begin(), chiamata per prima in setup().
// ---------------------------------------------------------------------------
#include "Timings.h"

/**
 * Endpoint del ping di sveglia del server cinema. Non e' un tempo, quindi non
 * sta in Timings.h; il suo timeout invece si' (CINEMA_PREWARM_TIMEOUT_MS).
 *
 * L'host deve restare uguale a quello di Layout::CINEMA_URL (Layout_097c.h,
 * Layout_122c.h). La', l'URL vive nei layout perche' la query string codifica
 * width/height/colors del pannello; /health non dipende dal pannello e sta
 * quindi qui. Cambiando dominio vanno aggiornati tutti e tre i punti.
 */
#define CINEMA_PREWARM_URL "https://cinema-epd.onrender.com/health"

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
#include "Env.h"
#include "Log.h"
#include "Scheduler.h"

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
//   1. Boot: g_cinema_desc punta a img_la_grande_onda_desc (fallback PROGMEM).
//   2. Al primo ciclo con WiFi connesso, come ultima chiamata di rete del
//      giro (ultimo task di rete della tabella), fetchCinemaImage() scarica i
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
 * dal server cinema o fallback PROGMEM img_la_grande_onda_desc) deve essere
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
static const GxEPDImage::Descriptor *g_cinema_desc = &img_la_grande_onda_desc;


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
			LOG("cinema", "%s: alloc %u byte in PSRAM", label, (unsigned)Layout::CINEMA_PLANE_SZ);
			return p;
		}
		LOG("cinema", "%s: PSRAM alloc fallita, provo heap interno", label);
	}
	p = (uint8_t *)malloc(Layout::CINEMA_PLANE_SZ);
	if (p)
		LOG("cinema", "%s: alloc %u byte in heap interno (free: %u)",
			label, (unsigned)Layout::CINEMA_PLANE_SZ, (unsigned)ESP.getFreeHeap());
	else
		LOG("cinema", "%s: allocazione fallita (%u byte richiesti, %u disponibili)",
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
	WiFiClientSecure client;
	client.setInsecure();

	HTTPClient http;
	http.setTimeout(CINEMA_PREWARM_TIMEOUT_MS);
	// Nessun keep-alive: end() deve chiudere il socket subito invece di
	// tenerlo aperto per un riuso che non arrivera' mai.
	http.setReuse(false);
	if (!http.begin(client, CINEMA_PREWARM_URL))
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
 * Scarica l'immagine cinema dal server render.com.
 *
 * Ultima chiamata di rete del giro, dopo meteo, mail e calendari: e' l'unica
 * che puo' pagare il cold start di render.com, e il tempo speso dagli altri
 * fetch e' il tempo che il server ha per completare il boot avviato da
 * prewarmCinemaServer(). Cadenza (giornaliera) e ritenti sono dello
 * scheduler; qui si scarica e basta, con la radio gia' connessa.
 *
 * Sequenza:
 *   1. Libera i buffer dell'immagine precedente (se presenti da un fetch
 *      riuscito in un giro precedente) e riporta g_cinema_desc al fallback
 *      PROGMEM: se il fetch fallisce o il refresh avviene durante un
 *      render, il display mostra il fallback invece di un'immagine corrotta.
 *   2. Alloca Layout::CINEMA_PLANES buffer da Layout::CINEMA_PLANE_SZ byte
 *      (PSRAM preferita, heap interno come fallback).
 *   3. HTTP GET -> verifica status 200 e Content-Length == Layout::CINEMA_TOTAL_SZ.
 *   4. Legge in stream i piani nell'ordine in cui il server li concatena via
 *      readBytes, direttamente nei buffer.
 *   5. Ripuntamento di g_cinema_desc al descrittore dinamico.
 *
 * In caso di qualunque errore (OOM, HTTP != 200, size mismatch, read short)
 * libera i buffer e mantiene il fallback PROGMEM.
 * @return true se l'immagine e' stata scaricata e rimappata.
 */
static bool fetchCinemaImage()
{
	// Libera l'immagine precedente (no-op al primo boot) e ripristina il
	// fallback PROGMEM come "immagine corrente" finchè il nuovo download
	// non completa con successo.
	freeCinemaBuffers();
	g_cinema_desc = &img_la_grande_onda_desc;

	LOG("cinema", "fetching %s", Layout::CINEMA_URL);
	LOG("cinema", "PSRAM %s, free heap: %u byte",
		psramFound() ? "presente" : "assente (uso heap interno)",
		(unsigned)ESP.getFreeHeap());

	for (uint8_t p = 0; p < Layout::CINEMA_PLANES; ++p)
	{
		g_cinema_planes[p] = allocPlaneBuffer(g_cinema_plane_names[p]);
		if (!g_cinema_planes[p])
		{
			LOG("cinema", "allocazione buffer fallita, fallback PROGMEM");
			freeCinemaBuffers();
			return false;
		}
	}

	HTTPClient http;
	// 45s: margine per il cold start del free tier render.com (una ventina di
	// secondi) quando il ping di prewarmCinemaServer() non e' bastato a
	// completare il boot dell'istanza prima di arrivare qui.
	http.setTimeout(CINEMA_HTTP_TIMEOUT_MS);
	if (!http.begin(Layout::CINEMA_URL))
	{
		LOG("cinema", "HTTP begin fallita");
		freeCinemaBuffers();
		return false;
	}
	int code = http.GET();
	if (code != 200)
	{
		LOG("cinema", "HTTP status %d, fallback PROGMEM", code);
		http.end();
		freeCinemaBuffers();
		return false;
	}
	int size = http.getSize();
	if (size != (int)Layout::CINEMA_TOTAL_SZ)
	{
		LOG("cinema", "Content-Length %d atteso %u, fallback PROGMEM",
			size, (unsigned)Layout::CINEMA_TOTAL_SZ);
		http.end();
		freeCinemaBuffers();
		return false;
	}

	WiFiClient *stream = http.getStreamPtr();
	// readBytes() blocca fino a riempimento del buffer richiesto, timeout
	// (controllato da setTimeout) o EOF. Niente polling manuale di available()
	// con delay(1): readBytes lo fa gia' internamente in modo equivalente.
	stream->setTimeout(CINEMA_HTTP_TIMEOUT_MS);
	for (uint8_t p = 0; p < Layout::CINEMA_PLANES; ++p)
	{
		size_t read = 0;
		uint32_t t0 = millis();
		// Wall-clock guard 45s per piano: se readBytes ritorna in modo
		// frazionario (n>0 ma < richiesto) e la rete è lenta, evita di
		// accumulare timeout >45s totali sul singolo piano.
		while (read < Layout::CINEMA_PLANE_SZ && (millis() - t0) < CINEMA_HTTP_TIMEOUT_MS)
		{
			int n = stream->readBytes(g_cinema_planes[p] + read, Layout::CINEMA_PLANE_SZ - read);
			if (n <= 0)
				break; // timeout interno o connessione chiusa
			read += n;
		}
		if (read != Layout::CINEMA_PLANE_SZ)
		{
			LOG("cinema", "piano %s letto parzialmente (%u/%u)",
				g_cinema_plane_names[p], (unsigned)read, (unsigned)Layout::CINEMA_PLANE_SZ);
			http.end();
			freeCinemaBuffers();
			return false;
		}
	}
	http.end();

	g_cinema_dynamic_desc.data0 = g_cinema_planes[0];
	g_cinema_dynamic_desc.data1 = g_cinema_planes[1];
	g_cinema_dynamic_desc.data2 = Layout::CINEMA_PLANES >= 3 ? g_cinema_planes[2] : nullptr;
	g_cinema_desc = &g_cinema_dynamic_desc;
	LOG("cinema", "download completato, immagine remappata");
	return true;
}

/**
 * Adattatori fra l'API dei moduli e il vocabolario dello scheduler.
 * Restano nel .ino perche' la tabella nomina anche il cinema, che e' fatto di
 * statici di questo file.
 */
static Scheduler::Esito eseguiMeteo()
{
	return Weather::runFetch() ? Scheduler::Esito::OK : Scheduler::Esito::FALLITO;
}

/** Corrente senza previsioni non basta a disegnare il banner: lo scheduler
 *  ritenta invece di attendere la cadenza piena. */
static bool meteoIncompleto() { return !Weather::datiCompleti(); }

static Scheduler::Esito eseguiMail()
{
	return Mail::runFetch() ? Scheduler::Esito::OK : Scheduler::Esito::FALLITO;
}

static Scheduler::Esito eseguiGoogle()
{
	return Calendar::Google::runFetch() ? Scheduler::Esito::OK : Scheduler::Esito::FALLITO;
}

static Scheduler::Esito eseguiOutlook()
{
	return Calendar::Outlook::runFetch() ? Scheduler::Esito::OK : Scheduler::Esito::FALLITO;
}

static Scheduler::Esito eseguiCinema()
{
	return fetchCinemaImage() ? Scheduler::Esito::OK : Scheduler::Esito::FALLITO;
}

/** Poll del sensore: BSEC temporizza da se' i 5 minuti, quindi la chiamata e'
 *  a costo trascurabile quando nessun campione e' dovuto. */
static Scheduler::Esito eseguiIndoor()
{
	return Indoor::refresh() ? Scheduler::Esito::OK : Scheduler::Esito::SALTATO;
}

/** Scadenza del prossimo campione ULP, l'unica informazione disponibile:
 *  next_call della libreria non e' leggibile dall'esterno. */
static uint32_t scadenzaIndoor()
{
	const Indoor::Sample& s = Indoor::sample();
	if (!s.valid) return (uint32_t)SLEEP_MAX_S * 1000UL;
	const uint32_t quando = s.lastUpdateMs + (uint32_t)BSEC_PERIODO_ULP_S * 1000UL + BSEC_MARGINE_POLL_MS;
	const int32_t d = (int32_t)(quando - millis());
	return d <= 0 ? 0 : (uint32_t)d;
}

/** Chiede il ridisegno quando il task ha portato dati nuovi. */
static void marcaSuOk(Scheduler::Esito e)
{
	if (e == Scheduler::Esito::OK) Weather::markDirty();
}

/** Il pannello si ridisegna solo con dati nuovi da mostrare, e non prima che
 *  la rete abbia dato un esito o sia scaduta l'attesa del primo frame. */
static bool displayPronto()
{
	return Weather::sporco() && Scheduler::primoFrameConsentito();
}

/** render() azzera da se' il flag dei dati nuovi. */
static Scheduler::Esito eseguiDisplay()
{
	Weather::render();
	return Scheduler::Esito::OK;
}

/** Il display non si auto-sporca: senza questo hook il ridisegno si
 *  richiederebbe da solo a ogni giro. */
static void dopoDisplay(Scheduler::Esito) {}

/**
 * Tutti i flussi temporizzati del dispositivo. L'ordine delle righe e'
 * l'ordine di esecuzione dentro la rispettiva fase, e non e' arbitrario:
 *
 *   - mail prima di Google perche' i due condividono la cache del token
 *     OAuth: chi gira per primo paga il refresh;
 *   - Google prima di Outlook perche' il token appena rinfrescato e' il suo;
 *   - cinema per ultimo perche' e' l'unico che puo' pagare il cold start di
 *     render.com, e il tempo di rete degli altri e' la copertura di quel
 *     boot; il suo prima() manda il ping di sveglia all'inizio del giro;
 *   - il sensore precede il display cosi' un campione appena prodotto entra
 *     nel frame dello stesso giro.
 */
static Scheduler::Task TABELLA_TASK[] = {
	{"meteo", Scheduler::Cadenza::PERIODICA, true, false,
	 &Timings::Valori::owmMin, nullptr,
	 FETCH_RITENTO_MS, FETCH_MAX_TENTATIVI,
	 eseguiMeteo, nullptr, meteoIncompleto, nullptr, marcaSuOk, nullptr},

	{"mail", Scheduler::Cadenza::PERIODICA, true, false,
	 &Timings::Valori::mailMin, nullptr,
	 FETCH_RITENTO_MS, FETCH_MAX_TENTATIVI,
	 eseguiMail, nullptr, nullptr, nullptr, marcaSuOk, nullptr},

	{"google", Scheduler::Cadenza::PERIODICA, true, false,
	 &Timings::Valori::googleMin, nullptr,
	 FETCH_RITENTO_MS, FETCH_MAX_TENTATIVI,
	 eseguiGoogle, nullptr, nullptr, nullptr, marcaSuOk, nullptr},

	{"outlook", Scheduler::Cadenza::PERIODICA, true, false,
	 &Timings::Valori::outlookMin, nullptr,
	 FETCH_RITENTO_MS, FETCH_MAX_TENTATIVI,
	 eseguiOutlook, nullptr, nullptr, nullptr, marcaSuOk, nullptr},

	{"cinema", Scheduler::Cadenza::GIORNALIERA, true, false,
	 nullptr, &Timings::Valori::cinemaOra,
	 FETCH_RITENTO_MS, FETCH_MAX_TENTATIVI,
	 eseguiCinema, nullptr, nullptr, prewarmCinemaServer, marcaSuOk, nullptr},

	{"bsec", Scheduler::Cadenza::PERIODICA, false, false,
	 &Timings::Valori::displayRefreshMin, nullptr,
	 FETCH_RITENTO_MS, 0,
	 eseguiIndoor, nullptr, nullptr, nullptr, marcaSuOk, scadenzaIndoor},

	{"display", Scheduler::Cadenza::PERIODICA, false, false,
	 &Timings::Valori::displayRefreshMin, nullptr,
	 FETCH_RITENTO_MS, 0,
	 eseguiDisplay, displayPronto, nullptr, nullptr, dopoDisplay, nullptr},
};

static constexpr uint8_t N_TASK = sizeof(TABELLA_TASK) / sizeof(TABELLA_TASK[0]);

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
 * Ritorna true se l'orologio di sistema è stato sincronizzato, cioè se time()
 * supera TIME_VALID_EPOCH_MIN. Prima della sincronizzazione time() restituisce
 * l'uptime contato dal 1970, che dopo 27,7 h di accensione diventa
 * indistinguibile da un'ora reale.
 */
static bool timeIsValid()
{
	return time(nullptr) >= TIME_VALID_EPOCH_MIN;
}

/**
 * Sincronizza l'orologio via SNTP se non lo è già. Presuppone la STA
 * connessa: va chiamata da wifiOn() o dentro la finestra OTA, dove la radio è
 * su. Quando l'ora è già valida costa un solo confronto.
 *
 * Usa configTzTime() e non configTime(): la prima applica la stringa POSIX che
 * le viene passata, quindi ripassando CAL_POSIX_TZ riapplica lo stesso fuso di
 * Calendar::initTimezone(); la seconda deriverebbe il TZ dagli offset e lo
 * sovrascriverebbe, perdendo il DST automatico di Europe/Rome.
 *
 * Una sincronizzazione riuscita per boot basta: il light sleep mantiene la base
 * temporale su cui poggiano time() e millis(), quindi l'ora sopravvive al
 * sonno. Il drift dell'RTC, che su questo modulo gira sull'oscillatore RC
 * interno perchè non c'è il cristallo da 32 kHz, viene corretto dal polling
 * SNTP di lwIP: riprova ogni 3 h e va a buon fine appena cade in una finestra
 * con radio accesa, per questo sntp non viene mai fermato.
 *
 * @param timeout_ms attesa massima della prima sincronizzazione. A 0 avvia
 *        SNTP e ritorna subito, lasciando che il polling di lwIP allinei
 *        l'ora entro pochi secondi: serve al ramo OTA, dove bloccare
 *        congelerebbe AP e web server.
 * @return true se l'ora è valida al ritorno.
 */
static bool ensureTimeSynced(uint32_t timeout_ms = TIME_SYNC_TIMEOUT_MS)
{
	static uint32_t last_attempt_ms = 0;
	static bool attempted = false;

	if (timeIsValid())
		return true;
	// Backoff fra tentativi falliti: senza, il ramo OTA riavvierebbe SNTP a
	// ogni giro da 10 ms.
	if (attempted && (int32_t)(millis() - last_attempt_ms) < (int32_t)TIME_SYNC_RETRY_MS)
		return false;

	last_attempt_ms = millis();
	attempted = true;

	configTzTime(CAL_POSIX_TZ, NTP_SERVER_1, NTP_SERVER_2);

	uint32_t t0 = millis();
	while (!timeIsValid() && (millis() - t0) < timeout_ms)
	{
		delay(WIFI_ATTESA_POLL_MS);
	}

	if (!timeIsValid())
	{
		// Con timeout_ms a 0 non è un errore: la richiesta è partita e il
		// polling SNTP di lwIP la porta a termine senza bloccare il chiamante.
		if (timeout_ms == 0)
			LOG("Time", "SNTP avviato, sincronizzazione in corso");
		else
			LOG("Time", "SNTP timeout");
		return false;
	}

	time_t now = time(nullptr);
	struct tm t;
	localtime_r(&now, &t);
	LOG("Time", "SNTP ok: %04d-%02d-%02d %02d:%02d:%02d locale",
		t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
		t.tm_hour, t.tm_min, t.tm_sec);
	return true;
}

/**
 * Accende il WiFi in modalita' STA e attende la connessione fino a
 * WIFI_CONNECT_TIMEOUT_MS.
 * La radio resta accesa solo per la finestra di fetch: viene spenta
 * da wifiOff() subito dopo.
 *
 * A connessione riuscita sincronizza anche l'orologio con ensureTimeSynced(),
 * che al primo boot puo' aggiungere fino a TIME_SYNC_TIMEOUT_MS di attesa.
 * @return true se connesso entro il timeout.
 */
static bool wifiOn()
{
	WiFi.mode(WIFI_STA);
	WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
	uint32_t t0 = millis();
	while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_CONNECT_TIMEOUT_MS)
	{
		delay(WIFI_ATTESA_POLL_MS);
	}
	if (WiFi.status() == WL_CONNECTED)
	{
		LOG("WiFi", "connected, IP=%s", WiFi.localIP().toString().c_str());
		/**
		 * Orologio sincronizzato nello stesso punto in cui la radio diventa
		 * disponibile: tutti i fetch del ramo normale stanno dentro questo
		 * if (wifiOn()), quindi da qui in avanti vedono l'ora vera.
		 */
		ensureTimeSynced();
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
	 * Maintenance::chiudi() che spegne AP e WiFi, ripristinando
	 * il ciclo originario STA on-demand + light sleep DISPLAY_REFRESH_MIN.
	 */
	Maintenance::begin();
	Scheduler::begin(TABELLA_TASK, N_TASK, wifiOn, wifiOff);
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
 * comportamento energetico originale: STA acceso solo poco prima del fetch
 * e spento subito dopo, con light sleep DISPLAY_REFRESH_MIN minuti fra un
 * giro e l'altro (tempo minimo fra un refresh del display e il successivo).
 *
 * @modified 21/04/26 light sleep 60 s -> 5 min + Indoor::refresh() agganciato
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
		 */
		Scheduler::giro(Scheduler::Radio::ESTERNA);
		delay(MAINTENANCE_LOOP_DELAY_MS);
		return;
	}

	// Finestra chiusa: consegna la radio spenta allo scheduler (idempotente).
	Maintenance::chiudi();

	Scheduler::giro(Scheduler::Radio::PROPRIA);

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

	Scheduler::dormi();
}

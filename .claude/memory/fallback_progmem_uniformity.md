---
name: Fallback PROGMEM e RAM/PSRAM via descriptor unico
description: Perchè GxEPDImage::showImage funziona uguale per dati Flash e dati RAM su ESP32 + il vincolo di dimensione e formato del wallpaper di fallback
type: project
---

`g_cinema_desc` (`ePaper-weather-dashboard.ino`) viene riassegnato senza problemi tra:
- `&img_la_grande_onda_desc` → buffer in `PROGMEM` (Flash readonly).
- `&g_cinema_dynamic_desc` → buffer in `RAM` (heap interno) o `PSRAM` (SPI external RAM).

`drawTestBackground()` chiama `GxEPDImage::showImage(display, *g_cinema_desc)` senza distinguere il caso.

**Why (deduzione architetturale, non documentata esplicitamente nel driver):**
- Su AVR (Arduino UNO, Mega) `PROGMEM` vive in spazio Harvard separato dalla RAM: per leggere serve `pgm_read_byte()` con un opcode dedicato.
- Su ESP32 (memoria piatta von Neumann, mappa lineare) `pgm_read_byte(p)` è definito come `(*(const uint8_t*)(p))`, cioè una normale dereferenza, indipendentemente da dove punta `p` (Flash, IRAM, DRAM, PSRAM).
- Quindi `GxEPDImage::showImage` può usare `pgm_read_byte()` in modo uniforme: legge correttamente sia da `.h` PROGMEM hardcoded sia da `malloc()`/`heap_caps_malloc(MALLOC_CAP_SPIRAM)`.

**Conseguenze pratiche:**
- Aggiungere immagini PROGMEM hardcoded (es. slideshow) richiede solo un nuovo `Descriptor` con i puntatori giusti; nessuna modifica al driver.
- Aggiungere immagini scaricate richiede solo allocazione + popolamento del descriptor; idem.
- Se in futuro il firmware girasse su un AVR (improbabile ma possibile come retrofit didattico), questo trucco si rompe: serve duplicare `showImage` in versione "PROGMEM-only" e "RAM-only".

**Effetto sulla progettazione del fetch:**
- `freeCinemaBuffers()` libera la RAM e ripuntare `g_cinema_desc` al descrittore del wallpaper riporta a un descrittore in Flash. Lo swap è atomico per il rendering perchè il puntatore è una variabile statica (anche se non `volatile`, il loop del display è single-thread sul core principale: nessun lock necessario).
- L'immagine fallback PROGMEM costa solo Flash, già inclusa nel firmware: nessun runtime overhead. Quanto costa lo dice il formato, non una misura: un piano a 1 bpp da 620 px di larghezza ha stride 78 byte, quindi 78 x altezza.

**Guardia del descrittore.** Il wallpaper in `wallpaper/` definisce il proprio `_desc` solo
dentro `#ifdef _GxEPDImage_H_`, cioè la include guard dell'header che definisce il namespace
(`GxEPD2_SOLUM_ESL/src/GxEPDImage.h`), non quella di un driver: gli array raw restano sempre
disponibili, il descrittore solo se il tipo esiste. La guardia la emette `epd_image_converter.pyw`,
quindi va corretta lì oltre che nel file generato. Nominare un driver specifico romperebbe la build
su tutte le altre varianti di display.

**Il fallback deve combaciare con l'area del wallpaper, e oggi combacia.** Il descrittore in
`wallpaper/` e' generato a `620x300`, cioe' esattamente `Layout::CINEMA_W x CINEMA_H` del 097c, ed
e' un vincolo da rispettare a ogni rigenerazione: `showImage` **non clippa**, disegna da `(0,0)`
usando width e height del `Descriptor`. Un fallback piu' alto invaderebbe la fascia mail
(`MAIL_Y = CINEMA_H`), che `Mail::draw()` non ripulisce perche' conta sul `fillScreen(GxEPD_WHITE)`
del loop paged, e le righe delle mail finirebbero disegnate sopra l'immagine; uno piu' largo
entrerebbe nella sidebar del calendario.

**Il fallback e' monocromatico** (`FORMAT_BW_1BPP`, un solo piano, `data1` e `data2` a `nullptr`),
mentre il descrittore dinamico scaricato e' a due piani su questi pannelli. Il formato viaggia
dentro il `Descriptor`, quindi i due convivono senza rami nel chiamante: `drawTestBackground()`
passa il puntatore e basta.

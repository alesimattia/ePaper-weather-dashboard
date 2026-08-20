---
name: Fallback PROGMEM e RAM/PSRAM via descriptor unico
description: Perchè GxEPDImage::showImage funziona uguale per dati Flash e dati RAM su ESP32 + disallineamento di dimensione del fallback
type: project
---

`g_cinema_desc` (`ePaper-weather-dashboard.ino`) viene riassegnato senza problemi tra:
- `&img_apple_bwry_desc` → buffer in `PROGMEM` (Flash readonly).
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
- `freeCinemaBuffers()` libera la RAM e ripuntare `g_cinema_desc = &img_apple_bwry_desc` riporta a un descrittore in Flash. Lo swap è atomico per il rendering perchè il puntatore è una variabile statica (anche se non `volatile`, il loop del display è single-thread sul core principale: nessun lock necessario).
- L'immagine fallback PROGMEM costa solo Flash (~100 KB) già inclusa nel firmware: nessun runtime overhead.

**Guardia del descrittore.** `wallpaper/img_apple_bwry.h` definisce `img_apple_bwry_desc` solo
dentro `#ifdef _GxEPDImage_H_`, cioè la include guard dell'header che definisce il namespace
(`GxEPD2_SOLUM_ESL/src/GxEPDImage.h`), non quella di un driver: gli array raw restano sempre
disponibili, il descrittore solo se il tipo esiste. La guardia la emette `epd_image_converter.pyw`,
quindi va corretta lì oltre che nel file generato. Nominare un driver specifico romperebbe la build
su tutte le altre varianti di display.

**Disallineamento di dimensione del fallback.** `img_apple_bwry_desc` è **620x460** mentre
`Layout::CINEMA_W x CINEMA_H` è **620x300**. `showImage` non clippa (disegna da `(0,0)` usando
width/height del `Descriptor`), quindi le righe 300..459 del fallback cadono nella fascia mail
(`MAIL_Y = CINEMA_H = 300`, `MAIL_H = BANNER_Y - CINEMA_H = 160`). `Mail::draw()` non ripulisce la
propria banda — conta sul `fillScreen(GxEPD_WHITE)` del loop paged — perciò le righe mail vengono
disegnate sopra l'immagine. Il descrittore dinamico scaricato è invece coerente (620x300, da
`Layout::CINEMA_*`). Per allineare: rigenerare `wallpaper/img_apple_bwry.h` a 620x300 con
`epd_image_converter.pyw`.

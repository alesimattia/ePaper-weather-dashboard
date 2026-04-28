---
name: Fallback PROGMEM e RAM/PSRAM via descriptor unico
description: Perche' GxEPDImage::showImage funziona uguale per dati Flash e dati RAM su ESP32
type: project
---

`g_cinema_desc` (`ePaper-weather-dashboard.ino`) viene riassegnato senza problemi tra:
- `&img_apple_bwry_desc` → buffer in `PROGMEM` (Flash readonly).
- `&g_cinema_dynamic_desc` → buffer in `RAM` (heap interno) o `PSRAM` (SPI external RAM).

`drawTestBackground()` chiama `GxEPDImage::showImage(display, *g_cinema_desc)` senza distinguere il caso.

**Why (deduzione architetturale, non documentata esplicitamente nel driver):**
- Su AVR (Arduino UNO, Mega) `PROGMEM` vive in spazio Harvard separato dalla RAM: per leggere serve `pgm_read_byte()` con un opcode dedicato.
- Su ESP32 (memoria piatta von Neumann, mappa lineare) `pgm_read_byte(p)` e' definito come `(*(const uint8_t*)(p))`, cioe' una normale dereferenza, indipendentemente da dove punta `p` (Flash, IRAM, DRAM, PSRAM).
- Quindi `GxEPDImage::showImage` puo' usare `pgm_read_byte()` in modo uniforme: legge correttamente sia da `.h` PROGMEM hardcoded sia da `malloc()`/`heap_caps_malloc(MALLOC_CAP_SPIRAM)`.

**Conseguenze pratiche:**
- Aggiungere immagini PROGMEM hardcoded (es. slideshow) richiede solo un nuovo `Descriptor` con i puntatori giusti; nessuna modifica al driver.
- Aggiungere immagini scaricate richiede solo allocazione + popolamento del descriptor; idem.
- Se in futuro il firmware girasse su un AVR (improbabile ma possibile come retrofit didattico), questo trucco si rompe: serve duplicare `showImage` in versione "PROGMEM-only" e "RAM-only".

**Effetto sulla progettazione del fetch:**
- `freeCinemaBuffers()` libera la RAM e ripuntare `g_cinema_desc = &img_apple_bwry_desc` riporta a un descrittore in Flash. Lo swap e' atomico per il rendering perche' il puntatore e' una variabile statica (anche se non `volatile`, il loop del display e' single-thread sul core principale: nessun lock necessario).
- L'immagine fallback PROGMEM costa solo Flash (~100 KB) gia' inclusa nel firmware: nessun runtime overhead.

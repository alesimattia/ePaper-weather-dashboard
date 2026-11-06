---
name: Fallback PROGMEM e immagine in flash via descriptor unico
description: Perchè GxEPDImage::showImage legge uguale la PROGMEM e la partizione mappata su ESP32, come drawTestBackground taglia le righe alla pagina e a CINEMA_H, e il vincolo di dimensione e formato del wallpaper di fallback
type: project
---

`drawTestBackground()` (`ePaper-weather-dashboard.ino`) sceglie fra due descrittori senza altri rami:
- `CinemaStore::image()` → immagine scaricata, nella partizione `wallpaper` mappata con `esp_partition_mmap` (vedi [[esp32_cinema_consumer]]);
- `&img_la_grande_onda_desc` → fallback in `PROGMEM` (Flash readonly), quando la prima è `nullptr`.

**Why (deduzione architetturale, non documentata esplicitamente nel driver):**
- Su AVR (Arduino UNO, Mega) `PROGMEM` vive in spazio Harvard separato dalla RAM: per leggere serve `pgm_read_byte()` con un opcode dedicato.
- Su ESP32 (memoria piatta von Neumann, mappa lineare) `pgm_read_byte(p)` è definito come `(*(const uint8_t*)(p))`, cioè una normale dereferenza, indipendentemente da dove punta `p` (Flash mappata, IRAM, DRAM, PSRAM).
- Quindi `GxEPDImage::showImage` può usare `pgm_read_byte()` in modo uniforme: legge correttamente sia da `.h` PROGMEM sia dalla partizione dati mappata, che passa dalla stessa cache della flash.

**Conseguenze pratiche:**
- Aggiungere immagini PROGMEM hardcoded (es. slideshow) richiede solo un nuovo `Descriptor` con i puntatori giusti; nessuna modifica al driver.
- Se in futuro il firmware girasse su un AVR (improbabile ma possibile come retrofit didattico), questo trucco si rompe: serve duplicare `showImage` in versione "PROGMEM-only" e "RAM-only".
- L'immagine fallback PROGMEM costa solo Flash, già inclusa nel firmware: nessun runtime overhead. Quanto costa lo dice il formato, non una misura: un piano a 1 bpp ha stride `ceil(larghezza/8)` byte, 77 a 610 px, quindi 77 x altezza.

**Righe per pagina.** `drawTestBackground()` restringe le righe [`CINEMA_Y`, `CINEMA_Y` + min(altezza, `CINEMA_H`)) alla pagina corrente con `Graphics::clipToPage()` e passa a `showImage` un `Descriptor` ridotto: stessi dati spostati di riga × stride (`data1`/`data2` solo se presenti), altezza ridotta, y della prima riga utile. Ogni riga si decodifica una volta per frame e nessun dato si copia; il salto interno di `showImage`, basato sul page hint del driver, con questo descrittore non scarta più niente.

**Guardia del descrittore.** Il wallpaper in `wallpaper/` definisce il proprio `_desc` solo
dentro `#ifdef _GxEPDImage_H_`, cioè la include guard dell'header che definisce il namespace
(`GxEPD2_SOLUM_ESL/src/GxEPDImage.h`), non quella di un driver: gli array raw restano sempre
disponibili, il descrittore solo se il tipo esiste. La guardia la emette `GxEPD2_SOLUM_ESL/tools/epd_image_converter.pyw`,
quindi va corretta lì oltre che nel file generato. Nominare un driver specifico romperebbe la build
su tutte le altre varianti di display.

**Il fallback deve combaciare con l'area del wallpaper.** Il descrittore in `wallpaper/` va
generato dall'utente con `epd_image_converter.pyw` a `Layout::CINEMA_W x CINEMA_H` del 097c,
cioè `610x290`, la stessa larghezza del riquadro Posta (`CINEMA_W` = `MAIL_W`), così wallpaper e
Posta restano allineati. `showImage` **non clippa** e ricava lo stride dalla larghezza del
descrittore: `drawTestBackground()` taglia le righe oltre `CINEMA_H`, ma le colonne oltre
`CINEMA_W` sconfinano nel gutter, dove nessuno le sbianca. La larghezza giusta si ottiene
fornendo l'immagine a misura, non con codice (scelta dell'utente). Il fallback è unico per i due
pannelli: sul 122c (area 610x325) resta più basso e lascia una fascia bianca. Nessuno
`static_assert` sulla misura, per scelta dell'utente: è preferibile a un errore di compilazione.

**Il fallback è monocromatico** (`FORMAT_BW_1BPP`, un solo piano, `data1` e `data2` a `nullptr`),
mentre l'immagine scaricata è a due piani su questi pannelli. Il formato viaggia dentro il
`Descriptor`, quindi i due convivono senza rami nel chiamante.

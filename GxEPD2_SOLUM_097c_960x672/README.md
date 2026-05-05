# GxEPD2_SOLUM_097c_960x672 — Driver custom

Driver header-only per pannello e-paper **SOLUM ESL 9.7"** (672×960 px,
4 colori nativi: bianco/nero/rosso/giallo, controller SSD1677) su
**ESP32**. Estende [GxEPD2](https://github.com/ZinggJM/GxEPD2) di
Jean-Marc Zingg, fornendo:

- **API `showImage()` unificata** come unico entry-point one-shot di
  stampa immagine, con hibernate automatico opzionale. Due overload:
  descrittore generico (output dello script Python) e bitmap raw 1bpp
  B/N (formato [image2cpp](https://javl.github.io/image2cpp/));
- **3 API siblings uniformi** `writeImageBlack` / `writeImageRed` /
  `writeImageYellow` per scrittura single-channel, usate nel flusso
  paged con yellow iniettato "out-of-band" (vedi [sezione dedicata](#3-perché-il-yellow-è-out-of-band-nel-flusso-paged));
- **sistema di descrittori universali** (`GxEPDImage::Descriptor`) che
  porta con sé formato e dimensioni dell'immagine (BW / BWR / BWRY);
- **supporto nativo al 4° colore** (giallo) sul comando `0x28` del
  controller SSD1677 — verificato su hardware.

Per il contesto applicativo (sketch principale, moduli Weather/Calendar,
flussi di boot, OTA, ecc.) vedi [README principale del progetto](../README.md).

---

## Indice

- [Origine](#origine)
- [1. `GxEPDImage::showImage()` — unico entry-point pubblico](#1-gxepdimageshowimage--unico-entry-point-pubblico)
- [2. Tre API siblings single-channel uniformi](#2-tre-api-siblings-single-channel-uniformi)
- [3. Perché il yellow è "out-of-band" nel flusso paged](#3-perché-il-yellow-è-out-of-band-nel-flusso-paged)
- [4. Sistema di descrittori universali (`namespace GxEPDImage`)](#4-sistema-di-descrittori-universali-namespace-gxepdimage)
- [5. Ottimizzazioni rispetto al driver stock](#5-ottimizzazioni-rispetto-al-driver-stock)
- [6. API completa](#6-api-completa)

---

## Origine

Il driver è **header-only** (`inline` nell'`.h`, nessuna `.cpp`) e nasce
come fork del driver upstream
[`GxEPD2_1330c_GDEM133Z91`](https://github.com/ZinggJM/GxEPD2/blob/master/src/epd3c/GxEPD2_1330c_GDEM133Z91.cpp)
(pannello Good Display 13.3" 3-colori). Eredita da `GxEPD2_EPD` (classe
base della libreria) e implementa la sequenza di comandi specifica del
pannello SOLUM 9.7" su controller SSD1677:

- soft-reset (`0x12`)
- soft-start (`0x0C`)
- MUX per 680 gate lines (`0x01 = 0xA7 0x02 0x00`)
- bordo bianco (`0x3C = 0x01`)
- entry-mode x/y increase (`0x11 = 0x03`)
- full-window refresh (`0x22 = 0xF7` + `0x20`)
- power-off (`0x22 = 0xC3`)

Rispetto ai driver stock di GxEPD2 per pannelli simili, questa versione
introduce:

## 1. `GxEPDImage::showImage()` — unico entry-point pubblico

Free function template nel namespace `GxEPDImage` (vive nel driver `.h`):

```cpp
template<typename DisplayT>
void GxEPDImage::showImage(DisplayT& display,
                           const GxEPDImage::Descriptor& d,
                           int16_t x = 0, int16_t y = 0);
```

È **l'unica funzione pubblica per stampare un'immagine** sul pannello.
Va chiamata **dentro** un loop `firstPage()`/`nextPage()` del template
`GxEPD2_3C`, dopo `fillScreen()` e prima di `nextPage()`. Supporta tutti
e 3 i formati del descrittore (BW / BWR / BWRY) — gestisce internamente
il yellow out-of-band per il caso BWRY.

Pattern minimale one-shot:

```cpp
display.firstPage();
do {
  display.fillScreen(GxEPD_WHITE);
  GxEPDImage::showImage(display, *my_desc_ptr);
} while (display.nextPage());
display.hibernate();
```

Per immagini raw image2cpp B/N basta wrappare con la macro `GXEPD_BW_IMAGE`:

```cpp
GxEPDImage::showImage(display, GXEPD_BW_IMAGE(my_array, 960, 672));
```

Il chiamante è responsabile di: aprire il loop paged e chiamare
`hibernate()` se vuole spegnere il pannello. Il reset di
`preserveYellow(false)` per il caso BWRY è **gestito automaticamente**
dentro `refresh()` del driver al termine del loop paged.

**Idempotency canale yellow.** Per i descrittori BWRY, `showImage`
scrive il canale 0x28 al MASSIMO una volta per loop paged. Le iterazioni
2..8 di `nextPage()` trovano `isYellowPreserved()==true` e saltano la
chiamata `writeImageYellow` (risparmio ~110 ms per refresh @ 10 MHz SPI).

In più, se il chiamante ha già scritto yellow su 0x28 e attivato
`preserveYellow(true)` PRIMA di `firstPage()` (uso avanzato per
compositing yellow custom), `showImage` rispetta quello stato e non
sovrascrive.

**Multi-call per page.** `showImage` può essere chiamata 0, 1 o N volte
all'interno di una stessa iterazione del paged loop senza problemi: il
page-tracking interno avanza il counter solo quando il template chiude
la page (`writeImage(black, color)` chiamato da `nextPage()`), non a
ogni chiamata `showImage`. Utile per **compositing multi-immagine**:
sovrapporre più descrittori con offset `(x, y)` diversi, ognuno
disegnato correttamente nella sola porzione che intersecta la page
corrente. Vedi §5 "Loop pixel di `showImage` row-skip" per i dettagli
del meccanismo.

### Casi d'uso e firme

| # | Caso d'uso | Firma array (in `.h` incluso) | Firma chiamata |
|---|---|---|---|
| 1 | Bitmap **B/N raw** (image2cpp) | `const unsigned char img_xxx[] PROGMEM = { … };` | `GxEPDImage::showImage(display, GXEPD_BW_IMAGE(img_xxx, w, h));` |
| 2 | **BWR** da `epd_image_converter.pyw` | `const GxEPDImage::Descriptor img_xxx_desc;` *(auto-generato)* | `GxEPDImage::showImage(display, img_xxx_desc);` |
| 3 | **BWRY** da `epd_image_converter.pyw` | `const GxEPDImage::Descriptor img_xxx_desc;` *(auto-generato)* | `GxEPDImage::showImage(display, img_xxx_desc);` |
| 4 | **BWR raw inline** (2 piani separati) | `const unsigned char img_b[], img_r[] PROGMEM = { … };` | `GxEPDImage::showImage(display, GXEPD_BWR_IMAGE(img_b, img_r, w, h));` |
| 5 | **BWRY raw inline** (3 piani separati) | `const unsigned char img_b[], img_r[], img_y[] PROGMEM = { … };` | `GxEPDImage::showImage(display, GXEPD_BWRY_IMAGE(img_b, img_r, img_y, w, h));` |
| 6 | **BWRY con yellow pre-iniettato** (compositing yellow custom) | `const unsigned char img_b[], img_r[], img_y[] PROGMEM = { … };` | `display.epd2.writeImageYellow(custom_y, x, y, w, h, pgm);` + `display.epd2.preserveYellow(true);` + `GxEPDImage::showImage(display, GXEPD_BWR_IMAGE(img_b, img_r, w, h));` |
| 7 | **Single-channel diretto** (no GFX) | `const unsigned char img_b[], img_r[], img_y[] PROGMEM = { … };` | `display.epd2.writeImageBlack(img_b, x, y, w, h, true);` + `…Red(…)` + `…Yellow(…)` + `display.epd2.refresh(false);` |

Casi **1–6** vanno chiamati dentro un loop `firstPage()` / `nextPage()`
con `fillScreen()` prima, e seguiti da `display.hibernate()` se si vuole
spegnere il pannello. Il reset del flag `preserveYellow(false)` per il
caso BWRY è gestito automaticamente dentro `refresh()` del driver, non
serve farlo a mano.

- Per i casi **3** e **5** (BWRY senza pre-write), `showImage` scrive il
  canale 0x28 una sola volta per refresh grazie all'idempotency-check
  (le iterazioni 2..8 del paged loop saltano la riscrittura).
- Per il caso **6**, il pre-write del chiamante su 0x28 ha la precedenza:
  `showImage` non sovrascrive il yellow custom. Va usato il descrittore
  BWR (non BWRY) per non passare a `showImage` un `data2` che verrebbe
  comunque ignorato.

Il caso **7** bypassa il template GFX e si chiama standalone (incluso
`refresh()` esplicito).

## 2. Tre API siblings single-channel uniformi

I 3 canali del controller SSD1677 sono esposti con shape identica per
scritture single-channel (no refresh):

```cpp
void writeImageBlack (const uint8_t* bitmap, int16_t x, int16_t y,
                      int16_t w, int16_t h, bool pgm = true);  // cmd 0x24
void writeImageRed   (const uint8_t* bitmap, int16_t x, int16_t y,
                      int16_t w, int16_t h, bool pgm = true);  // cmd 0x26
void writeImageYellow(const uint8_t* bitmap, int16_t x, int16_t y,
                      int16_t w, int16_t h, bool pgm = true);  // cmd 0x28
```

Convenzione bitmap input: bit=1 dove il pixel **non** appartiene a quel
canale (formato compatibile con lo script Python e image2cpp). Per gli
accent (red/yellow) il driver applica `~data` prima del transfer SPI
per allinearsi alla polarity nativa SSD1677 (bit=1 in RAM = colorante
acceso).

## 3. Perché il yellow è "out-of-band" nel flusso paged

Il driver custom origina da `GxEPD2_1330c_GDEM133Z91`, un driver del
ramo **`epd3c`** di GxEPD2, pensato per pannelli a **3 colori**
(bianco/nero/+1 accent). Tutta l'organizzazione del rendering di GxEPD2
ruota attorno al template `GxEPD2_3C<Driver, page_height>`, che fa da
intermediario tra il layer GFX (Adafruit_GFX) e il driver. Questo
template ha un'architettura **hard-coded su 2 canali**:

- mantiene un buffer GFX paged in RAM con **due piani** (black + accent)
- nel loop `firstPage()` / `nextPage()` invoca **una sola hook** sul
  driver: `writeImagePart(black, color, ...)`
- non ha né campi né API per un terzo canale

Quando abbiamo confermato che il pannello SOLUM supporta nativamente un
quarto colore (giallo via comando `0x28`), l'opzione "pulita" sarebbe
stata scrivere un template `GxEPD2_4C` custom — fork con buffer paged a
3 piani e nuova hook `writeImagePart(black, red, yellow, ...)`. Sarebbe
stato un refactor invasivo della libreria upstream, fuori scope.

La soluzione pragmatica adottata è il pattern **"yellow out-of-band"**:

```
┌────────────────────────────────────────────────────────────────┐
│  writeImageYellow(buffer_giallo, ...)    ← scrive 0x28 PRIMA   │
│  preserveYellow(true)                    ← protegge 0x28       │
│                                                                 │
│  display.firstPage();                                           │
│  do {                                                           │
│    fillScreen(WHITE);                                           │
│    drawBitmap(...);    // GFX nel buffer paged 2-canali (B+R)  │
│    drawText(...);      // GFX                                  │
│    ...                                                          │
│  } while (display.nextPage());                                  │
│  ↓                                                              │
│  ogni nextPage() chiama writeImagePart(black, color, ...)       │
│  che internamente NON tocca 0x28 finché _preserve_yellow=true   │
│                                                                 │
│  preserveYellow(false)                   ← ripristina cleanup   │
└────────────────────────────────────────────────────────────────┘
```

Il giallo viene **iniettato manualmente** prima del loop paged e
**protetto** dal cleanup automatico tramite il flag `_preserve_yellow`.
Il flag viene **resettato automaticamente** dentro `refresh()` del
driver, chiamato dal template alla fine del loop paged: il chiamante
non deve preoccuparsene.

Tutta questa complessità (writeImageYellow + preserveYellow + decodifica
bitmap pixel-per-pixel + auto-reset) è incapsulata nella free function
template `GxEPDImage::showImage(display, desc)` (vedi §1): il chiamante
deve solo aprire il loop paged.

**Idempotency.** `showImage` è idempotente sul canale 0x28: scrive
`writeImageYellow` al massimo una volta per loop paged grazie al check
`isYellowPreserved()`. Le iterazioni 2..8 di `nextPage()` trovano il
flag attivo e saltano la riscrittura (~110 ms risparmiati per refresh).
Se il chiamante ha già scritto yellow su 0x28 e attivato `preserveYellow(true)`
prima di `firstPage()`, `showImage` non sovrascrive — utile per
compositing yellow custom (vedi caso 6 della tabella in §1).

I 3 siblings `writeImageBlack` / `writeImageRed` / `writeImageYellow`
sono **simmetrici a livello di API**, ma in pratica B+R sono scritti
dal template e Y è scritto manualmente — l'asimmetria viene dal
template GxEPD2_3C, non dal driver.

### Pitfall: `drawPixel(x, y, GxEPD_YELLOW)` non scrive sul piano giallo

Conseguenza diretta dell'architettura 2-canali del template upstream: nel
sorgente di `GxEPD2_3C.h` la funzione `drawPixel` ha questa condizione:

```cpp
else if ((color == GxEPD_RED) || (color == GxEPD_YELLOW))
  _color_buffer[i] = (_color_buffer[i] & ...);   // scrive nel piano red (0x26)
```

`GxEPD_YELLOW` viene **trattato come `GxEPD_RED`** — la libreria non
distingue. Il pixel "giallo" finisce sul piano rosso del controller
(`0x26`) e MAI sul piano giallo (`0x28`). Questo è invisibile su pannelli
3-colori (B+W+R), ma su SOLUM 4-colori si traduce in un pixel rosso al
posto del giallo atteso.

Per pilotare davvero il piano `0x28` ci sono due strade, entrambe già
incapsulate nel driver custom:

1. **`GxEPDImage::showImage(display, descriptor)`** con descrittore
   `FORMAT_BWRY_1BPP` — il giallo viene iniettato direttamente sul
   controller via `writeImageYellow()` e protetto durante il loop paged.
   Strada preferita per immagini pre-encoded.
2. **`writeImageYellow()` + `preserveYellow(true)`** chiamati a mano
   prima di `firstPage()`, per compositing custom (es. la barra
   gialla temp-range del banner Weather, vedi
   [`Weather.h`](../Weather.h)). Il driver auto-resetta il flag dentro
   `refresh()` al termine del loop paged.

## 4. Sistema di descrittori universali (`namespace GxEPDImage`)

```cpp
namespace GxEPDImage {
  enum Format : uint8_t {
    FORMAT_BW_1BPP   = 0,   // 1 buffer 1bpp (compat image2cpp)
    FORMAT_BWR_1BPP  = 1,   // buffer separati black + red
    FORMAT_BWRY_1BPP = 2,   // buffer separati black + red + yellow
  };

  struct Descriptor {
    Format format;
    uint16_t width, height;
    const uint8_t *data0, *data1, *data2;
  };
}
```

Il descrittore porta con sé formato e dimensioni. Per costruire
descrittori inline lo header espone tre macro di comodo:

```cpp
GXEPD_BW_IMAGE(ptr, w, h)
GXEPD_BWR_IMAGE(black, red, w, h)
GXEPD_BWRY_IMAGE(black, red, yellow, w, h)
```

Lo script Python `epd_image_converter.pyw` genera automaticamente una
variabile `img_<nome>_desc` ad ogni conversione, pronta per essere
passata a `showImage()`.

## 5. Ottimizzazioni rispetto al driver stock

### Confronto driver custom vs. base GDEM133Z91 e patterns moderni di GxEPD2

La tabella sotto mette in evidenza ogni divergenza dal driver upstream
[`GxEPD2_1330c_GDEM133Z91`](https://github.com/ZinggJM/GxEPD2/blob/master/src/gdem3c/GxEPD2_1330c_GDEM133Z91.cpp)
(che è il driver SSD1677 più vicino al pannello SOLUM) e dai patterns
adottati negli altri driver moderni della libreria.

| Aspetto | GDEM133Z91 base | Driver custom SOLUM | Stato |
|---|---|---|---|
| Init RAM 3 piani (B/R/Y) | solo B+R (0x24, 0x26) | B+R+Y (0x24, 0x26, 0x28) | ✓ migliore |
| `delay(1)` ESP8266 WDT in hot path | presente in `_writeImage`/`_writeImagePart` | rimosso (target ESP32, task WDT 5 s) | ✓ migliore |
| Delay dopo SWRESET | 10 ms (sotto-stimato) | 200 ms (datasheet SSD1677 ~100-300 ms) | ✓ migliore |
| Entry-mode `0x11` | inviato a ogni `_setPartialRamArea` | inviato 1 volta in `_InitDisplay` | ✓ migliore |
| Cleanup accent dirty-tracking | n/a (cleanup sempre o mai) | flag `_color_dirty` / `_yellow_dirty` + `_cleanAccentIfDirty` | ✓ ottimizzato |
| Polarity cleanup accent | n/a | `0x00` esplicito (= accent spento, polarity nativa) | ✓ corretto vs. bug latente `0xFF` |
| `hibernate()` guard idempotente | scrive 0x10 sempre, anche su re-entry | early return se già `_hibernating` | ✓ migliore |
| `_init_display_done` reset on hibernate | sì | sì | = parità |
| 4° canale (yellow `0x28`) | non gestito | API single-channel + `preserveYellow` | ✓ unica via possibile |
| Cleanup accent in `writeImage(bitmap, ...)` BW | non fa | sì (entrambi 0x26 e 0x28 dirty-checked) | ✓ migliore |
| Cleanup accent in `writeImagePart(bitmap, ...)` BW | non fa | sì (allineato a `writeImage` per simmetria) | ✓ migliore |
| Reset `_preserve_yellow` post-refresh | n/a | centralizzato in `_Update_Full()` | ✓ corretto |
| Reset `_preserve_yellow` in `hibernate()` | n/a | sì (simmetria con refresh) | ✓ corretto |
| Loop pixel di `showImage` | itera tutti i pixel × tutte le 8 page (drawPixel early-return) | row-skip rotation-aware via page-hint counter (1/8 delle iterazioni) | ✓ ottimizzato (~145 ms/refresh) |
| Transfer SPI verso il controller | per-byte `_pSPIx->transfer(uint8_t)` (~1.5 μs/byte) | row-buffered via `_pSPIx->writeBytes(buf, n)` (FIFO 64-byte ESP32, ~0.1 μs/byte effettivi) | ✓ ottimizzato (~290 ms/refresh) |

### Dettaglio delle ottimizzazioni

- **`_setPartialRamArea()`** non riscrive più l'entry-mode ad ogni draw
  (spostato una tantum in `_InitDisplay`).
- **Dirty-flag** `_color_dirty` / `_yellow_dirty`: il cleanup di 0x26 e
  0x28 in `writeImage(bw)` viene saltato se i flag sono zero, evitando
  ~160 ms di SPI per draw quando si concatenano frame B/N.
- **Helper `_cleanAccentIfDirty(cmd, flag)`** centralizza la pulizia
  scrivendo `0x00` (polarity nativa SSD1677 = accent spento), corretto
  rispetto al precedente `0xFF` che scriveva "accent ON ovunque" — bug
  latente mascherato da hibernate+SWRESET ad ogni wake.
- **`hibernate()`** protetto contro chiamate multiple (early return se
  `_hibernating == true`); resetta i dirty-flag e `_preserve_yellow`
  perché il SWRESET successivo riporta la RAM controller a stato noto.
- **`writeImagePart(bitmap, ...)` BW** allineato a `writeImage(bitmap, ...)`:
  pulisce gli accent dirty prima di scrivere il piano `0x24`, evitando
  che red/yellow residui di un draw colorato precedente trasparissero
  sotto la zona BW.
- **Reset `_preserve_yellow`** centralizzato in `_Update_Full()` (chiamato
  da entrambi gli overload di `refresh()`) invece di duplicarlo. Auto-reset
  garantito al termine di ogni ciclo di rendering.
- **`delay(1)` rimossi** da `_writeImage` / `_writeImagePart` (servivano
  come yield WDT per ESP8266; ESP32 ha task WDT 5 s, ampio margine).
  Risparmio ~64 ms per refresh paged completo.
- **`GxEPDImage::showImage` row-skip**: il loop pixel skippa a priori le
  righe sorgente fuori dalla page corrente del template `GxEPD2_3C`,
  riducendo le iterazioni di un fattore ~8 (~145 ms risparmiati per
  refresh full-screen). Il template upstream tiene `_current_page`
  privato senza getter pubblico, quindi il driver custom mantiene un
  counter `_show_image_page_hint` parallelo: reset a 0 dentro
  `setPaged()` (override del hook `virtual` di `GxEPD2_EPD` chiamato da
  `firstPage()` del template), avanzato dentro `writeImage(black, color, ...)`
  (invocato dal template ESATTAMENTE una volta per page in `nextPage()`),
  reset difensivo dentro `_Update_Full()`. Bonus: `showImage` può essere
  chiamata 0, 1 o N volte all'interno della stessa page senza
  desincronizzare il counter (il counter avanza solo a chiusura page,
  non per ogni chiamata `showImage`). Include anche hoist del row offset
  fuori dal loop interno e fallback al loop completo per rotation 1/3
  (90°, dove la skip-by-row non si applica perché le righe sorgente
  mappano sull'asse x dell'output).
- **Bulk SPI transfer**: i tre hot path SPI (`_writeImage`,
  `_writeImagePart`, `_writeScreenBuffer`) usano `_pSPIx->writeBytes(buf, n)`
  invece di `_transfer(uint8_t)` per-byte. Il base class `GxEPD2_EPD`
  espone `_pSPIx` come `protected`, quindi il subclass può chiamarlo
  direttamente. Il loop interno popola un buffer di stack
  (120 byte/riga per le immagini, 256 byte di chunk per la scrittura
  costante) e lo scarica con `writeBytes` che usa la FIFO 64-byte
  dell'ESP32 + DMA, raggiungendo il limite teorico del clock SPI
  (~0.1 μs/byte effettivi vs ~1.5 μs/byte della transfer per-byte upstream
  — `_writeData(buf, n)` di `GxEPD2_EPD` è anch'esso un loop per-byte di
  `transfer()`, NON un bulk vero, vedi
  [GxEPD2-master/src/GxEPD2_EPD.cpp:197-207](../GxEPD2-master/src/GxEPD2_EPD.cpp#L197-L207)).
  Su un refresh full-window worst-case (~322 KB SPI con cleanup accent
  dirty), il tempo totale di transfer SPI passa da ~480 ms a ~190 ms:
  **risparmio ~290 ms per refresh**. Stack temporaneo aggiunto:
  ~120 byte per `_writeImage`/`_writeImagePart`, ~256 byte per
  `_writeScreenBuffer`. API pubblica invariata, ottimizzazione
  trasparente.

## 6. API completa

La lista degli overload `drawImage*` / `writeImage*` / `writeImagePart*`
ereditati dalla base class è in
[../DOCS/drawImage_overloads_it.md](../DOCS/drawImage_overloads_it.md) (o la
versione inglese [../DOCS/drawImage_overloads.md](../DOCS/drawImage_overloads.md)).
Sono override di virtual del base class `GxEPD2_EPD` necessari al
contratto della libreria — non sono pensati per uso diretto: lo sketch
chiama `showImage()` per immagini singole, oppure il template
`GxEPD2_3C` invoca `writeImagePart(black, color)` durante il flusso
paged.

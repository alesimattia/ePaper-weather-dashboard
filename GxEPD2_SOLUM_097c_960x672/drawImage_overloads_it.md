# API del driver `GxEPD2_SOLUM_097c_960x672`

Riferimento completo dei metodi pubblici del driver custom
[GxEPD2_SOLUM_097c_960x672.h](../GxEPD2_SOLUM_097c_960x672.h) per il
pannello SOLUM 9.7" (672×960, controller SSD1677, 4 colori nativi).

## API custom (livello applicativo)

### `GxEPDImage::showImage()` — entry-point unico

Free function template nel namespace `GxEPDImage` (vive nel driver `.h`,
non come metodo classe). È **l'unica funzione pubblica per stampare
un'immagine** sul pannello SOLUM, riusabile da qualsiasi sketch:

```cpp
template<typename DisplayT>
void GxEPDImage::showImage(DisplayT& display,
                           const GxEPDImage::Descriptor& d,
                           int16_t x = 0, int16_t y = 0);
```

Va chiamata **dentro** un loop `firstPage()`/`nextPage()` del template
`GxEPD2_3C`, dopo `fillScreen()` e prima di `nextPage()`. Supporta tutti
e 3 i formati (BW / BWR / BWRY) — gestisce internamente il yellow
out-of-band (`writeImageYellow` + `preserveYellow(true)`).

**Responsabilità del chiamante:**
- Aprire il loop paged (`firstPage()` + `do { ... } while (nextPage())`)
- Chiamare `display.hibernate()` se vuole spegnere il pannello

Il reset di `preserveYellow(false)` per il caso BWRY è **automatico**
dentro `refresh()` del driver, chiamato dal template alla fine del loop.

**Idempotency canale yellow.** Per i descrittori BWRY, `showImage`
chiama `writeImageYellow` al massimo una volta per loop paged: le
iterazioni 2..8 di `nextPage()` trovano `isYellowPreserved()==true` e
saltano la riscrittura (~110 ms risparmiati per refresh @ 10 MHz SPI).
Inoltre, se il chiamante ha già scritto yellow su 0x28 e attivato
`preserveYellow(true)` PRIMA di `firstPage()` (compositing yellow
custom), `showImage` non sovrascrive lo stato preparato.

Esempio one-shot full-screen:

```cpp
display.firstPage();
do {
  display.fillScreen(GxEPD_WHITE);
  GxEPDImage::showImage(display, *desc_ptr);
} while (display.nextPage());
display.hibernate();
```

Per bitmap raw image2cpp B/N usare la macro inline:

```cpp
GxEPDImage::showImage(display, GXEPD_BW_IMAGE(my_array, 960, 672));
```

### `writeImageBlack` / `writeImageRed` / `writeImageYellow` — siblings single-channel

Scrivono un singolo canale del controller, **senza refresh**. Usate
internamente da `showImage()` ma anche disponibili come API pubblica per
compositing manuale. Stessa shape per i 3 canali:

```cpp
void writeImageBlack (const uint8_t* bitmap, int16_t x, int16_t y,
                      int16_t w, int16_t h, bool pgm = true);  // cmd 0x24
void writeImageRed   (const uint8_t* bitmap, int16_t x, int16_t y,
                      int16_t w, int16_t h, bool pgm = true);  // cmd 0x26
void writeImageYellow(const uint8_t* bitmap, int16_t x, int16_t y,
                      int16_t w, int16_t h, bool pgm = true);  // cmd 0x28
```

Convenzione bitmap input: `bit=1` dove il pixel **NON** appartiene a quel
canale (formato compatibile con script Python e image2cpp). Il driver
applica `~data` prima del transfer SPI per gli accent (red/yellow), per
allinearsi alla polarity nativa SSD1677 (`bit=1` in RAM = colorante ON).

Nessuno chiama `refresh()`. Il chiamante è responsabile di farlo (o usare
`showImage()` che lo include).

### `preserveYellow(bool)` / `isYellowPreserved()` — protezione canale 0x28 in flusso paged

```cpp
void preserveYellow(bool preserve);
bool isYellowPreserved() const;
```

Quando `preserveYellow(true)`, il metodo `writeImagePart(black, color, ...)`
chiamato dal template `GxEPD2_3C` durante `nextPage()` **non** azzera
il canale 0x28. Permette di iniettare il giallo manualmente prima del
loop paged e mantenerlo fino al refresh finale.

Il flag viene **resettato automaticamente** dentro `refresh()` del
driver al termine del loop paged: il chiamante non deve fare
`preserveYellow(false)` manualmente.

Il getter `isYellowPreserved()` viene usato da `GxEPDImage::showImage`
come idempotency-check, per evitare di riscrivere 0x28 ad ogni
iterazione del paged loop o di sovrascrivere uno yellow custom
preparato dall'utente.

Dettagli in [README §3 "Perché il yellow è out-of-band"](../README.md#3-perché-il-yellow-è-out-of-band-nel-flusso-paged).

---

## API ereditate da `GxEPD2_EPD` (override virtual)

Mantenute per compatibilità con il template `GxEPD2_3C` e il contratto
della libreria upstream. **Non sono pensate per uso diretto dallo
sketch**: lo sketch usa `showImage()` o lascia che il template invochi
internamente questi metodi durante il flusso paged.

### Bitmap (write senza refresh; coordinate `x` e `w` devono essere multipli di 8)

```cpp
void writeImage(const uint8_t bitmap[],
                int16_t x, int16_t y, int16_t w, int16_t h,
                bool invert = false, bool mirror_y = false, bool pgm = false);

void writeImage(const uint8_t* black, const uint8_t* color,
                int16_t x, int16_t y, int16_t w, int16_t h,
                bool invert = false, bool mirror_y = false, bool pgm = false);
```

### Bitmap part (window di una bitmap più grande)

```cpp
void writeImagePart(const uint8_t bitmap[],
                    int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                    int16_t x, int16_t y, int16_t w, int16_t h,
                    bool invert = false, bool mirror_y = false, bool pgm = false);

void writeImagePart(const uint8_t* black, const uint8_t* color,
                    int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                    int16_t x, int16_t y, int16_t w, int16_t h,
                    bool invert = false, bool mirror_y = false, bool pgm = false);
```

`writeImagePart(black, color, ...)` è il **HOT PATH** invocato 8 volte
per refresh dal template `GxEPD2_3C` durante `nextPage()` (8 = numero
di pagine, dato dal template `GxEPD2_3C<Driver, HEIGHT/8>`).

### Native (sprite raw del controller)

```cpp
void writeNative(const uint8_t* data1, const uint8_t* data2,
                 int16_t x, int16_t y, int16_t w, int16_t h,
                 bool invert = false, bool mirror_y = false, bool pgm = false);
```

### Draw (write + refresh full window)

Stesse signature dei `writeImage*`/`writeImagePart*`/`writeNative` ma
con refresh finale:

```cpp
void drawImage(const uint8_t bitmap[], ...);
void drawImage(const uint8_t* black, const uint8_t* color, ...);
void drawImagePart(const uint8_t bitmap[], ...);
void drawImagePart(const uint8_t* black, const uint8_t* color, ...);
void drawNative(const uint8_t* data1, const uint8_t* data2, ...);
```

### Refresh / power management

```cpp
void refresh(bool partial_update_mode = false);
void refresh(int16_t x, int16_t y, int16_t w, int16_t h);
void powerOff();
void hibernate();
```

`refresh()` esegue sempre un full-window refresh (~22 s), il pannello
non supporta fast partial update. `hibernate()` mette il controller in
deep sleep e va invocato per ridurre i consumi tra refresh distanti
(viene chiamato automaticamente da `showImage()` se `hibernateAfter=true`).

### Buffer init

```cpp
void clearScreen(uint8_t value = 0xFF);                                     // tutto bianco
void clearScreen(uint8_t black_value, uint8_t color_value);                 // BWR
void clearScreen(uint8_t black_value, uint8_t color_value, uint8_t yellow_value); // BWRY
void writeScreenBuffer(uint8_t value = 0xFF);
void writeScreenBuffer(uint8_t black_value, uint8_t color_value);
void writeScreenBuffer(uint8_t black_value, uint8_t color_value, uint8_t yellow_value);
```

`writeScreenBuffer` scrive direttamente sui 3 canali RAM del controller
con i valori indicati (polarity nativa SSD1677: `0x00` = accent spento,
`0xFF` = accent ON ovunque).

---

## Sistema descrittore + macro

```cpp
namespace GxEPDImage {
  enum Format : uint8_t {
    FORMAT_BW_1BPP   = 0,   // 1 buffer 1bpp (compat image2cpp)
    FORMAT_BWR_1BPP  = 1,   // 2 buffer separati: black + red
    FORMAT_BWRY_1BPP = 2,   // 3 buffer separati: black + red + yellow
  };

  struct Descriptor {
    Format format;
    uint16_t width;
    uint16_t height;
    const uint8_t* data0;
    const uint8_t* data1;  // null per BW
    const uint8_t* data2;  // null per BW/BWR
  };
}
```

Macro per istanziare descrittori inline (utili nei `frame array` del
firmware o dentro `showImage(GXEPD_BWRY_IMAGE(...))`):

| Macro | Espansione |
|---|---|
| `GXEPD_BW_IMAGE(ptr, w, h)` | descrittore B/N a 1 buffer |
| `GXEPD_BWR_IMAGE(pb, pr, w, h)` | descrittore 3-colori (black + red) |
| `GXEPD_BWRY_IMAGE(pb, pr, py, w, h)` | descrittore 4-colori (black + red + yellow) |

Lo script `epd_image_converter.pyw` genera automaticamente una variabile
`img_<nome>_desc` di tipo `GxEPDImage::Descriptor` ad ogni conversione,
pronta per essere passata a `showImage()`.

---

## Storia dei cambiamenti rilevanti

- **`showImage(Descriptor&)` + `showImage(uint8_t*, w, h)` (metodi classe)
  + `drawImage(const Descriptor&)` + `drawImageBWRY()` + helper privati
  `_drawDescriptor` / `_drawBWRY`** → tutti rimossi e sostituiti dalla
  free function template `GxEPDImage::showImage<DisplayT>(display, desc)`
  che vive nel namespace `GxEPDImage`. Unico entry-point pubblico per
  stampare un'immagine, con tutta la logica display-specific (yellow
  out-of-band + decodifica bitmap pixel-per-pixel via drawPixel) dentro.
- **`drawImageRGB565` / `rgb565ToQuadColor` / `FORMAT_RGB565` /
  `GXEPD_RGB565_IMAGE`** → rimossi insieme al supporto RGB565.
- **`writeImageYellow` + `preserveYellow`** → introdotti per supportare
  il canale 0x28 nel flusso paged del template `GxEPD2_3C`, che vede
  solo 2 canali (architettura ereditata dal driver upstream).

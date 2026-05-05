# API of the `GxEPD2_SOLUM_097c_960x672` driver

Complete reference for the public methods of the custom driver
[GxEPD2_SOLUM_097c_960x672.h](../GxEPD2_SOLUM_097c_960x672.h) for the
SOLUM 9.7" panel (672×960, SSD1677 controller, 4 native colors).

## Custom API (application level)

### `GxEPDImage::showImage()` — single entry point

Free function template in the `GxEPDImage` namespace (lives in the
driver `.h`, not as a class method). It is **the only public function
to display an image** on the SOLUM panel, reusable from any sketch:

```cpp
template<typename DisplayT>
void GxEPDImage::showImage(DisplayT& display,
                           const GxEPDImage::Descriptor& d,
                           int16_t x = 0, int16_t y = 0);
```

Must be called **inside** a `firstPage()`/`nextPage()` loop of the
`GxEPD2_3C` template, after `fillScreen()` and before `nextPage()`.
Supports all 3 descriptor formats (BW / BWR / BWRY) — internally handles
the yellow out-of-band (`writeImageYellow` + `preserveYellow(true)`).

**Caller responsibilities:**
- Open the paged loop (`firstPage()` + `do { ... } while (nextPage())`)
- Call `display.hibernate()` to power off the panel

The `preserveYellow(false)` reset for the BWRY case is **automatic**
inside the driver's `refresh()`, called by the template at the end of
the paged loop.

**Yellow channel idempotency.** For BWRY descriptors, `showImage` calls
`writeImageYellow` at most once per paged loop: iterations 2..8 of
`nextPage()` find `isYellowPreserved()==true` and skip the rewrite
(~110 ms saved per refresh @ 10 MHz SPI). Additionally, if the caller
has already written yellow on 0x28 and activated `preserveYellow(true)`
BEFORE `firstPage()` (custom yellow compositing), `showImage` does not
overwrite the prepared state.

Minimal one-shot full-screen example:

```cpp
display.firstPage();
do {
  display.fillScreen(GxEPD_WHITE);
  GxEPDImage::showImage(display, *desc_ptr);
} while (display.nextPage());
display.hibernate();
```

For raw image2cpp B/W bitmaps use the inline macro:

```cpp
GxEPDImage::showImage(display, GXEPD_BW_IMAGE(my_array, 960, 672));
```

### `writeImageBlack` / `writeImageRed` / `writeImageYellow` — single-channel siblings

Write a single channel of the controller, **without refresh**. Used
internally by `showImage()` and also exposed as public API for manual
compositing. Same shape for the 3 channels:

```cpp
void writeImageBlack (const uint8_t* bitmap, int16_t x, int16_t y,
                      int16_t w, int16_t h, bool pgm = true);  // cmd 0x24
void writeImageRed   (const uint8_t* bitmap, int16_t x, int16_t y,
                      int16_t w, int16_t h, bool pgm = true);  // cmd 0x26
void writeImageYellow(const uint8_t* bitmap, int16_t x, int16_t y,
                      int16_t w, int16_t h, bool pgm = true);  // cmd 0x28
```

Input bitmap convention: `bit=1` where the pixel does **NOT** belong to
that channel (compatible format with Python script and image2cpp). The
driver applies `~data` before SPI transfer for accents (red/yellow), to
match the native SSD1677 polarity (`bit=1` in RAM = colorant ON).

None of these calls `refresh()`. The caller is responsible for that (or
use `showImage()` which includes it).

### `preserveYellow(bool)` / `isYellowPreserved()` — channel 0x28 protection in paged flow

```cpp
void preserveYellow(bool preserve);
bool isYellowPreserved() const;
```

When `preserveYellow(true)`, the `writeImagePart(black, color, ...)`
method called by the `GxEPD2_3C` template during `nextPage()` does
**not** clear channel 0x28. Allows manually injecting yellow before the
paged loop and keeping it until the final refresh.

The flag is **automatically reset** inside the driver's `refresh()` at
the end of the paged loop: the caller does not need to call
`preserveYellow(false)` manually.

The `isYellowPreserved()` getter is used by `GxEPDImage::showImage` as
an idempotency-check, to avoid rewriting 0x28 on every iteration of
the paged loop or overwriting a custom yellow prepared by the user.

Details in [README §3 "Why yellow is out-of-band"](../README.md#3-perché-il-yellow-è-out-of-band-nel-flusso-paged).

---

## API inherited from `GxEPD2_EPD` (virtual override)

Kept for compatibility with the `GxEPD2_3C` template and the upstream
library contract. **Not intended for direct use from the sketch**: the
sketch uses `showImage()` or lets the template invoke these methods
internally during the paged flow.

### Bitmap (write without refresh; coordinates `x` and `w` must be multiples of 8)

```cpp
void writeImage(const uint8_t bitmap[],
                int16_t x, int16_t y, int16_t w, int16_t h,
                bool invert = false, bool mirror_y = false, bool pgm = false);

void writeImage(const uint8_t* black, const uint8_t* color,
                int16_t x, int16_t y, int16_t w, int16_t h,
                bool invert = false, bool mirror_y = false, bool pgm = false);
```

### Bitmap part (window of a larger bitmap)

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

`writeImagePart(black, color, ...)` is the **HOT PATH** invoked 8 times
per refresh by the `GxEPD2_3C` template during `nextPage()` (8 = number
of pages, given by the `GxEPD2_3C<Driver, HEIGHT/8>` template).

### Native (raw controller sprite)

```cpp
void writeNative(const uint8_t* data1, const uint8_t* data2,
                 int16_t x, int16_t y, int16_t w, int16_t h,
                 bool invert = false, bool mirror_y = false, bool pgm = false);
```

### Draw (write + full-window refresh)

Same signatures as `writeImage*`/`writeImagePart*`/`writeNative` but
with final refresh:

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

`refresh()` always performs a full-window refresh (~22 s); the panel does
not support fast partial update. `hibernate()` puts the controller in
deep sleep and should be called to reduce power consumption between
distant refreshes (called automatically by `showImage()` when
`hibernateAfter=true`).

### Buffer init

```cpp
void clearScreen(uint8_t value = 0xFF);                                     // all white
void clearScreen(uint8_t black_value, uint8_t color_value);                 // BWR
void clearScreen(uint8_t black_value, uint8_t color_value, uint8_t yellow_value); // BWRY
void writeScreenBuffer(uint8_t value = 0xFF);
void writeScreenBuffer(uint8_t black_value, uint8_t color_value);
void writeScreenBuffer(uint8_t black_value, uint8_t color_value, uint8_t yellow_value);
```

`writeScreenBuffer` writes directly to the 3 RAM channels of the
controller with the indicated values (native SSD1677 polarity:
`0x00` = accent OFF, `0xFF` = accent ON everywhere).

---

## Descriptor system + macros

```cpp
namespace GxEPDImage {
  enum Format : uint8_t {
    FORMAT_BW_1BPP   = 0,   // 1 buffer 1bpp (image2cpp compat)
    FORMAT_BWR_1BPP  = 1,   // 2 separate buffers: black + red
    FORMAT_BWRY_1BPP = 2,   // 3 separate buffers: black + red + yellow
  };

  struct Descriptor {
    Format format;
    uint16_t width;
    uint16_t height;
    const uint8_t* data0;
    const uint8_t* data1;  // null for BW
    const uint8_t* data2;  // null for BW/BWR
  };
}
```

Macros to instantiate inline descriptors (useful in firmware `frame
array` or inside `showImage(GXEPD_BWRY_IMAGE(...))`):

| Macro | Expansion |
|---|---|
| `GXEPD_BW_IMAGE(ptr, w, h)` | B/W single-buffer descriptor |
| `GXEPD_BWR_IMAGE(pb, pr, w, h)` | 3-color descriptor (black + red) |
| `GXEPD_BWRY_IMAGE(pb, pr, py, w, h)` | 4-color descriptor (black + red + yellow) |

The `epd_image_converter.pyw` script automatically generates a variable
`img_<name>_desc` of type `GxEPDImage::Descriptor` at each conversion,
ready to be passed to `showImage()`.

---

## Relevant change history

- **`showImage(Descriptor&)` + `showImage(uint8_t*, w, h)` (class methods)
  + `drawImage(const Descriptor&)` + `drawImageBWRY()` + private helpers
  `_drawDescriptor` / `_drawBWRY`** → all removed and replaced by the
  free function template `GxEPDImage::showImage<DisplayT>(display, desc)`
  living in the `GxEPDImage` namespace. Single public entry point for
  displaying an image, with all display-specific logic inside (yellow
  out-of-band + pixel-by-pixel bitmap decoding via drawPixel).
- **`drawImageRGB565` / `rgb565ToQuadColor` / `FORMAT_RGB565` /
  `GXEPD_RGB565_IMAGE`** → removed along with RGB565 support.
- **`writeImageYellow` + `preserveYellow`** → introduced to support the
  0x28 channel in the paged flow of the `GxEPD2_3C` template, which only
  sees 2 channels (architecture inherited from the upstream driver).

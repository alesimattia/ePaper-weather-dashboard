// =============================================================================
// Driver custom per pannello e-paper SOLUM Newton-Core 12.2" 960x768 BWR su ESP32.
//
// Origine:
//   - Libreria base: GxEPD2 (https://github.com/ZinggJM/GxEPD2).
//   - Scheletro strutturale: GxEPD2_1248c (Good Display GDEY1248Z51,
//     controller UC8179, dual-controller master/slave). Per maggiori
//     informazioni: src/epd3c/GxEPD2_1248c.h e GxEPD2_1248c.cpp.
//   - Custom features (showImage page-hint, bulk-SPI writeBytes,
//     _cleanAccentIfDirty): portate da GxEPD2_097c_SOLUM_672x960.h
//     (driver SOLUM 9.7" del progetto, controller SSD1677 single-channel).
//
// Pannello pilotato:
//   - Produttore: SOLUM (modulo ESL Newton-Core 12.2" riusato).
//   - Risoluzione: 960x768 (datasheet PDF Newton-Core_Specifications.pdf).
//   - Colori: 3 colori nativi (bianco / nero / rosso). Niente giallo,
//     a differenza del driver SOLUM 9.7" del progetto.
//   - Connettivita': 2 cavi FFC da 21 pin -> assumption "dual-controller
//     master/slave" coerente con il pattern del 1248c.
//   - Refresh: solo full refresh (~25 s), niente fast partial update.
//
// !!! ASSUMPTION CONTROLLER (TODO[VERIFY] al bring-up):
//   Il datasheet Newton-Core e' marketing-only e NON dichiara il controller
//   IC. La scelta UC8179 e' motivata da:
//     - 2 FFC da 21 pin = pattern dual-controller (coerente con 1248c).
//     - 12.2" ~ 12.48" (1248c) per dimensione fisica.
//   Piano B: se al bring-up il pannello non risponde con la sequenza UC8179,
//   sostituire _InitDisplay/_PowerOn/_PowerOff/_Update_Full/hibernate con la
//   sequenza SSD1677 del driver 9.7" (file GxEPD2_097c_SOLUM_672x960.h).
//   Lo scheletro dual-controller in questo header (ScreenPart M/S,
//   _writeCommandAll/_writeDataAll, _waitWhileAnyBusy) resta valido in
//   entrambi i casi.
//
// !!! ASSUMPTION SPLIT MASTER/SLAVE (TODO[VERIFY] al bring-up):
//   Il datasheet non dichiara come i 2 FFC mappino sui pixel del pannello.
//   Assumption iniziale: split verticale, master = colonne 0..479
//   (sinistra), slave = colonne 480..959 (destra). Coerente con il pattern
//   del 1248c che mette M1/M2 a sinistra e S1/S2 a destra.
//   Se al bring-up solo meta' del display si aggiorna o ci sono artefatti
//   sulla giunzione, valutare:
//     - split orizzontale: master = righe 0..383 (alto), slave = 384..767;
//     - swap master <-> slave: cs_m e cs_s scambiati a livello hardware;
//     - reverse scan diverso (cmd 0x00 panel setting): cambiare _PANEL_S
//       da 0x03 (reverse) a 0x0f (normale) o viceversa.
//
// Requisiti build:
//   - HW SPI (HSPI su ESP32 tramite la Waveshare E-Paper ESP32 Driver Board
//     o cablaggio equivalente).
//   - Target ESP32 (Arduino core): le ottimizzazioni bulk-SPI usano
//     SPI.writeBytes() che e' specifica ESP32.
//   - Adafruit_GFX opzionale (se ENABLE_GxEPD2_GFX=0 il footprint e' minore).
//
// Aggiunte custom rispetto alla base GxEPD2 (riprese dal driver SOLUM 9.7"):
//   - GxEPDImage::showImage(display, descriptor) come UNICO entry-point
//     pubblico per stampare un'immagine. Free function template (vive nel
//     namespace GxEPDImage del .h, non come metodo classe) che accetta
//     descrittori BW / BWR e va chiamata dentro un loop paged
//     firstPage()/nextPage() del template GxEPD2_3C.
//   - 2 API siblings writeImageBlack / writeImageRed per scrittura
//     single-channel diretta sul controller (no GFX).
//   - Bulk-SPI con writeBytes() invece di SPI.transfer() per-byte: saving
//     ~290 ms per refresh full-screen (vedi commento _writeScreenBuffer
//     e _writeImage).
//   - Page-tracking di showImage tramite hint counter: skip a priori delle
//     righe sorgente fuori dalla page corrente del template GxEPD2_3C.
//
// CONVIVENZA CON IL DRIVER SOLUM 9.7":
//   Il driver 9.7" definisce gia' un namespace GxEPDImage con i formati
//   BW / BWR / BWRY. Includere ENTRAMBI gli header nello stesso TU
//   produrrebbe ridefinizione del namespace -> errore di compilazione.
//   In un progetto che usa entrambi i pannelli, mantenere gli include in
//   TU separati (un file .cpp per il 9.7", un altro per il 12.2") oppure
//   scegliere quale dei due includere a runtime tramite #if/#define.
//   Stub yellow: questo driver definisce isYellowPreserved() e
//   writeImageYellow() come no-op per garantire ODR-compatibility se per
//   qualche ragione il template showImage<> del 9.7" finisse istanziato
//   col tipo di questo driver.
//
// Author: Mattia Alesi
// =============================================================================

#ifndef _GxEPD2_122c_SOLUM_960x768_H_
#define _GxEPD2_122c_SOLUM_960x768_H_

#include <GxEPD2_EPD.h>

// ===========================================================================
// Sistema descrittore immagine universale (BWR-only, no yellow).
// Permette di passare a showImage() un puntatore opaco che descrive formato,
// dimensioni e canali dell'immagine. Compatibile con:
//   - bitmap 1bpp B/N generate da image2cpp (const uint8_t* raw)
//   - bitmap 3-colori (black + red) generate da epd_image_converter.pyw
// ===========================================================================
namespace GxEPDImage
{
  enum Format : uint8_t
  {
    FORMAT_BW_1BPP    = 0,  // 1 bpp singolo buffer (compat image2cpp)
    FORMAT_BWR_1BPP   = 1,  // due buffer separati black + red
    // FORMAT_BWRY_1BPP NON disponibile: pannello 12.2" non supporta yellow.
  };

  /**
   * Descrittore universale di immagine. data2 e' presente per compatibilita'
   * di firma con i descrittori del driver 9.7" ma viene ignorato.
   */
  struct Descriptor
  {
    Format format;
    uint16_t width;
    uint16_t height;
    const uint8_t* data0;
    const uint8_t* data1;
    const uint8_t* data2; // ignorato (no yellow)
  };

  /**
   * Unico entry-point pubblico per stampare un'immagine sul pannello SOLUM
   * 12.2" 3-colori (BWR). Va chiamata DENTRO un loop paged firstPage() /
   * nextPage() del template GxEPD2_3C, dopo fillScreen() e prima di
   * nextPage().
   *
   * Strategia (BWR-only, niente protezione yellow):
   *   - canali black + red: decodificati pixel-per-pixel con drawPixel,
   *     perchè la convenzione bit=1=NOT color delle bitmap (output dello
   *     script python e di image2cpp invertito) e' opposta a quella di
   *     Adafruit_GFX::drawBitmap (bit=1=IS color), e BWR richiede il
   *     compositing di 2 piani che drawBitmap non fa nativamente.
   *
   * Page-hint optimization: skippa le righe sorgente che non intersecano
   * la page corrente del template GxEPD2_3C, riducendo il loop pixel a
   * 1/8 delle iterazioni complessive (saving ~145 ms per refresh full).
   *
   * @tparam DisplayT  template instance di GxEPD2_3C<Driver, page_height>
   * @param display    riferimento al display (per drawPixel + display.epd2)
   * @param d          descrittore dell'immagine (BW / BWR)
   * @param x, y       offset dell'angolo top-left (default 0,0)
   */
  template<typename DisplayT>
  inline void showImage(DisplayT& display, const Descriptor& d,
                        int16_t x = 0, int16_t y = 0)
  {
    const int16_t  w      = static_cast<int16_t>(d.width);
    const int16_t  h      = static_cast<int16_t>(d.height);
    const uint16_t stride = static_cast<uint16_t>((w + 7) / 8);

    // Page-hint skip per rotation 0/2 (no swap assi). Per rotation 1/3
    // (90 deg) le righe sorgente mappano sull'asse x dell'output, quindi
    // la skip-by-row non si applica: fallback al loop completo.
    const int16_t pageH    = static_cast<int16_t>(display.pageHeight());
    const int16_t pageY    = static_cast<int16_t>(display.epd2.showImagePageHint()) * pageH;
    const uint8_t rot      = display.getRotation();
    const bool    can_skip = (rot == 0 || rot == 2);

    const int16_t pyStart = can_skip
        ? static_cast<int16_t>(max(static_cast<int16_t>(0), static_cast<int16_t>(pageY - y)))
        : static_cast<int16_t>(0);
    const int16_t pyEnd = can_skip
        ? static_cast<int16_t>(min(h, static_cast<int16_t>(pageY + pageH - y)))
        : h;

    for (int16_t py = pyStart; py < pyEnd; ++py)
    {
      // Hoist del row offset fuori dal loop interno.
      const uint32_t rowOffset = static_cast<uint32_t>(py) * stride;
      const uint8_t* row0 = d.data0 + rowOffset;
      const uint8_t* row1 = d.data1 ? (d.data1 + rowOffset) : nullptr;

      for (int16_t px = 0; px < w; ++px)
      {
        const uint16_t byteIdx = static_cast<uint16_t>(px >> 3);
        const uint8_t  bitMask = 0x80 >> (px & 7);

        uint16_t color = GxEPD_WHITE;
        if (!(pgm_read_byte(row0 + byteIdx) & bitMask))
          color = GxEPD_BLACK;
        else if (row1 && !(pgm_read_byte(row1 + byteIdx) & bitMask))
          color = GxEPD_RED;

        display.drawPixel(x + px, y + py, color);
      }
    }
  }
} // namespace GxEPDImage

// Macro di comodo per descrittori inline (utile per i frame array nello sketch).
#define GXEPD_BW_IMAGE(ptr, w, h)     { GxEPDImage::FORMAT_BW_1BPP,  (w), (h), (const uint8_t*)(ptr), nullptr, nullptr }
#define GXEPD_BWR_IMAGE(pb, pr, w, h) { GxEPDImage::FORMAT_BWR_1BPP, (w), (h), (const uint8_t*)(pb), (const uint8_t*)(pr), nullptr }

// ===========================================================================
// Classe driver.
// ===========================================================================
class GxEPD2_122c_SOLUM_960x768 : public GxEPD2_EPD
{
  public:
    // attributi
    static const uint16_t WIDTH = 960;
    static const uint16_t WIDTH_VISIBLE = WIDTH;
    static const uint16_t HEIGHT = 768;
    // Riuso GDEY1248Z51 come Panel enum (stesso pattern del driver 9.7" che
    // riusa GDEM133Z91): evita la modifica invasiva di GxEPD2.h. La scelta e'
    // motivata dal fatto che 1248c e' la base strutturale di questo driver.
    static const GxEPD2::Panel panel = GxEPD2::GDEY1248Z51;
    static const bool hasColor = true;
    static const bool hasPartialUpdate = true; // partial window addressing, full window refresh
    static const bool hasFastPartialUpdate = false;
    static const uint16_t power_on_time = 200;       // ms (come 1248c)
    static const uint16_t power_off_time = 50;       // ms (come 1248c)
    static const uint16_t full_refresh_time = 25000; // ms, conservativo per 12.2"
    static const uint16_t partial_refresh_time = 25000;

    // Split master/slave (TODO[VERIFY]): meta' larghezza per ogni controller,
    // altezza piena. Coerente con il pattern del 1248c (M = sinistra,
    // S = destra). Le costanti sono usate sia dalla classe outer per dispatch
    // delle scritture sia dalle ScreenPart per l'addressing locale.
    static const uint16_t M_WIDTH = 480;  // master = colonne 0..479
    static const uint16_t S_WIDTH = 480;  // slave  = colonne 480..959
    static const uint16_t PART_HEIGHT = HEIGHT;

    // ----- Costruttori -----
#if defined(ESP32)
    // Costruttore completo ESP32 con SPI espliciti (es. Waveshare ESP32 driver board).
    GxEPD2_122c_SOLUM_960x768(int16_t sck, int16_t miso, int16_t mosi,
                              int16_t cs_m, int16_t cs_s,
                              int16_t dc, int16_t rst,
                              int16_t busy_m, int16_t busy_s);
#endif
    // Costruttore standard SPI (default SCK / MISO / MOSI).
    GxEPD2_122c_SOLUM_960x768(int16_t cs_m, int16_t cs_s,
                              int16_t dc, int16_t rst,
                              int16_t busy_m, int16_t busy_s);
    // Costruttore "compat single-CS" per bring-up con un solo controller cablato
    // (il secondo CS viene passato come -1, le scritture vanno solo al master).
    GxEPD2_122c_SOLUM_960x768(int16_t cs, int16_t dc, int16_t rst, int16_t busy);

    // ----- API pubbliche (pattern GxEPD2 standard, BWR-only) -----
    // Override di init(): il base GxEPD2_EPD::init() configura solo i pin
    // del master (_cs / _rst / _busy / SPI). Per il dual-controller serve
    // estendere con pinMode su _cs_s e _busy_s. Pattern preso da 1248c.
    void init(uint32_t serial_diag_bitrate = 0);
    void init(uint32_t serial_diag_bitrate, bool initial, uint16_t reset_duration = 20, bool pulldown_rst_mode = false);

    void clearScreen(uint8_t value = 0xFF);
    void clearScreen(uint8_t black_value, uint8_t color_value);
    void writeScreenBuffer(uint8_t value = 0xFF);
    void writeScreenBuffer(uint8_t black_value, uint8_t color_value);

    void writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                        int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                        int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);

    void drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                       int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                       int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);

    void refresh(bool partial_update_mode = false);
    void refresh(int16_t x, int16_t y, int16_t w, int16_t h);
    void powerOff();
    void hibernate();

    // ------------------------------------------------------------------
    // API siblings per scrittura single-channel (senza refresh).
    // Stessa shape per i 2 canali del controller UC8179:
    //   writeImageBlack -> cmd 0x10 (black/white plane, no invert)
    //   writeImageRed   -> cmd 0x13 (red accent, invert applicato)
    // Convenzione bitmap input: bit=1 dove il pixel NON appartiene al canale
    // (stesso formato prodotto da epd_image_converter.pyw e image2cpp).
    // ------------------------------------------------------------------
    void writeImageBlack(const uint8_t* bitmap, int16_t x, int16_t y,
                         int16_t w, int16_t h, bool pgm = true);
    void writeImageRed  (const uint8_t* bitmap, int16_t x, int16_t y,
                         int16_t w, int16_t h, bool pgm = true);

    // Hook virtual chiamato da GxEPD2_3C::firstPage() all'inizio di ogni
    // loop paged. Reset del page-hint per allinearlo a _current_page del
    // template (privato senza getter pubblico).
    void setPaged() override { _show_image_page_hint = 0; }

    // Getter del page-hint usato da GxEPDImage::showImage come surrogato
    // di _current_page del template GxEPD2_3C: permette a showImage di
    // skippare a priori le righe sorgente fuori dalla page corrente.
    int16_t showImagePageHint() const { return _show_image_page_hint; }

    // ------------------------------------------------------------------
    // Stub yellow: presenti SOLO per ODR-compatibility con il template
    // showImage<> del driver 9.7" se entrambi gli header finissero
    // inclusi nello stesso TU e il template venisse istanziato col tipo
    // di questo driver. A runtime non vengono mai chiamati per il 12.2"
    // (FORMAT_BWRY_1BPP non e' nemmeno definito qui).
    // ------------------------------------------------------------------
    bool isYellowPreserved() const { return true; }
    void writeImageYellow(const uint8_t* /*bitmap*/, int16_t /*x*/, int16_t /*y*/,
                          int16_t /*w*/, int16_t /*h*/, bool /*pgm*/ = true) {}

  private:
    // ------------------------------------------------------------------
    // ScreenPart: gestisce un singolo controller (master o slave).
    // Pattern preso da GxEPD2_1248c::ScreenPart, semplificato per 2
    // controller invece di 4.
    // ------------------------------------------------------------------
    class ScreenPart
    {
      public:
        const uint16_t WIDTH;
        const uint16_t HEIGHT;
        ScreenPart(uint16_t width, uint16_t height, bool rev_scan, int16_t cs, int16_t dc);
        void writeScreenBuffer(uint8_t command, uint8_t value);
        void writeImagePart(uint8_t command, const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                            int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm);
        void writeCommand(uint8_t c);
        void writeData(uint8_t d);
        bool isActive() const { return _cs >= 0; }
      private:
        bool    _rev_scan;
        int16_t _cs;
        int16_t _dc;
        SPISettings _spi_settings;
        void _setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    };

    // ------------------------------------------------------------------
    // Helpers privati outer-class.
    // ------------------------------------------------------------------
    void _cleanAccentIfDirty(uint8_t command, bool& dirty_flag);
    void _writeScreenBuffer(uint8_t command, uint8_t value);
    void _writeImage(uint8_t command, const uint8_t* bitmap, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm);
    void _writeImagePart(uint8_t command, const uint8_t* bitmap, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                         int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm);
    void _resetDual();
    void _initSPI();
    void _PowerOn();
    void _PowerOff();
    void _InitDisplay();
    void _Update_Full();
    void _writeCommandMaster(uint8_t c);
    void _writeDataMaster(uint8_t d);
    void _writeCommandAll(uint8_t c);
    void _writeDataAll(uint8_t d);
    void _waitWhileAnyBusy(const char* comment, uint16_t busy_time);

    // ------------------------------------------------------------------
    // Stato.
    // ------------------------------------------------------------------
    int16_t _sck, _miso, _mosi;
    int16_t _cs_m, _cs_s;
    int16_t _dc_pin;
    int16_t _rst_pin;
    int16_t _busy_m, _busy_s;
    int8_t  _temperature;

    ScreenPart M;
    ScreenPart S;

    // Dirty flag canale rosso (cmd 0x13). Permette di saltare la pulizia
    // pre-draw quando non serve (catena di immagini B/N consecutive).
    bool _color_dirty = false;

    // Counter usato da GxEPDImage::showImage per dedurre la page corrente
    // del template GxEPD2_3C.
    int16_t _show_image_page_hint = 0;
};

// =============================================================================
// IMPLEMENTAZIONI INLINE
// =============================================================================

// ----- Costruttori outer-class -----

#if defined(ESP32)
inline GxEPD2_122c_SOLUM_960x768::GxEPD2_122c_SOLUM_960x768(
    int16_t sck, int16_t miso, int16_t mosi,
    int16_t cs_m, int16_t cs_s,
    int16_t dc, int16_t rst,
    int16_t busy_m, int16_t busy_s) :
  GxEPD2_EPD(cs_m, dc, rst, busy_m, LOW, 20000000, WIDTH, HEIGHT, panel, hasColor, hasPartialUpdate, hasFastPartialUpdate),
  _sck(sck), _miso(miso), _mosi(mosi),
  _cs_m(cs_m), _cs_s(cs_s), _dc_pin(dc), _rst_pin(rst),
  _busy_m(busy_m), _busy_s(busy_s),
  _temperature(20),
  M(M_WIDTH, PART_HEIGHT, false, cs_m, dc),
  S(S_WIDTH, PART_HEIGHT, true,  cs_s, dc)
{
}
#endif

inline GxEPD2_122c_SOLUM_960x768::GxEPD2_122c_SOLUM_960x768(
    int16_t cs_m, int16_t cs_s,
    int16_t dc, int16_t rst,
    int16_t busy_m, int16_t busy_s) :
  GxEPD2_EPD(cs_m, dc, rst, busy_m, LOW, 20000000, WIDTH, HEIGHT, panel, hasColor, hasPartialUpdate, hasFastPartialUpdate),
  _sck(SCK), _miso(MISO), _mosi(MOSI),
  _cs_m(cs_m), _cs_s(cs_s), _dc_pin(dc), _rst_pin(rst),
  _busy_m(busy_m), _busy_s(busy_s),
  _temperature(20),
  M(M_WIDTH, PART_HEIGHT, false, cs_m, dc),
  S(S_WIDTH, PART_HEIGHT, true,  cs_s, dc)
{
}

// Variante single-CS: utile per bring-up con un solo controller cablato
// fisicamente. Lo slave riceve cs=-1, quindi le scritture verso S sono no-op.
// Mezzo display non aggiornera' (probabilmente meta' destra) ma il bring-up
// del master si puo' validare in isolamento.
inline GxEPD2_122c_SOLUM_960x768::GxEPD2_122c_SOLUM_960x768(int16_t cs, int16_t dc, int16_t rst, int16_t busy) :
  GxEPD2_EPD(cs, dc, rst, busy, LOW, 20000000, WIDTH, HEIGHT, panel, hasColor, hasPartialUpdate, hasFastPartialUpdate),
  _sck(SCK), _miso(MISO), _mosi(MOSI),
  _cs_m(cs), _cs_s(-1), _dc_pin(dc), _rst_pin(rst),
  _busy_m(busy), _busy_s(-1),
  _temperature(20),
  M(M_WIDTH, PART_HEIGHT, false, cs, dc),
  S(S_WIDTH, PART_HEIGHT, true,  -1, dc)
{
}

// ----- API pubbliche outer-class -----

inline void GxEPD2_122c_SOLUM_960x768::clearScreen(uint8_t value)
{
  clearScreen(value, 0x00);
}

inline void GxEPD2_122c_SOLUM_960x768::clearScreen(uint8_t black_value, uint8_t color_value)
{
  writeScreenBuffer(black_value, color_value);
  refresh(false);
}

inline void GxEPD2_122c_SOLUM_960x768::writeScreenBuffer(uint8_t value)
{
  writeScreenBuffer(value, 0x00);
}

// Init dei buffer del controller: B/N (cmd 0x10) e rosso (cmd 0x13).
// UC8179 polarity: cmd 0x10 bit=1=white bit=0=black; cmd 0x13 bit=1=red.
// Per rendere la convenzione bitmap utente uniforme (bit=1=NOT color), il
// canale red viene riempito con ~value (es. value=0xFF -> red plane = 0x00).
inline void GxEPD2_122c_SOLUM_960x768::writeScreenBuffer(uint8_t black_value, uint8_t color_value)
{
  if (!_init_display_done) _InitDisplay();
  _writeScreenBuffer(0x10, black_value);
  _writeScreenBuffer(0x13, color_value);
  _initial_write = false;
  _color_dirty = false;
}

inline void GxEPD2_122c_SOLUM_960x768::_writeScreenBuffer(uint8_t command, uint8_t value)
{
  M.writeScreenBuffer(command, value);
  if (S.isActive()) S.writeScreenBuffer(command, value);
}

// HOT PATH: chiamato dal template GxEPD2_3C in modalita' BW (paged).
// Pulisce il canale rosso se dirty e poi scrive il piano BW.
inline void GxEPD2_122c_SOLUM_960x768::writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (!_initial_write) _cleanAccentIfDirty(0x13, _color_dirty);
  _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm);
}

inline void GxEPD2_122c_SOLUM_960x768::_writeImage(uint8_t command, const uint8_t* bitmap, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (_initial_write) writeScreenBuffer();
  if (!_init_display_done) _InitDisplay();
  if (!bitmap) return;
  // Dispatch master/slave: master gestisce le X 0..M.WIDTH-1, slave le X
  // M.WIDTH..WIDTH-1. La ScreenPart::writeImagePart fa il clipping interno.
  M.writeImagePart(command, bitmap, 0, 0, w, h, x, y, w, h, invert, mirror_y, pgm);
  if (S.isActive())
    S.writeImagePart(command, bitmap, 0, 0, w, h, x - int16_t(M.WIDTH), y, w, h, invert, mirror_y, pgm);
}

inline void GxEPD2_122c_SOLUM_960x768::writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (!_initial_write) _cleanAccentIfDirty(0x13, _color_dirty);
  _writeImagePart(0x10, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

inline void GxEPD2_122c_SOLUM_960x768::_writeImagePart(uint8_t command, const uint8_t* bitmap, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (_initial_write) writeScreenBuffer();
  if (!_init_display_done) _InitDisplay();
  if (!bitmap) return;
  M.writeImagePart(command, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  if (S.isActive())
    S.writeImagePart(command, bitmap, x_part, y_part, w_bitmap, h_bitmap, x - int16_t(M.WIDTH), y, w, h, invert, mirror_y, pgm);
}

// HOT PATH (paged full-window): GxEPD2_3C::nextPage() in modalita' full-window
// chiama questa overload una volta per page. Avanza il page-hint dopo aver
// scritto la page corrente sul controller, cosi' GxEPDImage::showImage
// nella prossima iterazione skippa le righe gia' scritte.
inline void GxEPD2_122c_SOLUM_960x768::writeImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black) _writeImage(0x10, black, x, y, w, h, invert, mirror_y, pgm);
  if (color)
  {
    _writeImage(0x13, color, x, y, w, h, !invert, mirror_y, pgm);
    _color_dirty = true;
  }
  _show_image_page_hint++;
}

inline void GxEPD2_122c_SOLUM_960x768::writeImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black) _writeImagePart(0x10, black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  if (color)
  {
    _writeImagePart(0x13, color, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, !invert, mirror_y, pgm);
    _color_dirty = true;
  }
}

inline void GxEPD2_122c_SOLUM_960x768::writeNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImage(data1, data2, x, y, w, h, invert, mirror_y, pgm);
}

inline void GxEPD2_122c_SOLUM_960x768::drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImage(bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
}

inline void GxEPD2_122c_SOLUM_960x768::drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImagePart(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
}

inline void GxEPD2_122c_SOLUM_960x768::drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImage(black, color, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
}

inline void GxEPD2_122c_SOLUM_960x768::drawImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImagePart(black, color, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
}

inline void GxEPD2_122c_SOLUM_960x768::drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeNative(data1, data2, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
}

inline void GxEPD2_122c_SOLUM_960x768::refresh(bool /*partial_update_mode*/)
{
  _Update_Full(); // sempre full window
}

inline void GxEPD2_122c_SOLUM_960x768::refresh(int16_t /*x*/, int16_t /*y*/, int16_t /*w*/, int16_t /*h*/)
{
  _Update_Full();
}

inline void GxEPD2_122c_SOLUM_960x768::powerOff()
{
  _PowerOff();
}

// Deep sleep UC8179: cmd 0x07 0xA5. Protetto contro chiamate multiple.
inline void GxEPD2_122c_SOLUM_960x768::hibernate()
{
  if (_hibernating) return;
  _PowerOff();
  if (_rst >= 0)
  {
    _writeCommandAll(0x07);
    _writeDataAll(0xA5);
    _hibernating = true;
    _init_display_done = false;
    _color_dirty = false;
  }
}

// ----- API single-channel -----

inline void GxEPD2_122c_SOLUM_960x768::writeImageBlack(const uint8_t* bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool pgm)
{
  if (!bitmap) return;
  _writeImage(0x10, bitmap, x, y, w, h, false, false, pgm);
}

inline void GxEPD2_122c_SOLUM_960x768::writeImageRed(const uint8_t* bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool pgm)
{
  if (!bitmap) return;
  _writeImage(0x13, bitmap, x, y, w, h, true, false, pgm);
  _color_dirty = true;
}

// ----- Helper privati outer-class -----

// Pulizia selettiva di un canale accent: scrive 0x00 ovunque (polarity nativa
// UC8179 = "accent spento") e resetta il flag dirty.
inline void GxEPD2_122c_SOLUM_960x768::_cleanAccentIfDirty(uint8_t command, bool& dirty_flag)
{
  if (dirty_flag)
  {
    _writeScreenBuffer(command, 0x00);
    dirty_flag = false;
  }
}

inline void GxEPD2_122c_SOLUM_960x768::_resetDual()
{
  if (_rst_pin >= 0)
  {
    digitalWrite(_rst_pin, LOW);
    delay(10);
    digitalWrite(_rst_pin, HIGH);
    delay(10);
  }
  _hibernating = false;
}

// Override di init(): pattern 1248c. Configura pinMode di tutti i pin
// (master + slave) e poi avvia SPI con i pin custom se ESP32 lo richiede.
// NON chiama GxEPD2_EPD::init() base, perche' il base assume single-CS.
inline void GxEPD2_122c_SOLUM_960x768::init(uint32_t serial_diag_bitrate)
{
  init(serial_diag_bitrate, true, 20, false);
}

inline void GxEPD2_122c_SOLUM_960x768::init(uint32_t serial_diag_bitrate, bool initial, uint16_t reset_duration, bool pulldown_rst_mode)
{
  _initial_write = initial;
  _initial_refresh = initial;
  _pulldown_rst_mode = pulldown_rst_mode;
  _power_is_on = false;
  _using_partial_mode = false;
  _hibernating = false;
  _init_display_done = false;
  _reset_duration = reset_duration;
  if (serial_diag_bitrate > 0)
  {
    Serial.begin(serial_diag_bitrate);
    _diag_enabled = true;
  }
  // Pin master + slave.
  pinMode(_cs_m, OUTPUT);
  digitalWrite(_cs_m, HIGH);
  if (_cs_s >= 0)
  {
    pinMode(_cs_s, OUTPUT);
    digitalWrite(_cs_s, HIGH);
  }
  pinMode(_dc_pin, OUTPUT);
  digitalWrite(_dc_pin, HIGH);
  if (_rst_pin >= 0)
  {
    pinMode(_rst_pin, OUTPUT);
    digitalWrite(_rst_pin, HIGH);
  }
  pinMode(_busy_m, INPUT);
  if (_busy_s >= 0) pinMode(_busy_s, INPUT);
  _initSPI();
  _resetDual();
}

inline void GxEPD2_122c_SOLUM_960x768::_initSPI()
{
#if defined(ESP32)
  if ((SCK != _sck) || (MISO != _miso) || (MOSI != _mosi))
  {
    SPI.end();
    SPI.begin(_sck, _miso, _mosi, _cs_m);
  }
  else SPI.begin();
#else
  SPI.begin();
#endif
}

inline void GxEPD2_122c_SOLUM_960x768::_PowerOn()
{
  if (!_power_is_on)
  {
    _writeCommandMaster(0x04); // power on (UC8179)
    _waitWhileAnyBusy("_PowerOn", power_on_time);
  }
  _power_is_on = true;
}

inline void GxEPD2_122c_SOLUM_960x768::_PowerOff()
{
  if (_power_is_on)
  {
    _writeCommandMaster(0x02); // power off (UC8179)
    _waitWhileAnyBusy("_PowerOff", power_off_time);
  }
  _power_is_on = false;
}

// Sequenza UC8179 portata da GxEPD2_1248c::_InitDisplay, semplificata per
// 2 controller (M, S) invece di 4 (M1, S1, M2, S2). Tutti i valori di
// resolution setting (cmd 0x61) sono ricalcolati per WIDTH=960 HEIGHT=768
// con split master/slave 480/480.
//
// TODO[VERIFY] al bring-up:
//   - cmd 0x00 panel setting: M=0x0f (BWROTP normal scan), S=0x03 (BWROTP
//     reverse scan). Speculare al pattern 1248c. Se al bring-up solo meta'
//     del display si aggiorna o lo split e' invertito, scambiare i valori.
//   - cmd 0x61 resolution: 480x768. Il valore "768" in 2 byte BE = 0x0300.
//   - cmd 0x06 booster soft start: valori 0x27 0x27 0x18 0x17 mantenuti
//     dal 1248c. Se il pannello ha contrasto basso o ghosting persistente,
//     calibrare leggendo il datasheet UC8179.
inline void GxEPD2_122c_SOLUM_960x768::_InitDisplay()
{
  if (_hibernating) _resetDual();
  // panel setting (cmd 0x00)
  M.writeCommand(0x00);
  M.writeData(0x0f);  // BWROTP, scan normale
  if (S.isActive())
  {
    S.writeCommand(0x00);
    S.writeData(0x03);  // BWROTP, scan reverse (per la meta' destra)
  }
  // booster soft start (cmd 0x06, solo master, lo slave segue via cascade setting)
  M.writeCommand(0x06);
  M.writeData(0x27);
  M.writeData(0x27);
  M.writeData(0x18);
  M.writeData(0x17);
  // resolution setting (cmd 0x61) per ogni ScreenPart: 480 source x 768 gate
  // 480 = 0x01E0, 768 = 0x0300
  M.writeCommand(0x61);
  M.writeData(0x01); M.writeData(0xE0);  // source 480
  M.writeData(0x03); M.writeData(0x00);  // gate 768
  if (S.isActive())
  {
    S.writeCommand(0x61);
    S.writeData(0x01); S.writeData(0xE0);
    S.writeData(0x03); S.writeData(0x00);
  }
  // DUSPI: SPI mode = single DIN
  _writeCommandAll(0x15);
  _writeDataAll(0x20);
  // Vcom and data interval setting
  _writeCommandAll(0x50);
  _writeDataAll(0x11); // border KW
  _writeDataAll(0x07);
  // TCON setting
  _writeCommandAll(0x60);
  _writeDataAll(0x22);
  // Spacing (cmd 0xE3) e cascade setting (cmd 0xE0): copiati dal 1248c.
  _writeCommandAll(0xE3);
  _writeDataAll(0x00);
  _writeCommandAll(0xE0);
  _writeDataAll(0x03);
  // Force temperature (cmd 0xE5): 20 default, da affinare con
  // _getMasterTemperature se ghosting termico osservabile.
  _writeCommandAll(0xE5);
  _writeDataAll(_temperature);

  _PowerOn();
  _init_display_done = true;
}

// Esegue il refresh elettroforetico full-window (~25 s). UC8179 usa cmd 0x12
// Display Refresh che innesca il ciclo completo (load LUT + scan).
inline void GxEPD2_122c_SOLUM_960x768::_Update_Full()
{
  _writeCommandAll(0x12);
  _waitWhileAnyBusy("_Update_Full", full_refresh_time);
  _show_image_page_hint = 0;
}

// ----- Dispatch comandi master/slave (pattern 1248c semplificato) -----

inline void GxEPD2_122c_SOLUM_960x768::_writeCommandMaster(uint8_t c)
{
  SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
  digitalWrite(_dc_pin, LOW);
  digitalWrite(_cs_m, LOW);
  SPI.transfer(c);
  digitalWrite(_cs_m, HIGH);
  digitalWrite(_dc_pin, HIGH);
  SPI.endTransaction();
}

inline void GxEPD2_122c_SOLUM_960x768::_writeDataMaster(uint8_t d)
{
  SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs_m, LOW);
  SPI.transfer(d);
  digitalWrite(_cs_m, HIGH);
  SPI.endTransaction();
}

inline void GxEPD2_122c_SOLUM_960x768::_writeCommandAll(uint8_t c)
{
  SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
  digitalWrite(_dc_pin, LOW);
  digitalWrite(_cs_m, LOW);
  if (_cs_s >= 0) digitalWrite(_cs_s, LOW);
  SPI.transfer(c);
  digitalWrite(_cs_m, HIGH);
  if (_cs_s >= 0) digitalWrite(_cs_s, HIGH);
  digitalWrite(_dc_pin, HIGH);
  SPI.endTransaction();
}

inline void GxEPD2_122c_SOLUM_960x768::_writeDataAll(uint8_t d)
{
  SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs_m, LOW);
  if (_cs_s >= 0) digitalWrite(_cs_s, LOW);
  SPI.transfer(d);
  digitalWrite(_cs_m, HIGH);
  if (_cs_s >= 0) digitalWrite(_cs_s, HIGH);
  SPI.endTransaction();
}

// Attende che entrambi i controller (master + slave) abbiano rilasciato il
// pin BUSY. Pattern OR-degli-AND-negati: usciamo solo quando NON e' busy
// alcuno dei due. Se _busy_s < 0 (single-controller bring-up) ignora lo slave.
inline void GxEPD2_122c_SOLUM_960x768::_waitWhileAnyBusy(const char* comment, uint16_t busy_time)
{
  if (_busy_m >= 0)
  {
    delay(1);
    unsigned long start = micros();
    while (true)
    {
      delay(1);
      bool nb_m = (_busy_level != digitalRead(_busy_m));
      bool nb_s = (_busy_s >= 0) ? (_busy_level != digitalRead(_busy_s)) : true;
      if (nb_m && nb_s) break;
      if (micros() - start > _busy_timeout)
      {
        if (_diag_enabled) Serial.println("Busy Timeout!");
        break;
      }
    }
    if (comment && _diag_enabled)
    {
      Serial.print(comment); Serial.print(" : "); Serial.println(micros() - start);
    }
  }
  else delay(busy_time);
}

// =============================================================================
// IMPLEMENTAZIONI INLINE — ScreenPart (controller singolo: master o slave)
// =============================================================================

inline GxEPD2_122c_SOLUM_960x768::ScreenPart::ScreenPart(uint16_t width, uint16_t height, bool rev_scan, int16_t cs, int16_t dc) :
  WIDTH(width), HEIGHT(height), _rev_scan(rev_scan), _cs(cs), _dc(dc),
  _spi_settings(20000000, MSBFIRST, SPI_MODE0)
{
}

// Bulk-SPI: invece di chiamare SPI.transfer(value) WIDTH*HEIGHT/8 volte
// (=46080 byte per controller = 0.36s a 1.5us/byte), pre-riempiamo un buffer
// di stack e lo scarichiamo a chunk via writeBytes(). Saving ~290 ms per
// refresh full-screen rispetto al pattern per-byte del 1248c originale.
inline void GxEPD2_122c_SOLUM_960x768::ScreenPart::writeScreenBuffer(uint8_t command, uint8_t value)
{
  if (_cs < 0) return; // ScreenPart non attiva
  writeCommand(command);
  uint8_t buf[256];
  memset(buf, value, sizeof(buf));
  uint32_t remaining = uint32_t(WIDTH) * uint32_t(HEIGHT) / 8;
  SPI.beginTransaction(_spi_settings);
  digitalWrite(_cs, LOW);
  while (remaining > 0)
  {
    uint32_t chunk = remaining > sizeof(buf) ? (uint32_t)sizeof(buf) : remaining;
    SPI.writeBytes(buf, chunk);
    remaining -= chunk;
  }
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

// Scrittura partial della meta' di pannello pertinente a questa ScreenPart.
// Bulk-SPI per riga: buffer di max WIDTH/8 byte (60 byte per WIDTH=480),
// flush via writeBytes una volta per riga invece di per-byte transfer().
//
// L'addressing usa cmd 0x91 (partial in) / 0x90 (partial window setting) /
// 0x92 (partial out), pattern del 1248c. Il _setPartialRamArea applica
// _rev_scan se la ScreenPart e' marked reverse (= meta' destra del display
// con scan invertito).
inline void GxEPD2_122c_SOLUM_960x768::ScreenPart::writeImagePart(uint8_t command, const uint8_t bitmap[],
    int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (_cs < 0) return;
  if ((w_bitmap < 0) || (h_bitmap < 0) || (w < 0) || (h < 0)) return;
  if ((x_part < 0) || (x_part >= w_bitmap)) return;
  if ((y_part < 0) || (y_part >= h_bitmap)) return;
  int32_t wb_bitmap = (w_bitmap + 7) / 8;
  x_part -= x_part % 8;
  w = w_bitmap - x_part < w ? w_bitmap - x_part : w;
  h = h_bitmap - y_part < h ? h_bitmap - y_part : h;
  x -= x % 8;
  w = 8 * ((w + 7) / 8);
  int16_t x1 = x < 0 ? 0 : x;
  int16_t y1 = y < 0 ? 0 : y;
  int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x;
  int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y;
  int16_t dx = x1 - x;
  int16_t dy = y1 - y;
  w1 -= dx;
  h1 -= dy;
  if ((w1 <= 0) || (h1 <= 0)) return;

  writeCommand(0x91); // partial in
  _setPartialRamArea(x1, y1, w1, h1);
  writeCommand(command);

  const int16_t rowBytes = w1 / 8;
  uint8_t rowBuf[64]; // max WIDTH/8 = 480/8 = 60
  SPI.beginTransaction(_spi_settings);
  digitalWrite(_cs, LOW);
  for (int16_t i = 0; i < h1; i++)
  {
    for (int16_t j = 0; j < rowBytes; j++)
    {
      uint8_t data;
      int32_t idx = mirror_y
          ? x_part / 8 + j + dx / 8 + ((h_bitmap - 1 - (y_part + i + dy))) * wb_bitmap
          : x_part / 8 + j + dx / 8 + (y_part + i + dy) * wb_bitmap;
      if (pgm)
      {
#if defined(__AVR) || defined(ESP8266) || defined(ESP32)
        data = pgm_read_byte(&bitmap[idx]);
#else
        data = bitmap[idx];
#endif
      }
      else data = bitmap[idx];
      if (invert) data = ~data;
      rowBuf[j] = data;
    }
    SPI.writeBytes(rowBuf, rowBytes);
  }
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();

  writeCommand(0x92); // partial out
}

inline void GxEPD2_122c_SOLUM_960x768::ScreenPart::writeCommand(uint8_t c)
{
  if (_cs < 0) return;
  SPI.beginTransaction(_spi_settings);
  if (_dc >= 0) digitalWrite(_dc, LOW);
  digitalWrite(_cs, LOW);
  SPI.transfer(c);
  digitalWrite(_cs, HIGH);
  if (_dc >= 0) digitalWrite(_dc, HIGH);
  SPI.endTransaction();
}

inline void GxEPD2_122c_SOLUM_960x768::ScreenPart::writeData(uint8_t d)
{
  if (_cs < 0) return;
  SPI.beginTransaction(_spi_settings);
  digitalWrite(_cs, LOW);
  SPI.transfer(d);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

// Imposta la finestra parziale RAM del controller (cmd 0x90, UC8179).
// Se _rev_scan e' true (slave / meta' destra del pannello fisico), riflette
// X attorno al centro della ScreenPart per allineare l'addressing alla
// numerazione fisica delle source line invertite.
inline void GxEPD2_122c_SOLUM_960x768::ScreenPart::_setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  if (_rev_scan) x = WIDTH - w - x;
  uint16_t xe = (x + w - 1) | 0x0007; // byte boundary inclusivo
  uint16_t ye = y + h - 1;
  x &= 0xFFF8; // byte boundary
  writeCommand(0x90);
  writeData(x  / 256); writeData(x  % 256);
  writeData(xe / 256); writeData(xe % 256);
  writeData(y  / 256); writeData(y  % 256);
  writeData(ye / 256); writeData(ye % 256);
  writeData(0x01);
}

#endif

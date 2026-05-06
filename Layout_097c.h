#ifndef LAYOUT_097C_H
#define LAYOUT_097C_H

/**
 * Layout per pannello SOLUM ESL 9.7" (controller SSD1677, 4 colori nativi
 * BWRY, native 672x960 -> setRotation(0) -> landscape 960x672 visibile).
 *
 * Centralizza tutti i parametri layout del display:
 *  - tipo pannello (Layout::Panel) e pin del driver
 *  - geometria schermo, sidebar, banner, riquadro calendario, area eventi
 *  - dimensioni e URL del wallpaper cinema
 *  - dimensioni delle icone meteo e indoor
 *  - selezione font usati dai moduli (Layout::FONT_*)
 *
 * Lo sketch .ino seleziona quale Layout includere via Layout.h. Per
 * compilare con un altro pannello SOLUM 12.2" (960x768) si commenta
 * DISPLAY_VARIANT_097C nello sketch e si scommenta DISPLAY_VARIANT_122C:
 * il dispatcher Layout.h ridirige all'header Layout_122c.h.
 */

#include <Arduino.h>
#include <stdint.h>
#include <Adafruit_GFX.h>

#include "GxEPD2_SOLUM_097c_960x672/GxEPD2_SOLUM_097c_960x672.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/Picopixel.h>

namespace Layout
{
  // -------------------------------------------------------------------------
  // Driver / pannello
  // -------------------------------------------------------------------------
  using Panel = GxEPD2_SOLUM_097c_960x672;

  // Pin del driver display (CS, DC, RST, BUSY) sul costruttore Panel(...).
  // Il bus HSPI (SCK/MISO/MOSI) e' condiviso fra le varianti (board-level)
  // e resta nello sketch .ino.
  inline constexpr int PIN_CS   = 15;
  inline constexpr int PIN_DC   = 27;
  inline constexpr int PIN_RST  = 26;
  inline constexpr int PIN_BUSY = 25;

  // Orientamento e geometria visibile dopo setRotation(ROTATION).
  inline constexpr uint8_t ROTATION = 0;
  inline constexpr int16_t SCREEN_W = 960;
  inline constexpr int16_t SCREEN_H = 672;

  // -------------------------------------------------------------------------
  // Font (semantic role, non size). Layout_122c.h puo' rimappare a font
  // diversi senza che i moduli vengano modificati.
  // -------------------------------------------------------------------------
  /** Numeri grandi di rilievo: temperatura percepita nel banner. */
  inline constexpr const GFXfont* FONT_LARGE_BOLD = &FreeSansBold18pt7b;
  /** Testo grande non-bold: orario, indoor T/RH/P, sunrise/sunset, nome mese. */
  inline constexpr const GFXfont* FONT_LARGE      = &FreeSans18pt7b;
  /** Testo standard: titoli fieldset, descrizione meteo, eventi calendario,
   *  header giorni, numeri delle celle del calendario. */
  inline constexpr const GFXfont* FONT_BODY       = &FreeSans12pt7b;
  /** Etichette ridotte: condizione IAQ + valore (riga 3 indoor). */
  inline constexpr const GFXfont* FONT_SMALL      = &FreeSans9pt7b;
  /** Font micro ~6 px: pedice accuracy IAQ. */
  inline constexpr const GFXfont* FONT_MICRO      = &Picopixel;

  // -------------------------------------------------------------------------
  // Sidebar (calendario + lista eventi a destra del wallpaper).
  // -------------------------------------------------------------------------
  inline constexpr int16_t SIDEBAR_W = 340;
  inline constexpr int16_t SIDEBAR_X = SCREEN_W - SIDEBAR_W;   // 620

  // -------------------------------------------------------------------------
  // Wallpaper cinema (a sinistra, sotto la sidebar). Una sorgente piu'
  // alta di CINEMA_H invade la fascia bianca CINEMA_H..BANNER_Y; una piu'
  // larga di CINEMA_W entra nella sidebar.
  // -------------------------------------------------------------------------
  inline constexpr int16_t  CINEMA_W         = 620;
  inline constexpr int16_t  CINEMA_H         = 440;
  inline constexpr uint16_t CINEMA_STRIDE    = (CINEMA_W + 7) / 8;             // 78
  inline constexpr uint32_t CINEMA_PLANE_SZ  = (uint32_t)CINEMA_STRIDE * CINEMA_H;  // 34320
  inline constexpr uint32_t CINEMA_TOTAL_SZ  = CINEMA_PLANE_SZ * 3;            // 102960

  /** URL del server cinema. width/height/colors devono corrispondere a
   *  CINEMA_W/H/BWRY: il server pre-genera i 3 piani 1bpp packed e l'ESP32
   *  fa solo readBytes nei buffer. */
  inline constexpr const char* CINEMA_URL =
      "https://cinema-epd.onrender.com/cinema/arduino?width=620&height=440&colors=bwry&dither=floyd";

  // -------------------------------------------------------------------------
  // Banner meteo (3 fieldset in basso). Layout fisso 5+154+10+306+10+470+5.
  // Tutte le baseline interne sono relative a BANNER_Y per cascata pulita
  // quando si cambia variante (banner ancorato al fondo del display).
  // -------------------------------------------------------------------------
  inline constexpr int16_t BANNER_Y = 460;
  inline constexpr int16_t BANNER_H = 212;
  inline constexpr int16_t BANNER_W = SCREEN_W;

  inline constexpr int16_t INDOOR_RR_X   = 5;
  inline constexpr int16_t INDOOR_RR_W   = 154;
  inline constexpr int16_t WEATHER_RR_X  = 169;
  inline constexpr int16_t WEATHER_RR_W  = 306;
  inline constexpr int16_t FORECAST_RR_X = 485;
  inline constexpr int16_t FORECAST_RR_W = 470;

  /** Blocco meteo corrente dentro il riquadro Weather (sezione sx).
   *  Il resto, da SUN_COL_OFFSET in poi, ospita la sub-col sun. */
  inline constexpr int16_t BLOCK_CURRENT_X = WEATHER_RR_X - 12;
  inline constexpr int16_t BLOCK_CURRENT_W = 200;
  inline constexpr int16_t SUN_COL_OFFSET  = 188;

  /** 3 slot forecast da BLOCK_FC_W px ognuno dentro il riquadro Forecast. */
  inline constexpr int16_t BLOCK_FC_W  = 156;
  inline constexpr int16_t BLOCK_FC0_X = FORECAST_RR_X;
  inline constexpr int16_t BLOCK_FC1_X = BLOCK_FC0_X + BLOCK_FC_W;
  inline constexpr int16_t BLOCK_FC2_X = BLOCK_FC1_X + BLOCK_FC_W;

  /** Baseline dei testi dentro un blocco meteo (current / forecast). */
  inline constexpr int16_t ICON_Y        = BANNER_Y + 6;     // top icona
  inline constexpr int16_t DESC_BASELINE = BANNER_Y + 118;
  inline constexpr int16_t TEMP_BASELINE = BANNER_Y + 153;
  inline constexpr int16_t TIME_BASELINE = BANNER_Y + 188;

  /** Baseline delle 4 righe condivise da Indoor (colonna unica) e dalla
   *  sub-col sun nel riquadro Weather. Spaziatura uniforme (interlinea 46). */
  inline constexpr int16_t INDOOR_ROW1_BASELINE = BANNER_Y + 46;
  inline constexpr int16_t INDOOR_ROW2_BASELINE = BANNER_Y + 92;
  inline constexpr int16_t INDOOR_ROW3_BASELINE = BANNER_Y + 138;
  inline constexpr int16_t INDOOR_ROW4_BASELINE = BANNER_Y + 184;

  inline constexpr int16_t INDOOR_ICON_GAP    = 6;
  inline constexpr int16_t INDOOR_COL1_OFFSET = 10;

  // -------------------------------------------------------------------------
  // Riquadro mese (in cima alla sidebar). 320x200 con bordi arrotondati e
  // titolo del mese sul bordo superiore in stile fieldset.
  // -------------------------------------------------------------------------
  inline constexpr int16_t CAL_X        = SIDEBAR_X + 10;   // 630
  inline constexpr int16_t CAL_Y        = 10;
  inline constexpr int16_t CAL_W        = 320;
  inline constexpr int16_t CAL_H        = 200;
  inline constexpr int16_t CAL_R        = 14;
  inline constexpr int16_t CELL_R       = 4;
  inline constexpr int16_t CAL_PAD      = 6;
  inline constexpr int16_t TITLE_H      = 20;
  inline constexpr int16_t HDR_H        = 26;
  inline constexpr int16_t GRID_TOP_PAD = 3;

  // -------------------------------------------------------------------------
  // Area eventi (sotto al riquadro mese, fino al top del banner meteo).
  // EVT_H scelto per stare in sidebar alta = BANNER_Y, con gap 10 px sopra
  // e sotto rispetto al mese e al banner.
  // -------------------------------------------------------------------------
  inline constexpr int16_t EVT_X        = CAL_X;
  inline constexpr int16_t EVT_Y        = CAL_Y + CAL_H + 10;     // 220
  inline constexpr int16_t EVT_W        = CAL_W;
  inline constexpr int16_t EVT_H        = 230;
  inline constexpr int16_t EVT_SEP_PAD  = 15;
  inline constexpr int16_t EVT_PAD      = 8;
  inline constexpr int16_t EVT_PAD_LEFT = 3;

  // -------------------------------------------------------------------------
  // Icone (lato in pixel). Le bitmap vivono in icons.h.
  // -------------------------------------------------------------------------
  /** Icone meteo (8-multiple per packing). 88 lascia margine verticale nel
   *  banner per il 18pt dell'orario. */
  inline constexpr int16_t ICON_SIZE        = 88;
  /** Icone della colonna indoor (T/RH/IAQ/P) e della sub-col sun. */
  inline constexpr int16_t INDOOR_ICON_SIZE = 20;

  // -------------------------------------------------------------------------
  // Banner fieldset / titoli (parametri estetici condivisi).
  // -------------------------------------------------------------------------
  inline constexpr int16_t BANNER_RR_INSET_Y       = 5;
  inline constexpr int16_t BANNER_RR_RADIUS       = 18;
  inline constexpr int16_t BANNER_TITLE_LEFT_OFFSET = 14;

  // -------------------------------------------------------------------------
  // Barra temp-range (canale yellow out-of-band sotto la sub-col sun).
  // TRB_W deve essere multiplo di 8 per byte-align senza padding.
  // -------------------------------------------------------------------------
  inline constexpr int16_t TRB_W             = 112;
  inline constexpr int16_t TRB_H             = 14;
  inline constexpr int16_t TRB_BYTES_PER_ROW = TRB_W / 8;
  inline constexpr int16_t TRB_CELL_Y_OFFSET = -12;   // cellY = INDOOR_ROW3_BASELINE + offset
}

#endif

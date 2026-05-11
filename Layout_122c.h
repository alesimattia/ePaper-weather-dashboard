#ifndef LAYOUT_122C_H
#define LAYOUT_122C_H

/**
 * Layout per pannello SOLUM 12.2" (controller SSD1677, 4 colori nativi
 * BWRY, native 768x960 -> setRotation(0) -> landscape 960x768 visibile).
 *
 * Variante "scalata" del layout 097c. La larghezza del display e' identica
 * (960 px), quindi il banner e la sidebar mantengono X e W invariati.
 * L'altezza cresce di 96 px (672 -> 768): i 96 px in piu' vengono assorbiti
 * dal wallpaper cinema (CINEMA_H 440 -> 536) e dall'area eventi
 * (EVT_H 230 -> 326). Il banner resta ancorato al fondo schermo: BANNER_Y
 * 460 -> 556. Le baseline interne del banner sono relative a BANNER_Y in
 * Layout_097c.h, quindi si propagano in automatico se vengono ricalcolate
 * sui valori 122c.
 *
 * Font: identici al 097c per ora (stessi FreeSans*, Picopixel). Potranno
 * essere ridefiniti qui se il layout 122c richiedera' size diverse.
 */

#include <Arduino.h>
#include <stdint.h>
#include <Adafruit_GFX.h>

#include "GxEPD2_SOLUM_122c_960x768/GxEPD2_SOLUM_122c_960x768.h"

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
  using Panel = GxEPD2_SOLUM_122c_960x768;

  // Pin del driver display: stessa Waveshare ESP32 Driver Board, stesso
  // cablaggio. Da rivedere se il pannello 12.2" richiede pin diversi.
  inline constexpr int PIN_CS   = 15;
  inline constexpr int PIN_DC   = 27;
  inline constexpr int PIN_RST  = 26;
  inline constexpr int PIN_BUSY = 25;

  inline constexpr uint8_t ROTATION = 0;
  inline constexpr int16_t SCREEN_W = 960;
  inline constexpr int16_t SCREEN_H = 768;

  // -------------------------------------------------------------------------
  // Font (stessi del 097c per ora). Modificare qui per scegliere font/size
  // diversi senza toccare i moduli applicativi.
  // -------------------------------------------------------------------------
  inline constexpr const GFXfont* FONT_LARGE_BOLD = &FreeSansBold18pt7b;
  inline constexpr const GFXfont* FONT_LARGE      = &FreeSans18pt7b;
  inline constexpr const GFXfont* FONT_BODY       = &FreeSans12pt7b;
  inline constexpr const GFXfont* FONT_SMALL      = &FreeSans9pt7b;
  inline constexpr const GFXfont* FONT_MICRO      = &Picopixel;
  /** Sul pannello 12.2" l'orario dei blocchi meteo (current + forecast)
   *  passa da FONT_LARGE a FONT_BODY: stesso font usato dagli eventi
   *  calendario nella sidebar, per uniformita' visiva. Il blocco meteo
   *  viene shiftato verso il basso (vedi baseline ICON_Y/DESC/TEMP/TIME)
   *  per ridistribuire lo spazio liberato dal font piu' piccolo. */
  inline constexpr const GFXfont* FONT_TIME       = FONT_BODY;

  // -------------------------------------------------------------------------
  // Sidebar: identica al 097c (stessa SCREEN_W).
  // -------------------------------------------------------------------------
  inline constexpr int16_t SIDEBAR_W = 340;
  inline constexpr int16_t SIDEBAR_X = SCREEN_W - SIDEBAR_W;   // 620

  // -------------------------------------------------------------------------
  // Wallpaper cinema: stessa larghezza, altezza +96 (assorbe l'extra
  // verticale). Stride invariato (W/8 = 78). PLANE_SZ e TOTAL_SZ ricalcolati.
  // L'URL deve riflettere la nuova height per ricevere il blob della giusta
  // dimensione dal server cinema.
  // -------------------------------------------------------------------------
  inline constexpr int16_t  CINEMA_W        = 620;
  inline constexpr int16_t  CINEMA_H        = 536;     // 440 + 96
  inline constexpr uint16_t CINEMA_STRIDE   = (CINEMA_W + 7) / 8;             // 78
  inline constexpr uint32_t CINEMA_PLANE_SZ = (uint32_t)CINEMA_STRIDE * CINEMA_H;  // 41808
  inline constexpr uint32_t CINEMA_TOTAL_SZ = CINEMA_PLANE_SZ * 3;            // 125424

  inline constexpr const char* CINEMA_URL =
      "https://cinema-epd.onrender.com/cinema/arduino?width=620&height=536&colors=bwry&dither=floyd";

  // -------------------------------------------------------------------------
  // Banner meteo: ancorato al fondo schermo. BANNER_H invariato (212),
  // BANNER_Y traslato di +96 (460 -> 556).
  // Tutti gli X / W del banner sono invariati: stessa SCREEN_W.
  // -------------------------------------------------------------------------
  inline constexpr int16_t BANNER_Y = 556;
  inline constexpr int16_t BANNER_H = 212;
  inline constexpr int16_t BANNER_W = SCREEN_W;

  inline constexpr int16_t INDOOR_RR_X   = 5;
  inline constexpr int16_t INDOOR_RR_W   = 154;
  inline constexpr int16_t WEATHER_RR_X  = 169;
  inline constexpr int16_t WEATHER_RR_W  = 306;
  inline constexpr int16_t FORECAST_RR_X = 485;
  inline constexpr int16_t FORECAST_RR_W = 470;

  inline constexpr int16_t BLOCK_CURRENT_X = WEATHER_RR_X - 12;
  inline constexpr int16_t BLOCK_CURRENT_W = 200;
  inline constexpr int16_t SUN_COL_OFFSET  = 188;

  inline constexpr int16_t BLOCK_FC_W  = 156;
  inline constexpr int16_t BLOCK_FC0_X = FORECAST_RR_X;
  inline constexpr int16_t BLOCK_FC1_X = BLOCK_FC0_X + BLOCK_FC_W;
  inline constexpr int16_t BLOCK_FC2_X = BLOCK_FC1_X + BLOCK_FC_W;

  /**
   * Baseline del blocco meteo. Sul 122c sono shiftate di +8 px rispetto al
   * 097c (+6 / +118 / +153 / +188) per ridistribuire lo spazio liberato dal
   * passaggio dell'orario a FONT_BODY (FreeSans12pt7b al posto di FreeSans
   * 18pt7b): l'icona scende dal margine superiore "stretto" del fieldset e
   * l'orario (font piu' basso) mantiene comunque ~7 px di margine sopra il
   * bordo inferiore del riquadro arrotondato.
   * Banner 212 px alto, fieldset interno utile ~202 px (INSET_Y=5).
   */
  inline constexpr int16_t ICON_Y        = BANNER_Y + 14;     // 097c: +6
  inline constexpr int16_t DESC_BASELINE = BANNER_Y + 126;    // 097c: +118
  inline constexpr int16_t TEMP_BASELINE = BANNER_Y + 161;    // 097c: +153
  inline constexpr int16_t TIME_BASELINE = BANNER_Y + 196;    // 097c: +188

  inline constexpr int16_t INDOOR_ROW1_BASELINE = BANNER_Y + 46;
  inline constexpr int16_t INDOOR_ROW2_BASELINE = BANNER_Y + 92;
  inline constexpr int16_t INDOOR_ROW3_BASELINE = BANNER_Y + 138;
  inline constexpr int16_t INDOOR_ROW4_BASELINE = BANNER_Y + 184;

  /** Baseline dedicate sun: riga sunset avvicinata a sunrise (interlinea 24). */
  inline constexpr int16_t SUN_ROW1_BASELINE = BANNER_Y + 46;
  inline constexpr int16_t SUN_ROW2_BASELINE = BANNER_Y + 70;

  /** Baseline slider temp-range traslata verso l'alto (-30 px rispetto a
   *  INDOOR_ROW3_BASELINE) per ridurre il gap col blocco alba/tramonto. */
  inline constexpr int16_t SUN_ROW3_BASELINE = BANNER_Y + 108;

  /** Centro orizzontale della sub-col sun (118 px di larghezza). */
  inline constexpr int16_t SUN_COL_CENTER_X =
      WEATHER_RR_X + SUN_COL_OFFSET + (WEATHER_RR_W - SUN_COL_OFFSET) / 2;

  inline constexpr int16_t INDOOR_ICON_GAP    = 6;
  inline constexpr int16_t INDOOR_COL1_OFFSET = 10;

  // -------------------------------------------------------------------------
  // Riquadro mese: identico al 097c (resta in cima alla sidebar).
  // -------------------------------------------------------------------------
  inline constexpr int16_t CAL_X        = SIDEBAR_X + 10;
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
  // Area eventi: stesso EVT_Y (sotto al mese), EVT_H esteso di +96 px per
  // arrivare a 10 px dal nuovo BANNER_Y.
  //   EVT_Y + EVT_H + 10 = 220 + 326 + 10 = 556 = BANNER_Y. OK.
  // -------------------------------------------------------------------------
  inline constexpr int16_t EVT_X        = CAL_X;
  inline constexpr int16_t EVT_Y        = CAL_Y + CAL_H + 10;     // 220
  inline constexpr int16_t EVT_W        = CAL_W;
  inline constexpr int16_t EVT_H        = 326;                    // 230 + 96
  inline constexpr int16_t EVT_SEP_PAD  = 15;
  inline constexpr int16_t EVT_PAD      = 8;
  inline constexpr int16_t EVT_PAD_LEFT = 3;

  // -------------------------------------------------------------------------
  // Icone: stesse del 097c (le bitmap in icons.h sono dimensionate cosi').
  // -------------------------------------------------------------------------
  inline constexpr int16_t ICON_SIZE        = 88;
  inline constexpr int16_t INDOOR_ICON_SIZE = 20;

  // -------------------------------------------------------------------------
  // Banner fieldset / titoli (estetica condivisa).
  // -------------------------------------------------------------------------
  inline constexpr int16_t BANNER_RR_INSET_Y       = 5;
  inline constexpr int16_t BANNER_RR_RADIUS       = 18;
  inline constexpr int16_t BANNER_TITLE_LEFT_OFFSET = 14;

  // -------------------------------------------------------------------------
  // @widget slider-temp-range
  // Barra temp-range: identica al 097c (W/H del buffer non dipendono dal
  // pannello, sono dimensioni del widget).
  // -------------------------------------------------------------------------
  inline constexpr int16_t TRB_W             = 112;
  inline constexpr int16_t TRB_H             = 14;
  inline constexpr int16_t TRB_BYTES_PER_ROW = TRB_W / 8;
  inline constexpr int16_t TRB_CELL_Y_OFFSET = -12;

  // -------------------------------------------------------------------------
  // @widget storico-temperature
  // Mini-chart temperatura: identico al 097c (baseline interne del banner
  // sono uguali, offset relativi a BANNER_Y).
  // -------------------------------------------------------------------------
  inline constexpr int16_t TC_W            = TRB_W;    // 112
  inline constexpr int16_t TC_H            = 56;
  inline constexpr int16_t TC_TOP_OFFSET   = 116;      // = (SUN_ROW3_BASELINE - BANNER_Y) + 8
  inline constexpr int16_t TC_LABEL_W      = 14;
  inline constexpr int16_t TC_AXIS_PAD_BOT = 8;
  inline constexpr int16_t TC_TICK_H       = 2;
  inline constexpr int16_t TC_DOT_R        = 3;
}

#endif

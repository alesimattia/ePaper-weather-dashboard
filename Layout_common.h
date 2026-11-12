#ifndef LAYOUT_COMMON_H
#define LAYOUT_COMMON_H

/**
 * Geometria della dashboard comune ai pannelli: token di spaziatura, font e
 * coordinate di ogni zona, tutte derivate da SCREEN_W/SCREEN_H e dalle due
 * manopole del pannello (CINEMA_H e MAIL_ROWS_PER_COL). Va incluso in coda a
 * un Layout_<pannello>.h, che definisce prima quei simboli nel namespace Layout.
 *
 * Convenzione: NwxMh = N px in larghezza (X) e M px in altezza (Y), costanti
 * _W larghezze e _H altezze, coordinate (x, y) con origine in alto a sinistra
 * dopo setRotation(ROTATION).
 */

#ifndef LAYOUT_PANEL_SELECTED
  #error "Layout_common.h va incluso da un Layout_<pannello>.h, non direttamente"
#endif

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

namespace Layout
{
  // Font per ruolo: i moduli non nominano mai un font GFX.
  /** Temperature dei blocchi meteo. */
  inline constexpr const GFXfont* FONT_LARGE_BOLD = &FreeSansBold18pt7b;
  /** Valori del riquadro Interno. */
  inline constexpr const GFXfont* FONT_LARGE      = &FreeSans18pt7b;
  /** Testo standard: orari, eventi, giorni del mese, etichette meteo. */
  inline constexpr const GFXfont* FONT_BODY       = &FreeSans12pt7b;
  /** Unità, pedici, oggetto delle mail, etichette del grafico, badge Tuya. */
  inline constexpr const GFXfont* FONT_SMALL      = &FreeSans9pt7b;
  /** Titoli dei riquadri, seduti sul bordo superiore. */
  inline constexpr const GFXfont* FONT_TITLE      = &FreeSansBold12pt7b;

  // Spaziature comuni a tutte le zone.
  /** Distanza delle zone dal bordo dello schermo e fra zone affiancate. */
  inline constexpr int16_t MARGIN      = 10;
  inline constexpr int16_t GUTTER      = 10;
  /** Distanza fra zone impilate, perchè il titolo di un riquadro sporge di circa 9 px sopra il bordo. */
  inline constexpr int16_t STACK_GAP   = 16;
  inline constexpr int16_t BOX_RADIUS  = 12;
  /** Distanza del titolo dal bordo sinistro del riquadro. */
  inline constexpr int16_t TITLE_INSET = 16;
  /** Margine orizzontale fra il bordo di un riquadro e il suo contenuto. */
  inline constexpr int16_t BOX_PAD     = 12;
  /** Fascia sotto il bordo superiore che il titolo occupa, e margine sul fondo. */
  inline constexpr int16_t BOX_TITLE_BAND = 14;
  inline constexpr int16_t BOX_BOTTOM_PAD = 6;
  /** Bordo superiore dei riquadri in cima allo schermo: il titolo resta a 5 px dal bordo. */
  inline constexpr int16_t TOP_BOX_Y   = 14;

  // Lato delle icone in pixel.
  /** Spazio riservato all'icona meteo; le bitmap stanno in icons_weather.h. */
  inline constexpr int16_t ICON_SIZE       = 88;
  /** Icone piccole di icons.h (Interno, alba e tramonto, busta). */
  inline constexpr int16_t SMALL_ICON_SIZE = 20;
  inline constexpr int16_t SMALL_ICON_GAP  = 6;

  // Colonne: wallpaper e Posta a sinistra, scheda calendario a destra.
  inline constexpr int16_t SIDEBAR_W = 320;
  inline constexpr int16_t SIDEBAR_X = SCREEN_W - MARGIN - SIDEBAR_W;
  /** Bordo destro della colonna sinistra, che ne fissa la larghezza utile. */
  inline constexpr int16_t LEFT_COLUMN_END = SIDEBAR_X - GUTTER;

  // Banner meteo, ancorato al fondo dello schermo.
  inline constexpr int16_t BANNER_H = 196;
  inline constexpr int16_t BANNER_Y = SCREEN_H - MARGIN - BANNER_H;

  inline constexpr int16_t INDOOR_X   = MARGIN;
  inline constexpr int16_t INDOOR_W   = 200;
  inline constexpr int16_t WEATHER_X  = INDOOR_X + INDOOR_W + GUTTER;
  inline constexpr int16_t WEATHER_W  = 280;
  inline constexpr int16_t FORECAST_X = WEATHER_X + WEATHER_W + GUTTER;
  inline constexpr int16_t FORECAST_W = SCREEN_W - MARGIN - FORECAST_X;

  /** Colonna alba/tramonto a destra del blocco meteo corrente, dentro Meteo. */
  inline constexpr int16_t SUN_COL_W        = 118;
  inline constexpr int16_t SUN_COL_CENTER_X = WEATHER_X + WEATHER_W - SUN_COL_W / 2 - 4;
  inline constexpr int16_t CURRENT_BLOCK_X  = WEATHER_X;
  inline constexpr int16_t CURRENT_BLOCK_W  = WEATHER_W - SUN_COL_W;
  inline constexpr int16_t FORECAST_BLOCK_W = FORECAST_W / 3;
  /** Margine laterale dell'etichetta di un blocco meteo dai suoi bordi. */
  inline constexpr int16_t WX_DESC_SIDE_PAD = 4;

  /** Righe di un blocco meteo: orario, icona, temperatura, etichetta. */
  inline constexpr int16_t WX_TIME_BASELINE = BANNER_Y + 32;
  inline constexpr int16_t WX_ICON_Y        = BANNER_Y + 38;
  inline constexpr int16_t WX_TEMP_BASELINE = BANNER_Y + 158;
  inline constexpr int16_t WX_DESC_BASELINE = BANNER_Y + 184;

  /** Righe del riquadro Interno, a passo 44. */
  inline constexpr int16_t INDOOR_ROW1_BASELINE = BANNER_Y + 42;
  inline constexpr int16_t INDOOR_ROW2_BASELINE = BANNER_Y + 86;
  inline constexpr int16_t INDOOR_ROW3_BASELINE = BANNER_Y + 130;
  inline constexpr int16_t INDOOR_ROW4_BASELINE = BANNER_Y + 174;
  inline constexpr int16_t INDOOR_ICON_X        = INDOOR_X + BOX_PAD;
  /** Distanza fra un valore e la sua unità, scritta in FONT_SMALL. */
  inline constexpr int16_t INDOOR_UNIT_GAP      = 3;
  /** Abbassamento del pedice di accuratezza IAQ rispetto alla baseline. */
  inline constexpr int16_t INDOOR_SUBSCRIPT_DROP = 5;

  /** Colonna sole: alba, tramonto, barra mattino/sera e grafico. */
  inline constexpr int16_t SUN_ROW1_BASELINE = BANNER_Y + 32;
  inline constexpr int16_t SUN_ROW2_BASELINE = BANNER_Y + 58;
  inline constexpr int16_t SUN_ROW3_BASELINE = BANNER_Y + 96;

  // @widget slider-temp-range: la cella della barra parte da baseline + offset.
  inline constexpr int16_t TRB_W             = 112;
  inline constexpr int16_t TRB_CELL_Y_OFFSET = -12;

  // @widget storico-temperature: il grafico sta sotto la barra ed è largo come lei.
  inline constexpr int16_t TC_W            = TRB_W;
  inline constexpr int16_t TC_H            = 70;
  inline constexpr int16_t TC_TOP_Y        = BANNER_Y + 108;
  /** Colonna delle etichette min/max: "-12" in FONT_SMALL misura 25 px. */
  inline constexpr int16_t TC_LABEL_W      = 28;
  /** Spazio sotto l'asse per le etichette delle ore. */
  inline constexpr int16_t TC_AXIS_PAD_BOT = 16;
  inline constexpr int16_t TC_TICK_H       = 2;
  inline constexpr int16_t TC_DOT_R        = 3;
  /** Tratteggio della previsione, in passi della curva (1 px in x ciascuno): disegnati e saltati. */
  inline constexpr int16_t TC_DASH_ON      = 4;
  inline constexpr int16_t TC_DASH_OFF     = 3;

  // Scheda della sidebar: mese, griglia, filetto, eventi.
  inline constexpr int16_t CAL_X  = SIDEBAR_X;
  inline constexpr int16_t CAL_Y  = TOP_BOX_Y;
  inline constexpr int16_t CAL_W  = SIDEBAR_W;
  inline constexpr int16_t CAL_H  = BANNER_Y - STACK_GAP - CAL_Y;
  /** Margine orizzontale della griglia: 308 px divisi in 7 colonne da 44. */
  inline constexpr int16_t CAL_PAD          = 6;
  /** Riga dei giorni della settimana, con la baseline misurata dalla sua cima. */
  inline constexpr int16_t CAL_HDR_H        = 26;
  inline constexpr int16_t CAL_HDR_BASELINE = 20;
  inline constexpr int16_t CAL_GRID_TOP_PAD = 3;
  inline constexpr int16_t CAL_CELL_H       = 24;
  /** Righe della griglia: un mese di 31 giorni che inizia di sabato occupa 6 settimane. */
  inline constexpr int16_t CAL_ROWS         = 6;
  inline constexpr int16_t CAL_CELL_R       = 4;
  inline constexpr int16_t CAL_GRID_Y       = CAL_Y + BOX_TITLE_BAND;
  inline constexpr int16_t CAL_DIVIDER_Y    = CAL_GRID_Y + CAL_HDR_H + CAL_GRID_TOP_PAD
                                            + CAL_ROWS * CAL_CELL_H + 6;

  inline constexpr int16_t EVT_X = CAL_X + BOX_PAD;
  inline constexpr int16_t EVT_W = CAL_W - 2 * BOX_PAD;
  inline constexpr int16_t EVT_Y = CAL_DIVIDER_Y + 6;
  inline constexpr int16_t EVT_H = CAL_Y + CAL_H - BOX_BOTTOM_PAD - EVT_Y;
  /** Baseline dentro una riga evento: orario dall'alto, data dal fondo, titolo dal centro. */
  inline constexpr int16_t EVT_TIME_BASELINE        = 19;
  inline constexpr int16_t EVT_DATE_FROM_BOTTOM     = 7;
  inline constexpr int16_t EVT_TITLE_FROM_MIDDLE    = 6;
  /** Spazio fra il titolo troncato e la colonna orario/data. */
  inline constexpr int16_t EVT_RIGHT_GAP            = 10;

  // Wallpaper cinema, dentro i margini in cima alla colonna sinistra.
  inline constexpr int16_t  CINEMA_X        = MARGIN;
  inline constexpr int16_t  CINEMA_Y        = MARGIN;
  inline constexpr int16_t  CINEMA_W        = LEFT_COLUMN_END - MARGIN;
  inline constexpr uint16_t CINEMA_STRIDE   = (CINEMA_W + 7) / 8;
  inline constexpr uint32_t CINEMA_PLANE_SZ = (uint32_t)CINEMA_STRIDE * CINEMA_H;
  /** Piani che i due pannelli rendono, black e accent: nessuno ha il giallo. */
  inline constexpr uint8_t  CINEMA_PLANES   = 2;
  inline constexpr uint32_t CINEMA_TOTAL_SZ = CINEMA_PLANE_SZ * CINEMA_PLANES;

  // Posta, sotto il wallpaper: due colonne lette dall'alto in basso.
  inline constexpr int16_t MAIL_X      = MARGIN;
  inline constexpr int16_t MAIL_Y      = CINEMA_Y + CINEMA_H + STACK_GAP;
  inline constexpr int16_t MAIL_W      = LEFT_COLUMN_END - MARGIN;
  inline constexpr int16_t MAIL_H      = BANNER_Y - STACK_GAP - MAIL_Y;
  inline constexpr int16_t MAIL_COLS   = 2;
  inline constexpr int16_t MAIL_MAX    = MAIL_COLS * MAIL_ROWS_PER_COL;
  inline constexpr int16_t MAIL_COL_W  = MAIL_W / MAIL_COLS;
  inline constexpr int16_t MAIL_BODY_Y = MAIL_Y + BOX_TITLE_BAND;
  inline constexpr int16_t MAIL_ROW_H  =
      (MAIL_H - BOX_TITLE_BAND - BOX_BOTTOM_PAD) / MAIL_ROWS_PER_COL;
  /** Altezza delle due righe di testo di una mail, centrate nella riga; la
   *  prima baseline cade una cap height di FONT_BODY (17 px) sotto la cima. */
  inline constexpr int16_t MAIL_TEXT_H         = 41;
  inline constexpr int16_t MAIL_LINE1_BASELINE = (MAIL_ROW_H - MAIL_TEXT_H) / 2 + 17;
  inline constexpr int16_t MAIL_LINE_GAP       = 20;
  /** Spazio fra il testo a sinistra e la colonna orario/data. */
  inline constexpr int16_t MAIL_RIGHT_GAP      = 10;

  // Le manopole del pannello possono rompere queste relazioni: la build si
  // ferma qui invece di disegnare righe sovrapposte o aree negative.
  static_assert(MAIL_ROW_H >= MAIL_TEXT_H,
                "Layout: le righe della Posta sono troppo basse per mittente e oggetto");
  static_assert(CINEMA_H > 0 && MAIL_H > 0 && EVT_H > 0 && FORECAST_W > 0,
                "Layout: una zona ha altezza o larghezza non positiva");
  static_assert(CURRENT_BLOCK_W >= ICON_SIZE && FORECAST_BLOCK_W >= ICON_SIZE,
                "Layout: un blocco meteo è più stretto della sua icona");
}

#endif

#ifndef LAYOUT_097C_H
#define LAYOUT_097C_H

/**
 * Layout per pannello SOLUM ESL 9.7" (controller SSD1677, tre colori misurati:
 * bianco, nero e rosso, nessun quarto colore). Vetro nativo 672w x 960h
 * portrait, che il driver indirizza come 960w x 672h: con setRotation(0) è
 * la geometria visibile.
 *
 * Qui stanno solo il pannello (driver, pin, page, geometria dello schermo) e
 * le due manopole della dashboard, l'altezza del wallpaper e le righe della
 * Posta; tutte le coordinate le deriva Layout_common.h, incluso in coda.
 *
 * Lo sketch .ino seleziona quale Layout includere via Layout.h: per il
 * pannello 12.2" si commenta DISPLAY_VARIANT_097C e si scommenta
 * DISPLAY_VARIANT_122C.
 */

#include <Arduino.h>
#include <stdint.h>
#include <Adafruit_GFX.h>

/**
 * Selezione del driver: l'header ombrello della libreria include il driver
 * del pannello scelto e ne espone il nome come GxEPD2_SOLUM_DRIVER_CLASS,
 * così questo file non nomina la classe concreta. Aggiungere un pannello
 * alla libreria non cambia niente qui oltre al define.
 */
#define SOLUM_PANEL_097C
#include "GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM.h"

namespace Layout
{
  using Panel = GxEPD2_SOLUM_DRIVER_CLASS;

  // Pin del driver display (CS, DC, RST, BUSY) sul costruttore Panel(...).
  // Il bus HSPI (SCK/MISO/MOSI) è condiviso fra le varianti (board-level)
  // e resta nello sketch .ino.
  inline constexpr int PIN_CS   = 15;
  inline constexpr int PIN_DC   = 27;
  inline constexpr int PIN_RST  = 26;
  inline constexpr int PIN_BUSY = 25;

  /** BUSY che risveglia l'MCU dal light sleep durante l'attesa del refresh,
   *  vedi waitBusyAsleep() nel .ino. Simbolo comune ai due layout perchè i
   *  nomi dei pin, qui e nel 122c, non lo sono. */
  inline constexpr int BUSY_WAKE_PIN = PIN_BUSY;

  /**
   * Altezza della page del template GxEPD2_3C. Sta qui e non nel .ino perchè
   * dipende dal pannello: è il compromesso fra RAM occupata dai due buffer
   * di page e numero di iterazioni del loop paged. Non usa la
   * SOLUM_MAX_HEIGHT() della libreria, che su un pannello 960 px di larghezza
   * spenderebbe ~65 KB: qui la RAM serve anche al resto del firmware.
   */
  inline constexpr int16_t PAGE_HEIGHT = Panel::HEIGHT / 8;

  /**
   * Costruisce il driver del pannello. Il pinout passa dalla struct uniforme
   * della libreria, quindi la firma è identica per tutti i driver e il .ino
   * non cambia quando cambia il pannello. Ordine dei campi: cs, dc, rst,
   * busy, cs2, busy2, sck, miso, mosi; quelli non passati restano -1.
   */
  inline Panel makePanel()
  {
    return Panel(GxEPD2_SOLUM_Pins{ PIN_CS, PIN_DC, PIN_RST, PIN_BUSY });
  }

  /**
   * Geometria visibile dopo setRotation(ROTATION), presa dal driver con gli
   * stessi WIDTH_VISIBLE e HEIGHT che GxEPD2_3C passa ad Adafruit_GFX,
   * scambiati sulle rotazioni dispari come fa setRotation().
   */
  inline constexpr uint8_t ROTATION = 0;
  inline constexpr int16_t SCREEN_W = (ROTATION % 2 == 0) ? Panel::WIDTH_VISIBLE : Panel::HEIGHT;
  inline constexpr int16_t SCREEN_H = (ROTATION % 2 == 0) ? Panel::HEIGHT : Panel::WIDTH_VISIBLE;

  /** Altezza del wallpaper cinema: con i margini e la Posta a due righe
   *  chiude la colonna sinistra sopra il banner. */
  inline constexpr int16_t CINEMA_H          = 290;
  /** Righe di mail per colonna: due colonne da due, quattro mail. */
  inline constexpr int16_t MAIL_ROWS_PER_COL = 2;
}

#define LAYOUT_PANEL_SELECTED
#include "Layout_common.h"

#endif

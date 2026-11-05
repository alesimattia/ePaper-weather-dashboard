#ifndef LAYOUT_122C_H
#define LAYOUT_122C_H

/**
 * Layout per pannello SOLUM Newton PRO 12.2" (960w x 768h, bianco/nero/rosso
 * sul vetro). Il driver pilota due controller SSD16xx da 960 x 384, uno per
 * coda FFC: la seconda coda non risponde, quindi il pannello stampa una sola
 * banda, vedi GxEPD2_SOLUM_ESL/README_122c.md.
 *
 * Qui stanno solo il pannello (driver, pin, page, geometria dello schermo) e
 * le due manopole della dashboard; tutte le coordinate le deriva
 * Layout_common.h, incluso in coda. Rispetto al 9.7" la larghezza è la stessa
 * e i 96 px di altezza in più vanno al wallpaper (+35), alla terza riga della
 * Posta e a due righe di eventi in più nella sidebar.
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
#define SOLUM_PANEL_122C
#include "GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM.h"

namespace Layout
{
  using Panel = GxEPD2_SOLUM_DRIVER_CLASS;

  /**
   * Pin del driver display. Il pannello ha due FFC, uno per controller:
   * il master è quello cablato al connettore interno della Waveshare ESP32
   * Driver Board, lo slave passa da un breakout esterno con CS e BUSY
   * propri e tutti gli altri segnali in parallelo. Schema e procedura di
   * verifica in GxEPD2_SOLUM_ESL/docs/122c/connessioni.html.
   *
   * CS_S sta su GPIO32 perchè la Waveshare V3 non porta fuori GPIO33: il 32 è
   * l'unico pin d'uscita sull'header che lo schematico non usa altrove.
   */
  inline constexpr int PIN_CS_M   = 15;
  inline constexpr int PIN_CS_S   = 32;
  inline constexpr int PIN_DC     = 27;
  inline constexpr int PIN_RST    = 26;
  inline constexpr int PIN_BUSY_M = 25;
  inline constexpr int PIN_BUSY_S = 35;

  /** BUSY che risveglia l'MCU dal light sleep durante l'attesa del refresh,
   *  vedi waitBusyAsleep() nel .ino: è quello del master, che il driver
   *  controlla per primo nella sua attesa. */
  inline constexpr int BUSY_WAKE_PIN = PIN_BUSY_M;

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
   *
   * I pin del bus servono a questo driver, che apre l'SPI da sè dentro
   * init(): sono gli stessi HSPI della board (13/12/14) su cui lo sketch fa
   * hspi.begin(), e il selectSPI() dello sketch resta valido perchè il driver
   * usa _pSPIx della base.
   */
  inline Panel makePanel()
  {
    return Panel(GxEPD2_SOLUM_Pins{ PIN_CS_M, PIN_DC, PIN_RST, PIN_BUSY_M,
                                    PIN_CS_S, PIN_BUSY_S,
                                    13, 12, 14 });
  }

  /**
   * Geometria visibile dopo setRotation(ROTATION), presa dal driver con gli
   * stessi WIDTH_VISIBLE e HEIGHT che GxEPD2_3C passa ad Adafruit_GFX,
   * scambiati sulle rotazioni dispari come fa setRotation().
   */
  inline constexpr uint8_t ROTATION = 0;
  inline constexpr int16_t SCREEN_W = (ROTATION % 2 == 0) ? Panel::WIDTH_VISIBLE : Panel::HEIGHT;
  inline constexpr int16_t SCREEN_H = (ROTATION % 2 == 0) ? Panel::HEIGHT : Panel::WIDTH_VISIBLE;

  /** Altezza del wallpaper cinema: con i margini e la Posta a tre righe
   *  chiude la colonna sinistra sopra il banner. */
  inline constexpr int16_t CINEMA_H          = 325;
  /** Righe di mail per colonna: due colonne da tre, sei mail. */
  inline constexpr int16_t MAIL_ROWS_PER_COL = 3;
}

#define LAYOUT_PANEL_SELECTED
#include "Layout_common.h"

#endif

#ifndef LAYOUT_122C_H
#define LAYOUT_122C_H

/**
 * Layout per pannello SOLUM 12.2" EL122R2CRN (960w x 768h, bianco/nero/rosso
 * sul vetro). Il driver pilota due controller da 960 x 384, uno per coda FFC:
 * la coda lunga dipinge la banda bassa, la corta la banda alta quando è
 * cablata secondo GxEPD2_SOLUM_ESL/docs/122c/cable_linking.html.
 *
 * Qui sta solo il pannello (driver, pin, page, geometria dello schermo);
 * tutte le coordinate le deriva Layout_common.h, incluso in coda. Rispetto
 * al 9.7" la larghezza è la stessa e l'altezza in più va al wallpaper e a
 * righe di eventi in più nella sidebar.
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
   * Topologia del secondo controller, quello della coda corta, che si decide
   * con la fase 0 di GxEPD2_SOLUM_ESL/docs/122c/cable_linking.html. False vuol
   * dire chip select e BUSY propri sulla breakout; true vuol dire cascade, con
   * lo slave sullo stesso chip select del master e indirizzato a opcode | 0x80.
   */
  inline constexpr bool SECOND_CONTROLLER_CASCADE = false;

  /**
   * Pin del driver display. Il master è la coda lunga nel connettore della
   * Waveshare ESP32 Driver Board; lo slave passa da una breakout esterna con
   * gli altri segnali in parallelo. In cascade CS e BUSY dello slave non
   * esistono: il suo CS è il 15 del master e il suo BUSY resta aperto.
   *
   * CS_S sta su GPIO32, pin d'uscita che lo schematico non usa altrove (il 33,
   * che la netlist mette su J3-8, è un'alternativa da confermare sulla
   * serigrafia). BUSY_S sta su GPIO22 perchè ha il pull-down interno, che il
   * driver attiva: un BUSY scollegato o lasciato aperto si legge libero invece
   * di mandare ogni attesa al timeout, come farebbe un input-only 34..39.
   */
  inline constexpr int PIN_CS_M   = 15;
  inline constexpr int PIN_CS_S   = SECOND_CONTROLLER_CASCADE ? -1 : 32;
  inline constexpr int PIN_DC     = 27;
  inline constexpr int PIN_RST    = 26;
  inline constexpr int PIN_BUSY_M = 25;
  inline constexpr int PIN_BUSY_S = SECOND_CONTROLLER_CASCADE ? -1 : 22;

  /** BUSY che risveglia l'MCU dal light sleep durante l'attesa del refresh,
   *  vedi waitBusyAsleep() nel .ino: è quello del master, che il driver
   *  controlla per primo nella sua attesa. */
  inline constexpr int BUSY_WAKE_PIN = PIN_BUSY_M;

  /** Clock SPI: 4 MHz perchè SCK e MOSI arrivano alla breakout con fili
   *  volanti, che sono uno stub anche per il bus del master; un frame costa
   *  circa 0,4 s in più che a 10 MHz su un refresh di circa 18 s. Letterale
   *  decimale nudo perchè tools/layout_preview.py valuta queste costanti. */
  inline constexpr uint32_t SPI_CLOCK_HZ = 4000000;

  /** Impulso di reset in ms: 10 è il valore con cui il 12.2" è stato provato
   *  dallo sketch dual_panel_finder ed è quello del firmware di fabbrica R2. */
  inline constexpr uint16_t RESET_DURATION_MS = 10;

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
   * usa _pSPIx della base. Il modo di indirizzamento si imposta qui, prima di
   * init(), e GxEPD2_3C lo eredita con la copia del driver.
   */
  inline Panel makePanel()
  {
    Panel panel(GxEPD2_SOLUM_Pins{ PIN_CS_M, PIN_DC, PIN_RST, PIN_BUSY_M,
                                   PIN_CS_S, PIN_BUSY_S,
                                   13, 12, 14 });
    if (SECOND_CONTROLLER_CASCADE) panel.setAddressingMode(Panel::ADDRESSING_CASCADE);
    return panel;
  }

  /**
   * Geometria visibile dopo setRotation(ROTATION), presa dal driver con gli
   * stessi WIDTH_VISIBLE e HEIGHT che GxEPD2_3C passa ad Adafruit_GFX,
   * scambiati sulle rotazioni dispari come fa setRotation().
   *
   * Con la sola coda lunga la banda bassa del layout deve comparire dritta
   * sulla metà del vetro dal lato della coda. Capovolta del tutto: ROTATION 2.
   * Ribaltata solo in Y: setMasterMirror(false, true) e setSlaveMirror(true,
   * false) in makePanel(); solo in X: master (true, false), slave (false, true).
   */
  inline constexpr uint8_t ROTATION = 0;
  inline constexpr int16_t SCREEN_W = (ROTATION % 2 == 0) ? Panel::WIDTH_VISIBLE : Panel::HEIGHT;
  inline constexpr int16_t SCREEN_H = (ROTATION % 2 == 0) ? Panel::HEIGHT : Panel::WIDTH_VISIBLE;
}

#define LAYOUT_PANEL_SELECTED
#include "Layout_common.h"

#endif

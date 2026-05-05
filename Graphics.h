#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <Arduino.h>
#include <stdint.h>

#include <GxEPD2_3C.h>
#include "GxEPD2_SOLUM_097c_960x672/GxEPD2_SOLUM_097c_960x672.h"

/**
 * Utility di disegno condivise fra i moduli (Weather, Calendar).
 * Header-only, non tiene stato: tutte le funzioni scrivono direttamente
 * sul display globale dichiarato nello sketch .ino.
 *
 * @since 22/04/26
 */

// ---------------------------------------------------------------------------
// Istanza del display (definita nello sketch .ino).
// ---------------------------------------------------------------------------
extern GxEPD2_3C<GxEPD2_SOLUM_097c_960x672, GxEPD2_SOLUM_097c_960x672::HEIGHT / 8> display;

namespace Graphics
{
  /**
   * Disegna un rettangolo a bordi arrotondati con un titolo "stile fieldset"
   * incastrato nel bordo superiore: il bordo viene interrotto da un rettangolo
   * di sfondo sotto il testo, dando l'impressione che il titolo sia seduto
   * sulla linea. Il titolo è sempre nero; il rettangolo è nero anch'esso.
   *
   * Precondizioni: il chiamante deve aver impostato il font prima di chiamare
   * questa funzione (viene usato per misurare e disegnare il titolo).
   *
   * @param x          x (px) dell'angolo superiore sinistro del rettangolo.
   * @param y          y (px) dell'angolo superiore sinistro del rettangolo.
   * @param w          larghezza (px) del rettangolo.
   * @param h          altezza (px) del rettangolo.
   * @param radius     raggio (px) degli angoli arrotondati.
   * @param title      testo del titolo (NULL o stringa vuota = nessun titolo).
   * @param titleLeftOffset offset orizzontale del titolo dal bordo sinistro
   *                        del rettangolo (prima del gap); default ~ radius+6.
   * @param bgColor    colore di sfondo con cui "cancellare" il bordo sotto il
   *                   testo (tipicamente GxEPD_WHITE).
   *
   * @since 22/04/26
   */
  inline void drawFieldsetRect(int16_t x, int16_t y, int16_t w, int16_t h,
                               int16_t radius, const char* title,
                               int16_t titleLeftOffset, uint16_t bgColor)
  {
    display.drawRoundRect(x, y, w, h, radius, GxEPD_BLACK);

    if (!title || !*title) return;

    // Misura il testo con il font corrente impostato dal chiamante.
    int16_t  x1, y1;
    uint16_t tw, th;
    display.getTextBounds(title, 0, 0, &x1, &y1, &tw, &th);

    /**
     * Copri una striscia di sfondo dietro il testo per "spezzare" il bordo
     * superiore del rettangolo. Alta 3 px centrata sulla linea y (il bordo
     * è di 1 px quindi 3 px danno margine sicuro su entrambi i lati).
     */
    const int16_t PAD_H = 3;
    const int16_t stripX = x + titleLeftOffset;
    display.fillRect(stripX - 2, y - 1, (int16_t)tw + 4, PAD_H, bgColor);

    // Baseline del titolo: centrata verticalmente sul bordo superiore.
    // La baseline sta al top del rettangolo, con ~metà ascender sopra e
    // metà sotto (th/2 dal top del testo).
    const int16_t baselineY = y + (int16_t)th / 2;
    display.setCursor(stripX - x1, baselineY);
    display.setTextColor(GxEPD_BLACK);
    display.print(title);
  }
}

#endif

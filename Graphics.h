#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <Arduino.h>
#include <stdint.h>

#include <GxEPD2_3C.h>
#include "Layout.h"

/**
 * Utility di disegno condivise fra i moduli (Weather, Calendar).
 * Header-only, non tiene stato: le funzioni di disegno scrivono direttamente
 * sul display globale dichiarato nello sketch .ino.
 */

// ---------------------------------------------------------------------------
// Istanza del display (definita nello sketch .ino). Il tipo del pannello
// (Layout::Panel) viene scelto dal Layout dispatcher in base al
// DISPLAY_VARIANT_* definito nello sketch.
// ---------------------------------------------------------------------------
extern GxEPD2_3C<Layout::Panel, Layout::PAGE_HEIGHT> display;

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

  namespace detail
  {
    /** Resa ASCII delle lettere Latin-1 da U+00C0 a U+00FF: le vocali con
     *  accento grave o acuto diventano vocale più apostrofo, le altre la
     *  lettera base. Nessuna resa supera i due caratteri, cioè i byte UTF-8 che
     *  sostituisce. */
    inline constexpr const char* LATIN1_LETTERS[64] = {
      "A'", "A'", "A",  "A",  "A",  "A",  "AE", "C",
      "E'", "E'", "E",  "E",  "I'", "I'", "I",  "I",
      "D",  "N",  "O'", "O'", "O",  "O",  "O",  "x",
      "O",  "U'", "U'", "U",  "U",  "Y",  "",   "ss",
      "a'", "a'", "a",  "a",  "a",  "a",  "ae", "c",
      "e'", "e'", "e",  "e",  "i'", "i'", "i",  "i",
      "d",  "n",  "o'", "o'", "o",  "o",  "o",  "",
      "o",  "u'", "u'", "u",  "u",  "y",  "",   "y",
    };

    /** Resa ASCII di un carattere UTF-8 a due o tre byte, nullptr se il
     *  carattere va scartato. */
    inline const char* asciiFor(uint8_t lead, uint8_t b1, uint8_t b2)
    {
      if (lead == 0xC3) return LATIN1_LETTERS[b1 - 0x80];
      if (lead == 0xC2)
      {
        if (b1 == 0xA0) return " ";                  // spazio non separabile
        if (b1 == 0xAB || b1 == 0xBB) return "\"";   // virgolette angolari
        if (b1 == 0xB4) return "'";                  // accento acuto isolato
        return nullptr;
      }
      if (lead == 0xE2 && b1 == 0x80)
      {
        if (b2 == 0x98 || b2 == 0x99) return "'";    // apici tipografici
        if (b2 == 0x9C || b2 == 0x9D) return "\"";   // virgolette tipografiche
        if (b2 == 0x93 || b2 == 0x94) return "-";    // trattini medio e lungo
        if (b2 == 0xA6) return "...";                // puntini di sospensione
        return nullptr;
      }
      if (lead == 0xE2 && b1 == 0x82 && b2 == 0xAC) return "EUR";
      return nullptr;
    }
  }

  /**
   * Riduce sul posto una stringa UTF-8 ai soli caratteri che i font GFX a 7 bit
   * sanno disegnare: Adafruit_GFX scarta in silenzio ogni byte fuori da
   * 0x20..0x7E, quindi "caffè" uscirebbe "caff". Il risultato non è mai più
   * lungo dell'originale, e una sequenza troncata dal buffer viene scartata.
   * Usato da Weather.h, Calendar.h e Mail.h subito dopo la copia dei testi
   * ricevuti dalla rete.
   */
  inline void foldToFontCharset(char* text)
  {
    if (text == nullptr) return;
    const uint8_t* in = (const uint8_t*)text;
    char* out = text;
    while (*in)
    {
      const uint8_t c = *in;
      if (c < 0x80)
      {
        *out++ = (char)c;
        ++in;
        continue;
      }
      size_t len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
      size_t avail = 1;
      while (avail < len && (in[avail] & 0xC0) == 0x80) ++avail;
      if (avail < len)
      {
        // Byte di continuazione isolato o sequenza interrotta: si scarta.
        in += avail;
        continue;
      }
      const char* repl = (len == 2 || len == 3)
                         ? detail::asciiFor(c, in[1], len == 3 ? in[2] : 0)
                         : nullptr;
      if (repl != nullptr)
        while (*repl) *out++ = *repl++;
      in += len;
    }
    *out = '\0';
  }
}

#endif

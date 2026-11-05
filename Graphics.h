#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <Arduino.h>
#include <stdint.h>

#include <GxEPD2_3C.h>
#include "Layout.h"

/**
 * Utility di disegno condivise fra i moduli (Weather, Calendar, Mail).
 * Header-only, non tiene stato: le funzioni di disegno scrivono direttamente
 * sul display globale dichiarato nello sketch .ino e usano il font che il
 * chiamante ha impostato.
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
   * Cap height del font corrente, misurata sulla "H": è la stessa per ogni
   * stringa, quindi allinea testi e icone senza dipendere da ascendenti e
   * discendenti del testo da scrivere.
   */
  inline int16_t capHeight()
  {
    int16_t  x1, y1;
    uint16_t w, h;
    display.getTextBounds("H", 0, 0, &x1, &y1, &w, &h);
    return (int16_t)h;
  }

  /**
   * Cima di un'icona piccola (Layout::SMALL_ICON_SIZE) da affiancare a un
   * testo nel font corrente: il centro dell'icona cade a metà della cap height.
   */
  inline int16_t smallIconY(int16_t baselineY)
  {
    return baselineY - capHeight() / 2 - Layout::SMALL_ICON_SIZE / 2;
  }

  /**
   * Disegna una riga di testo che parte con il bordo sinistro dell'inchiostro
   * su leftX, compensando il bearing del primo carattere. Restituisce la x
   * del cursore dopo l'ultimo carattere.
   */
  inline int16_t drawLeftAligned(const char* txt, int16_t leftX, int16_t baselineY, uint16_t color)
  {
    int16_t  x1, y1;
    uint16_t w, h;
    display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(leftX - x1, baselineY);
    display.setTextColor(color);
    display.print(txt);
    return display.getCursorX();
  }

  /** Disegna una stringa centrata orizzontalmente su centerX. */
  inline void drawCentered(const char* txt, int16_t centerX, int16_t baselineY, uint16_t color)
  {
    int16_t  x1, y1;
    uint16_t w, h;
    display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(centerX - (int16_t)(w / 2) - x1, baselineY);
    display.setTextColor(color);
    display.print(txt);
  }

  /** Disegna una stringa con il bordo destro dell'inchiostro su rightX. */
  inline void drawRightAligned(const char* txt, int16_t rightX, int16_t baselineY, uint16_t color)
  {
    int16_t  x1, y1;
    uint16_t w, h;
    display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(rightX - (int16_t)w - x1, baselineY);
    display.setTextColor(color);
    display.print(txt);
  }

  /** Larghezza dell'inchiostro di una stringa nel font corrente. */
  inline int16_t textWidth(const char* txt)
  {
    int16_t  x1, y1;
    uint16_t w, h;
    display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
    return (int16_t)w;
  }

  /**
   * Tronca sul posto la stringa finchè la sua resa con ".." in coda rientra
   * in maxW. Il candidato si misura con i puntini già scritti, perchè ".."
   * può essere più largo dei caratteri che sostituisce; gli spazi rimasti in
   * coda al prefisso si tolgono.
   */
  inline void truncateToWidth(char* buf, int16_t maxW)
  {
    if (textWidth(buf) <= maxW) return;

    size_t length = strlen(buf);
    if (length < 2) return;
    for (size_t prefix = length - 2; ; --prefix)
    {
      size_t end = prefix;
      while (end > 0 && buf[end - 1] == ' ') --end;
      buf[end] = '.';
      buf[end + 1] = '.';
      buf[end + 2] = '\0';
      if (prefix == 0 || textWidth(buf) <= maxW) return;
    }
  }

  /**
   * Disegna un rettangolo a bordi arrotondati con il titolo seduto sul bordo
   * superiore, che una striscia di sfondo sotto il testo interrompe. Il font
   * è quello impostato dal chiamante.
   *
   * La baseline sta mezza cap height sotto il bordo e non dipende dalla
   * stringa: un titolo con discendenti, come "Maggio", e uno senza, come
   * "Ottobre", restano alla stessa altezza e dentro lo schermo.
   *
   * @param x, y, w, h       rettangolo in pixel.
   * @param radius           raggio degli angoli.
   * @param title            titolo, nessuno se nullptr o vuoto.
   * @param titleLeftOffset  distanza del titolo dal bordo sinistro.
   * @param bgColor          colore della striscia sotto il titolo, di norma GxEPD_WHITE.
   */
  inline void drawFieldsetRect(int16_t x, int16_t y, int16_t w, int16_t h,
                               int16_t radius, const char* title,
                               int16_t titleLeftOffset, uint16_t bgColor)
  {
    display.drawRoundRect(x, y, w, h, radius, GxEPD_BLACK);

    if (!title || !*title) return;

    // La striscia alta 3 px copre il bordo da 1 px con un pixel di margine per lato.
    const int16_t stripX = x + titleLeftOffset;
    display.fillRect(stripX - 4, y - 1, textWidth(title) + 8, 3, bgColor);
    drawLeftAligned(title, stripX, y + capHeight() / 2, GxEPD_BLACK);
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

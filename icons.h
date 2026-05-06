#ifndef ICONS_H
#define ICONS_H

#include <stdint.h>
#include <pgmspace.h>

#include "Layout.h"

/**
 * Icone meteo per il banner OpenWeatherMap.
 * Ogni icona è un bitmap 1bpp Layout::ICON_SIZE x Layout::ICON_SIZE in PROGMEM
 * (lato selezionato dal Layout, default 88 per il 097c). Il codice icona
 * OpenWeatherMap è del tipo "01d", "02n", ecc.
 * (vedi https://openweathermap.org/weather-conditions).
 *
 * Le icone vere saranno sostituite dall'utente; qui c'è solo un placeholder
 * tutto bianco come fallback per far compilare lo sketch.
 *
 * Usato da: Weather.h (funzione renderBlock)
 *
 * @since 20/04/26 Mattia Alesi
 */

/** Placeholder ICON_SIZE x ICON_SIZE tutto bianco. Sostituire con icone reali. */
static const uint8_t ICON_PLACEHOLDER[Layout::ICON_SIZE * Layout::ICON_SIZE / 8] PROGMEM = {
  /* Inizializzazione implicita a zero (pixel spenti = bianco su e-paper). */
  0x00
};

/**
 * Lookup dell'icona a partire dal codice OpenWeatherMap.
 * Ritorna un puntatore a un bitmap 88x88 1bpp in PROGMEM, oppure il
 * placeholder se il codice non è mappato.
 *
 * Usato da: Weather.h (renderBlock)
 *
 * @param code codice icona OWM (es. "10d"); puo' essere NULL.
 * @return puntatore PROGMEM all'icona 96x96 1bpp.
 */
inline const uint8_t* iconFromCode(const char* code)
{
  // TODO: sostituire con lookup reale quando le icone saranno disponibili.
  (void)code;
  return ICON_PLACEHOLDER;
}

// ===========================================================================
// Icone piccole per la sub-colonna dati ambientali BME680 (Weather.h
// -> renderIndoorSubColumn). Bitmap 1bpp 20x20 in PROGMEM, MSB-first,
// compatibili con display.drawBitmap(). Lato selezionato dal Layout
// tramite Layout::INDOOR_ICON_SIZE (default 20 per il 097c).
// Le bitmap qui sotto sono dimensionate "in chiaro" come 20x20 (3 byte
// per riga * 20 righe = 60 byte) perche' i loro contenuti dipendono dal
// layout grafico fisico delle icone, non dalla dimensione astratta.
// @since 21/04/26 Mattia Alesi
// ===========================================================================

/**
 * Icona temperatura (termometro 20x20, nero solido).
 * Stem verticale + bulbo tondo in basso.
 */
static const uint8_t INDOOR_ICON_TEMPERATURE[Layout::INDOOR_ICON_SIZE * 3] PROGMEM = {
  0x01, 0xE0, 0x00,   // riga  0: .......XXXX......... (stem)
  0x01, 0xE0, 0x00,   // riga  1
  0x01, 0xE0, 0x00,   // riga  2
  0x01, 0xE0, 0x00,   // riga  3
  0x01, 0xE0, 0x00,   // riga  4
  0x01, 0xE0, 0x00,   // riga  5
  0x01, 0xE0, 0x00,   // riga  6
  0x01, 0xE0, 0x00,   // riga  7
  0x01, 0xE0, 0x00,   // riga  8
  0x01, 0xE0, 0x00,   // riga  9
  0x01, 0xE0, 0x00,   // riga 10
  0x01, 0xE0, 0x00,   // riga 11
  0x01, 0xE0, 0x00,   // riga 12
  0x03, 0xF0, 0x00,   // riga 13: ......XXXXXX........
  0x07, 0xF8, 0x00,   // riga 14: .....XXXXXXXX....... (bulbo)
  0x07, 0xF8, 0x00,   // riga 15
  0x07, 0xF8, 0x00,   // riga 16
  0x07, 0xF8, 0x00,   // riga 17
  0x03, 0xF0, 0x00,   // riga 18
  0x00, 0xC0, 0x00    // riga 19: ........XX..........
};

/**
 * Icona umidita' (goccia d'acqua 20x20, piena nera).
 * Forma a teardrop: punta in alto, rotonda in basso.
 */
static const uint8_t INDOOR_ICON_HUMIDITY[Layout::INDOOR_ICON_SIZE * 3] PROGMEM = {
  0x00, 0x60, 0x00,   // riga  0: .........XX.........
  0x00, 0x60, 0x00,   // riga  1
  0x00, 0xF0, 0x00,   // riga  2: ........XXXX........
  0x00, 0xF0, 0x00,   // riga  3
  0x01, 0xF8, 0x00,   // riga  4: .......XXXXXX.......
  0x01, 0xF8, 0x00,   // riga  5
  0x03, 0xFC, 0x00,   // riga  6: ......XXXXXXXX......
  0x03, 0xFC, 0x00,   // riga  7
  0x07, 0xFE, 0x00,   // riga  8: .....XXXXXXXXXX.....
  0x07, 0xFE, 0x00,   // riga  9
  0x0F, 0xFF, 0x00,   // riga 10: ....XXXXXXXXXXXX....
  0x0F, 0xFF, 0x00,   // riga 11
  0x0F, 0xFF, 0x00,   // riga 12
  0x0F, 0xFF, 0x00,   // riga 13
  0x0F, 0xFF, 0x00,   // riga 14
  0x07, 0xFE, 0x00,   // riga 15: .....XXXXXXXXXX.....
  0x07, 0xFE, 0x00,   // riga 16
  0x03, 0xFC, 0x00,   // riga 17: ......XXXXXXXX......
  0x01, 0xF8, 0x00,   // riga 18: .......XXXXXX.......
  0x00, 0xF0, 0x00    // riga 19: ........XXXX........
};

/**
 * Icona pressione atmosferica (tachimetro/barometro 20x20, nero).
 * Quadrante semicircolare con lancetta centrale: cornice semicerchio
 * in alto, linea orizzontale base, lancetta diagonale che parte dal
 * centro basso verso il settore destro del quadrante.
 * @since 22/04/26
 */
static const uint8_t INDOOR_ICON_PRESSURE[Layout::INDOOR_ICON_SIZE * 3] PROGMEM = {
  0x00, 0x00, 0x00,   // riga  0
  0x00, 0x00, 0x00,   // riga  1
  0x03, 0xF0, 0x00,   // riga  2: ......XXXXXX........ (arco superiore)
  0x0C, 0x18, 0x00,   // riga  3: ....XX....XX........
  0x10, 0x04, 0x00,   // riga  4: ...X........X.......
  0x20, 0x22, 0x00,   // riga  5: ..X......X...X...... (tacca)
  0x20, 0x22, 0x00,   // riga  6: ..X......X...X......
  0x40, 0x21, 0x00,   // riga  7: .X.......X...X......
  0x40, 0x41, 0x00,   // riga  8: .X......X....X...... (lancetta)
  0x40, 0x80, 0x80,   // riga  9: .X.....X......X.....
  0x41, 0x00, 0x80,   // riga 10: .X....X.......X.....
  0x42, 0x00, 0x80,   // riga 11: .X...X........X.....
  0x40, 0x00, 0x80,   // riga 12: .X............X.....
  0x20, 0x00, 0x00,   // riga 13: ..X..........X......
  0x20, 0x01, 0x00,   // riga 14: ..X..........X......
  0x10, 0x02, 0x00,   // riga 15: ...X........X.......
  0x0F, 0xFC, 0x00,   // riga 16: ....XXXXXXXXXX...... (base)
  0x00, 0x00, 0x00,   // riga 17
  0x00, 0x00, 0x00,   // riga 18
  0x00, 0x00, 0x00    // riga 19
};

/**
 * Icona sunrise 20x20: 10 linee orizzontali progressivamente piu' larghe
 * verso l'alto. La riga piu' larga (20 px) è in cima, la piu' stretta
 * (2 px) è in basso: rappresenta il sole che sorge emanando raggi
 * crescenti. Spaziatura verticale di 1 riga vuota fra linee.
 * @since 22/04/26
 */
static const uint8_t INDOOR_ICON_SUNRISE[Layout::INDOOR_ICON_SIZE * 3] PROGMEM = {
  0xFF, 0xFF, 0xF0,   // riga  0: XXXXXXXXXXXXXXXXXXXX (w=20)
  0x00, 0x00, 0x00,   // riga  1
  0x7F, 0xFF, 0xE0,   // riga  2: .XXXXXXXXXXXXXXXXXX. (w=18)
  0x00, 0x00, 0x00,   // riga  3
  0x3F, 0xFF, 0xC0,   // riga  4: ..XXXXXXXXXXXXXXXX.. (w=16)
  0x00, 0x00, 0x00,   // riga  5
  0x1F, 0xFF, 0x80,   // riga  6: ...XXXXXXXXXXXXXX... (w=14)
  0x00, 0x00, 0x00,   // riga  7
  0x0F, 0xFF, 0x00,   // riga  8: ....XXXXXXXXXXXX.... (w=12)
  0x00, 0x00, 0x00,   // riga  9
  0x07, 0xFE, 0x00,   // riga 10: .....XXXXXXXXXX..... (w=10)
  0x00, 0x00, 0x00,   // riga 11
  0x03, 0xFC, 0x00,   // riga 12: ......XXXXXXXX...... (w=8)
  0x00, 0x00, 0x00,   // riga 13
  0x01, 0xF8, 0x00,   // riga 14: .......XXXXXX....... (w=6)
  0x00, 0x00, 0x00,   // riga 15
  0x00, 0xF0, 0x00,   // riga 16: ........XXXX........ (w=4)
  0x00, 0x00, 0x00,   // riga 17
  0x00, 0x60, 0x00,   // riga 18: .........XX......... (w=2)
  0x00, 0x00, 0x00    // riga 19
};

/**
 * Icona sunset 20x20: semicerchio (sole) che "tocca" una linea orizzontale
 * di orizzonte immediatamente sotto la base. Il semicerchio è riempito e
 * copre la meta' superiore, la linea di orizzonte è una singola riga a
 * larghezza massima che si appoggia contro la base del sole.
 * @since 22/04/26
 */
static const uint8_t INDOOR_ICON_SUNSET[Layout::INDOOR_ICON_SIZE * 3] PROGMEM = {
  0x00, 0x00, 0x00,   // riga  0
  0x00, 0x00, 0x00,   // riga  1
  0x00, 0xF0, 0x00,   // riga  2: ........XXXX........ (apex sole, w=4)
  0x01, 0xF8, 0x00,   // riga  3: .......XXXXXX....... (w=6)
  0x03, 0xFC, 0x00,   // riga  4: ......XXXXXXXX...... (w=8)
  0x07, 0xFE, 0x00,   // riga  5: .....XXXXXXXXXX..... (w=10)
  0x07, 0xFE, 0x00,   // riga  6: .....XXXXXXXXXX..... (w=10)
  0x0F, 0xFF, 0x00,   // riga  7: ....XXXXXXXXXXXX.... (w=12)
  0x0F, 0xFF, 0x00,   // riga  8: ....XXXXXXXXXXXX.... (w=12)
  0x1F, 0xFF, 0x80,   // riga  9: ...XXXXXXXXXXXXXX... (base sole, w=14)
  0xFF, 0xFF, 0xF0,   // riga 10: XXXXXXXXXXXXXXXXXXXX (orizzonte, tocca la base, w=20)
  0x00, 0x00, 0x00,   // riga 11
  0x00, 0x00, 0x00,   // riga 12
  0x00, 0x00, 0x00,   // riga 13
  0x00, 0x00, 0x00,   // riga 14
  0x00, 0x00, 0x00,   // riga 15
  0x00, 0x00, 0x00,   // riga 16
  0x00, 0x00, 0x00,   // riga 17
  0x00, 0x00, 0x00,   // riga 18
  0x00, 0x00, 0x00    // riga 19
};

/**
 * Icona qualita' aria / IAQ (foglia 20x20, nero solido).
 * Corpo simmetrico a goccia rovesciata (tip in alto) + gambo verticale
 * in basso che termina con una piccola piega.
 */
static const uint8_t INDOOR_ICON_AIR_QUALITY[Layout::INDOOR_ICON_SIZE * 3] PROGMEM = {
  0x00, 0x40, 0x00,   // riga  0: .........X.......... (tip)
  0x00, 0xE0, 0x00,   // riga  1: ........XXX.........
  0x01, 0xF0, 0x00,   // riga  2: .......XXXXX........
  0x03, 0xF8, 0x00,   // riga  3: ......XXXXXXX.......
  0x03, 0xFC, 0x00,   // riga  4: ......XXXXXXXX......
  0x07, 0xFE, 0x00,   // riga  5: .....XXXXXXXXXX.....
  0x0F, 0xFF, 0x00,   // riga  6: ....XXXXXXXXXXXX....
  0x0F, 0xFF, 0x00,   // riga  7
  0x0F, 0xFF, 0x00,   // riga  8
  0x0F, 0xFF, 0x00,   // riga  9
  0x0F, 0xFF, 0x00,   // riga 10
  0x07, 0xFE, 0x00,   // riga 11: .....XXXXXXXXXX.....
  0x03, 0xFC, 0x00,   // riga 12: ......XXXXXXXX......
  0x01, 0xF8, 0x00,   // riga 13: .......XXXXXX.......
  0x00, 0xF0, 0x00,   // riga 14: ........XXXX........
  0x00, 0x60, 0x00,   // riga 15: .........XX......... (base)
  0x00, 0x40, 0x00,   // riga 16: .........X.......... (gambo)
  0x00, 0x40, 0x00,   // riga 17
  0x00, 0x40, 0x00,   // riga 18
  0x00, 0xC0, 0x00    // riga 19: ........XX.......... (piega terminale)
};

#endif

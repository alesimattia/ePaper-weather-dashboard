#ifndef LOG_H
#define LOG_H

#include <Arduino.h>

/**
 * Verbosita' dei log seriali, risolta a compile-time.
 *   0 = nessun log
 *   1 = log operativi: esiti dei fetch, errori, transizioni di stato
 *   2 = 1 + dettaglio per singolo elemento (righe ripetute a ogni ciclo)
 *
 * Il valore si dichiara nel .ino insieme alle altre costanti di
 * configurazione, PRIMA degli include dei moduli. Questo #ifndef e' il
 * fallback che tiene l'header compilabile da solo, come gli altri moduli.
 */
#ifndef LOG_LEVEL
  #define LOG_LEVEL 1
#endif

/**
 * Espansione comune: il preprocessore concatena tag, testo e a capo in
 * un'unica stringa, quindi non c'e' nessuna concatenazione a runtime e il
 * letterale che finisce in .rodata e' quello finale.
 *
 * Il controllo della format string e' quello di Print::printf, dichiarata
 * __attribute__((format(printf,2,3))) nel core: un %d con un const char*
 * fa scattare -Wformat= indicando la riga della chiamata. Per vederlo la
 * compilazione va lanciata con --warnings more, perche' la build di
 * default passa -w.
 *
 * Niente F(): su ESP32 e' un puro cast, e selezionerebbe l'overload
 * printf(const __FlashStringHelper*) che NON ha l'attributo format.
 */
#define LOG_EMIT(tag, fmt, ...) \
  Serial.printf("[" tag "] " fmt "\n" __VA_OPT__(,) __VA_ARGS__)

/**
 * Forma spenta. La chiamata sta dentro sizeof, che e' un contesto non
 * valutato: non genera ne' codice ne' stringhe in .rodata, ma resta
 * type-checked e marca gli argomenti come usati, cosi' le variabili che
 * servono solo al log non diventano -Wunused-variable.
 *
 * E' un'espressione, non uno statement: LOG(...) resta usabile come ramo
 * senza graffe di un if/else senza il solito do{}while(0).
 */
#define LOG_DISCARD(tag, fmt, ...) \
  ((void)sizeof(LOG_EMIT(tag, fmt __VA_OPT__(,) __VA_ARGS__)))

/**
 * Riga di log su Serial, con tag fra parentesi quadre e a capo automatico.
 * `tag` e `fmt` devono essere letterali.
 *
 *   LOG("Mail", "list vuota (nessuna mail)");
 *   LOG("Mail", "list fetch failed: http=%d", code);
 *
 * Il '\n' finale lo mette la macro: non va ripetuto in `fmt`. Un '%' che
 * deve comparire nel testo va raddoppiato ("%%"), perche' il messaggio
 * passa da printf anche quando non ha argomenti.
 */
#if LOG_LEVEL >= 1
  #define LOG  LOG_EMIT
#else
  #define LOG  LOG_DISCARD
#endif

/** Variante di dettaglio: stessa sintassi di LOG, attiva da LOG_LEVEL 2. */
#if LOG_LEVEL >= 2
  #define LOGV LOG_EMIT
#else
  #define LOGV LOG_DISCARD
#endif

#endif // LOG_H

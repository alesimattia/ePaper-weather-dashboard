// =============================================================================
// test_12_2.ino — Test minimale del driver GxEPD2_122c_SOLUM_960x768
// per pannello e-paper SOLUM Newton-Core 12.2" (960x768 BWR, dual-controller).
//
// HARDWARE
//   - Board: Waveshare E-Paper ESP32 Driver Board (SPI HSPI fissa).
//   - Pannello: SOLUM Newton-Core 12.2" con 2 cavi FFC da 21 pin.
//   - FFC #1 (master) -> connettore interno della board (CS=GPIO15, BUSY=GPIO25).
//   - FFC #2 (slave)  -> breakout 24-pin 0.5mm esterno (FFC inserito allineato
//     a un lato, 3 pin liberi sull'altro). Cablaggio:
//       CS_S    -> GPIO 33  (output, libero sul pin header espansione)
//       BUSY_S  -> GPIO 35  (input-only, libero)
//       SCK     -> GPIO 13  (parallelo al FFC interno)
//       MOSI    -> GPIO 14  (parallelo)
//       DC      -> GPIO 27  (parallelo)
//       RST     -> GPIO 26  (parallelo)
//       3V3/GND -> rail breadboard
//
// COMPORTAMENTO
//   Loop infinito che mostra full-screen, in sequenza, ogni 60 s:
//     1. BIANCO
//     2. NERO
//     3. ROSSO
//   Su Serial (115200) vengono stampati i tempi di refresh effettivi.
//
// BRING-UP STEP-BY-STEP
//   Step 1: cabla solo FFC #1 (master), commenta le define USE_DUAL_CONTROLLER
//           sotto e il costruttore userà solo il master. Vedrai SOLO la meta'
//           sinistra del pannello (cols 0..479) cambiare colore: la destra
//           resta scura/random. Conferma che l'init UC8179 funziona e che il
//           BUSY si rilascia entro ~25 s.
//   Step 2: cabla anche FFC #2 (slave), riabilita USE_DUAL_CONTROLLER. Tutto
//           il pannello deve cambiare colore. Se solo meta' aggiorna o ci
//           sono artefatti, vedi sezione TODO[VERIFY] del README del driver.
// =============================================================================

#include <Arduino.h>
#include "../GxEPD2_122c_SOLUM_960x768/GxEPD2_122c_SOLUM_960x768.h"

// Commenta questa define per il bring-up Step 1 (solo master cablato).
#define USE_DUAL_CONTROLLER

// ----- Pin Waveshare E-Paper ESP32 Driver Board (master, FFC interno) -----
#define PIN_SCK    13
#define PIN_MISO   12
#define PIN_MOSI   14
#define PIN_CS_M   15
#define PIN_DC     27
#define PIN_RST    26
#define PIN_BUSY_M 25
// ----- Pin slave (FFC esterno via pin header espansione) -----
#define PIN_CS_S   33
#define PIN_BUSY_S 35

#if defined(USE_DUAL_CONTROLLER)
GxEPD2_122c_SOLUM_960x768 display(
    PIN_SCK, PIN_MISO, PIN_MOSI,
    PIN_CS_M, PIN_CS_S,
    PIN_DC, PIN_RST,
    PIN_BUSY_M, PIN_BUSY_S);
#else
// Bring-up Step 1: costruttore single-CS, lo slave e' disabilitato (cs=-1)
// e le scritture verso la sua meta' del pannello vengono saltate.
GxEPD2_122c_SOLUM_960x768 display(
    PIN_CS_M, PIN_DC, PIN_RST, PIN_BUSY_M);
#endif

// Convenzioni colore UC8179:
//   cmd 0x10 (black plane): bit=1 -> pixel bianco,  bit=0 -> pixel nero
//   cmd 0x13 (color plane): bit=1 -> pixel rosso,   bit=0 -> non rosso
// I valori passati a clearScreen(black_value, color_value) sono il "fill" RAM
// per ciascun piano, quindi per i 3 colori puri:
struct ColorTest {
  const char* name;
  uint8_t black_value;  // riempimento cmd 0x10
  uint8_t color_value;  // riempimento cmd 0x13
};

const ColorTest TESTS[] = {
  { "BIANCO", 0xFF, 0x00 },  // black plane tutto bianco, color plane spento
  { "NERO",   0x00, 0x00 },  // black plane tutto nero, color plane spento
  { "ROSSO",  0xFF, 0xFF },  // black plane bianco (lascia trasparire), color plane tutto rosso
};
const size_t NUM_TESTS = sizeof(TESTS) / sizeof(TESTS[0]);

// Periodo di attesa tra una visualizzazione e la successiva.
const unsigned long DELAY_BETWEEN_COLORS_MS = 60000;

void setup() {
  // display.init(baudrate) chiama internamente Serial.begin(baudrate)
  // e abilita i log diagnostici del driver (tempi BUSY, timeout).
  display.init(115200);
  delay(100);

  Serial.println();
  Serial.println(F("=================================================="));
  Serial.println(F(" test_12_2 — driver SOLUM 12.2\" (960x768 BWR)"));
  Serial.println(F("=================================================="));
  Serial.print  (F(" Risoluzione: "));
  Serial.print  (GxEPD2_122c_SOLUM_960x768::WIDTH);
  Serial.print  (F("x"));
  Serial.println(GxEPD2_122c_SOLUM_960x768::HEIGHT);
#if defined(USE_DUAL_CONTROLLER)
  Serial.println(F(" Modalita': DUAL CONTROLLER (master + slave)"));
  Serial.print  (F(" CS_M="));   Serial.print(PIN_CS_M);
  Serial.print  (F(" CS_S="));   Serial.print(PIN_CS_S);
  Serial.print  (F(" BUSY_M=")); Serial.print(PIN_BUSY_M);
  Serial.print  (F(" BUSY_S=")); Serial.println(PIN_BUSY_S);
#else
  Serial.println(F(" Modalita': SINGLE-CS (solo master, slave disabilitato)"));
  Serial.print  (F(" CS=")); Serial.print(PIN_CS_M);
  Serial.print  (F(" BUSY=")); Serial.println(PIN_BUSY_M);
#endif
  Serial.print  (F(" SCK=")); Serial.print(PIN_SCK);
  Serial.print  (F(" MOSI=")); Serial.print(PIN_MOSI);
  Serial.print  (F(" DC=")); Serial.print(PIN_DC);
  Serial.print  (F(" RST=")); Serial.println(PIN_RST);
  Serial.println(F("--------------------------------------------------"));
}

void loop() {
  static size_t idx = 0;
  const ColorTest& t = TESTS[idx];

  Serial.print(F("["));
  Serial.print(idx + 1);
  Serial.print(F("/"));
  Serial.print(NUM_TESTS);
  Serial.print(F("] full "));
  Serial.print(t.name);
  Serial.print(F(" (black=0x"));
  Serial.print(t.black_value, HEX);
  Serial.print(F(", color=0x"));
  Serial.print(t.color_value, HEX);
  Serial.println(F(") — refresh in corso..."));

  unsigned long t0 = millis();
  display.clearScreen(t.black_value, t.color_value);
  unsigned long elapsed = millis() - t0;

  Serial.print(F("    refresh completato in "));
  Serial.print(elapsed);
  Serial.print(F(" ms — sleep "));
  Serial.print(DELAY_BETWEEN_COLORS_MS / 1000);
  Serial.println(F(" s prima del prossimo colore"));

  delay(DELAY_BETWEEN_COLORS_MS);

  idx = (idx + 1) % NUM_TESTS;
}

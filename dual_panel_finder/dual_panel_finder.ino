// =============================================================================
// dual_panel_finder.ino — validazione bring-up SOLUM Newton-Core 12.2"
//
// Sketch standalone (no driver custom) modellato su panel_finder.ino.
// Usa il driver stock GxEPD2_1160c_GDEY116Z91 (controller SSD16xx) per pilotare
// uno dei due controller del pannello SOLUM 12.2" alla volta.
//
// Obiettivo: confermare che il controller del Newton-Core e' SSD16xx (non
// UC8179 come assunto dal driver custom GxEPD2_122c_SOLUM_960x768) e che
// entrambi i FFC rispondono ciascuno con i propri pin CS/BUSY.
//
// PROCEDURA
//   1. Compila con TEST_TARGET = TEST_MASTER, flasha, osserva il pannello e
//      annota tempi sul serial (115200). La meta' del pannello pilotata dal
//      controller del FFC #1 deve cambiare colore.
//   2. Compila con TEST_TARGET = TEST_SLAVE, flasha, ripeti. La meta'
//      complementare (FFC #2) deve cambiare colore.
//
// HARDWARE
//   - Board: Waveshare E-Paper ESP32 Driver Board (HSPI: SCK=13, MISO=12,
//     MOSI=14). FFC #1 nel connettore interno della board.
//   - FFC #2: breakout 24-pin esterno cablato come da driver custom README,
//     con CS_S=GPIO33 e BUSY_S=GPIO35.
//
// NOTE TECNICHE
//   - GxEPD2_1160c::HEIGHT = 640. Il SOLUM e' 768 alto: lo sketch scrive solo
//     le prime 640 righe. Le ultime 128 righe del SOLUM resteranno dello
//     stato precedente (accettabile per un test di vita).
//   - SPI clock 4 MHz come panel_finder.ino, non 20 MHz. Margine sicuro per
//     il bring-up; bumpare in seguito se tutto funziona.
//   - reset_duration = 2 ms come panel_finder.ino.
// =============================================================================

#include <SPI.h>
#include <GxEPD2_3C.h>
#include "image.h"

// === Compile-time switch: master | slave ============================
#define TEST_MASTER   1
#define TEST_SLAVE    2
#define TEST_TARGET   TEST_SLAVE   // <<< cambia tra MASTER e SLAVE e riflasha

// === Pin condivisi (Waveshare E-Paper ESP32 Driver Board, HSPI) =====
static const int PIN_SCK  = 13;
static const int PIN_MISO = 12;
static const int PIN_MOSI = 14;
static const int EPD_DC   = 27;
static const int EPD_RST  = 26;

#if TEST_TARGET == TEST_MASTER
  static const int  EPD_CS         = 15;   // FFC #1 (connettore interno della board)
  static const int  EPD_BUSY       = 25;
  static const char TARGET_LABEL[] = "MASTER (FFC#1, CS=15, BUSY=25)";
#elif TEST_TARGET == TEST_SLAVE
  static const int  EPD_CS         = 33;   // FFC #2 (breakout esterno)
  static const int  EPD_BUSY       = 35;
  static const char TARGET_LABEL[] = "SLAVE  (FFC#2, CS=33, BUSY=35)";
#else
  #error "TEST_TARGET deve essere TEST_MASTER o TEST_SLAVE"
#endif

SPIClass hspi(HSPI);

GxEPD2_3C<GxEPD2_1160c_GDEY116Z91, GxEPD2_1160c_GDEY116Z91::HEIGHT / 2>
    display(GxEPD2_1160c_GDEY116Z91(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// Disegna l'immagine di test 960x380 (image.h) come unico layer nero su sfondo
// bianco. Lo schema della bitmap e' asimmetrico (screenshot macOS) per
// riconoscere a colpo d'occhio quale quadrante del pannello reagisce.
void drawImage()
{
  display.setRotation(0);
  display.setFullWindow();

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawBitmap(0, 0, img_test, 960, 380, GxEPD_BLACK);
  } while (display.nextPage());
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println(F("=================================================="));
  Serial.print  (F(" dual_panel_finder — target: "));
  Serial.println(TARGET_LABEL);
  Serial.println(F(" stock driver: GxEPD2_1160c_GDEY116Z91 (SSD16xx)"));
  Serial.println(F("=================================================="));

  hspi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, EPD_CS);
  display.epd2.selectSPI(hspi, SPISettings(4000000, MSBFIRST, SPI_MODE0));

  /**
   * init(serial_diag_bitrate=115200, initial=true, reset_duration=2,
   *      pulldown_rst_mode=false): chiama Serial.begin internamente,
   * abilita i log diagnostici (tempi BUSY, "Busy Timeout!"), pulse di
   * reset corto come panel_finder.
   */
  Serial.println(F("init..."));
  unsigned long t0 = millis();
  display.init(115200, true, 2, false);
  Serial.print(F("init done in "));
  Serial.print(millis() - t0);
  Serial.println(F(" ms"));

  Serial.println(F("draw image (full window, fillScreen + drawBitmap)..."));
  t0 = millis();
  drawImage();
  Serial.print(F("draw + refresh in "));
  Serial.print(millis() - t0);
  Serial.println(F(" ms"));

  display.hibernate();
  Serial.println(F("hibernate done — observe panel ora"));
}

void loop()
{
}

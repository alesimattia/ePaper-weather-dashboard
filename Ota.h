#ifndef OTA_H
#define OTA_H

/**
 * Modulo OTA header-only: al boot apre una rete WiFi AP dedicata e un
 * piccolo web server con pagina /update (HTTPUpdateServer) per permettere
 * al tecnico di campo di caricare un nuovo firmware .bin senza smontare
 * il device.
 *
 * La durata e' data da OTA_WINDOW_MIN (default 3 min, dichiarato nello
 * sketch .ino); scaduta la finestra l'AP
 * viene spenta "per sempre" (fino al prossimo reboot) e la radio viene
 * lasciata in WIFI_OFF, cosi' il ciclo normale STA on-demand + light
 * sleep di GxEPD2_1330c_GDEM133Z91.ino puo' riprendere senza modifiche.
 *
 * Il modulo usa WIFI_AP_STA in modo che, mentre l'AP e' attiva, la STA
 * si colleghi in parallelo al router di casa: cosi' Weather::runFetch()
 * puo' girare come nel flusso normale durante la finestra OTA.
 *
 * L'unica barriera di accesso alla pagina /update e' la password WPA2
 * dell'AP (OTA_AP_PASSWORD in Env.h): chi non e' sull'AP non raggiunge
 * il web server. Niente Basic Auth sulla pagina HTTP.
 *
 * Scelta "header-only": l'implementazione e' piccola (~60 righe) e
 * viene inclusa solo dal .ino, quindi non serve una compilation unit
 * separata. Le funzioni sono `inline` e le variabili di stato
 * `inline static` (richiede C++17, abilitato di default da arduino-esp32).
 *
 * Usato da: GxEPD2_1330c_GDEM133Z91.ino
 *
 * @since 20/04/26 Mattia Alesi
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>

#include "Env.h"

namespace Ota
{
  /**
   * Durata della finestra OTA dall'avvio (millisecondi). Deriva da
   * OTA_WINDOW_MIN dichiarato nello sketch .ino; il fallback rende
   * l'header autonomamente compilabile.
   */
  #ifndef OTA_WINDOW_MIN
  #define OTA_WINDOW_MIN 3
  #endif
  inline constexpr uint32_t OTA_WINDOW_MS =
      (uint32_t)OTA_WINDOW_MIN * 60UL * 1000UL;  // default 3 min

  // -------------------------------------------------------------------------
  // Stato interno (incapsulato nel namespace Ota::detail per non inquinare
  // il namespace globale).
  // -------------------------------------------------------------------------
  namespace detail
  {
    inline WebServer        server(80);
    inline HTTPUpdateServer updater;
    inline uint32_t         deadlineMs = 0;
    inline bool             started    = false;
    inline bool             stopped    = false;

    /**
     * Redirect 301 di GET "/" verso "/update": chi apre l'IP dell'AP nel
     * browser finisce diretto sul form di upload, senza landing intermedia.
     *
     * Usato da: server.on("/", ...) in begin()
     *
     * @since 20/04/26 Mattia Alesi
     */
    inline void handleRoot()
    {
      server.sendHeader("Location", "/update");
      server.send(301, "text/plain", "");
    }
  }

  /**
   * Accende il WiFi in modalita' AP_STA, avvia il WebServer + HTTPUpdateServer
   * e memorizza la deadline (now + OTA_WINDOW_MIN). Chiamata una sola volta in setup().
   */
  inline void begin()
  {
    using namespace detail;
    if (started) return;

    /** Modalita' AP+STA: AP per l'OTA, STA per il fetch meteo in parallelo. */
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(OTA_AP_SSID, OTA_AP_PASSWORD);
    /** La connessione STA e' non bloccante: la completa il core in background. */
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    /** HTTPUpdateServer aggiunge la pagina /update. L'unica sicurezza e'
     *  la password WPA2 dell'AP (OTA_AP_PASSWORD): chi non e' sull'AP non
     *  puo' raggiungere /update. */
    updater.setup(&server, "/update");
    server.on("/", HTTP_GET, handleRoot);
    server.begin();

    deadlineMs = millis() + OTA_WINDOW_MS;
    started = true;
    stopped = false;

    Serial.printf("[OTA] AP up SSID=%s IP=%s window=%lus\n",
                  OTA_AP_SSID,
                  WiFi.softAPIP().toString().c_str(),
                  (unsigned long)(OTA_WINDOW_MS / 1000UL));
  }

  /**
   * Ritorna true finche' la finestra OTA_WINDOW_MIN e' aperta. Usata dal loop()
   * del .ino per scegliere fra il ramo "finestra OTA" e il ramo "normale".
   */
  inline bool windowOpen()
  {
    using namespace detail;
    if (!started || stopped) return false;
    /** Sottrazione signed per essere rollover-safe come Weather::elapsed(). */
    return (int32_t)(millis() - deadlineMs) < 0;
  }

  /**
   * Processa eventuali richieste HTTP pendenti sul WebServer. Va chiamata
   * ad ogni giro di loop() durante la finestra. No-op dopo endNow().
   */
  inline void handle()
  {
    using namespace detail;
    if (!started || stopped) return;
    server.handleClient();
  }

  /**
   * Spegne AP, ferma il WebServer e mette WiFi in WIFI_OFF. Idempotente:
   * chiamate successive alla prima sono no-op. Dopo questa chiamata la
   * finestra non puo' piu' essere riaperta senza reboot.
   */
  inline void endNow()
  {
    using namespace detail;
    if (!started || stopped) return;

    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);

    stopped = true;
    Serial.println(F("[OTA] window closed, AP down until reboot"));
  }
}

#endif

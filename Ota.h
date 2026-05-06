#ifndef OTA_H
#define OTA_H

/**
 * Modulo OTA header-only: al boot apre una rete WiFi AP dedicata e un
 * piccolo web server con pagina /update (HTTPUpdateServer) per permettere
 * al tecnico di campo di caricare un nuovo firmware .bin senza smontare
 * il device.
 *
 * La durata è data da OTA_WINDOW_MIN (default 3 min, dichiarato nello
 * sketch .ino); scaduta la finestra l'AP
 * viene spenta "per sempre" (fino al prossimo reboot) e la radio viene
 * lasciata in WIFI_OFF, cosi' il ciclo normale STA on-demand + light
 * sleep di ePaper-weather-dashboard.ino puo' riprendere senza modifiche.
 *
 * Il modulo usa WIFI_AP_STA in modo che, mentre l'AP è attiva, la STA
 * si colleghi in parallelo al router di casa: cosi' Weather::runFetch()
 * puo' girare come nel flusso normale durante la finestra OTA.
 *
 * L'unica barriera di accesso alla pagina /update è la password WPA2
 * dell'AP (OTA_AP_PASSWORD in Env.h): chi non è sull'AP non raggiunge
 * il web server. Niente Basic Auth sulla pagina HTTP.
 *
 * Scelta "header-only": l'implementazione è piccola (~60 righe) e
 * viene inclusa solo dal .ino, quindi non serve una compilation unit
 * separata. Le funzioni sono `inline` e le variabili di stato
 * `inline static` (richiede C++17, abilitato di default da arduino-esp32).
 *
 * Usato da: ePaper-weather-dashboard.ino
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
		 * Pagina HTML minimale (~180 byte) servita su GET / e GET /update.
		 * Sostituisce la pagina default di HTTPUpdateServer (~600 byte) che
		 * include CSS inline non necessario per il caso d'uso (tecnico di
		 * campo, una sola volta per device, niente styling richiesto).
		 *
		 * Memorizzata in PROGMEM (Flash readonly): zero costo RAM, costo
		 * Flash equivalente alla lunghezza della stringa.
		 *
		 * Il name=update e l'enctype=multipart/form-data sono richiesti dal
		 * POST handler di HTTPUpdateServer (registrato da updater.setup).
		 * accept=.bin filtra lato browser i file selezionabili.
		 */
		static const char UPDATE_PAGE_HTML[] PROGMEM =
				"<!DOCTYPE html>"
				"<title>OTA</title>"
				"<meta name=viewport content=\"width=device-width\">"
				"<h2>Firmware update</h2>"
				"<form method=POST action=/update enctype=multipart/form-data>"
				"<input type=file name=update accept=.bin required>"
				"<button>Upload</button>"
				"</form>";

		/**
		 * Serve UPDATE_PAGE_HTML su GET / e GET /update. Registrato in begin()
		 * PRIMA di updater.setup() per garantire (via first-match-wins del
		 * WebServer ESP32) che la nostra pagina vinca sulla default di
		 * HTTPUpdateServer su GET /update. Il POST /update resta gestito da
		 * updater.setup() — niente conflitto perche' i metodi differiscono.
		 */
		inline void handleUpdatePage(){
			server.send_P(200, PSTR("text/html"), UPDATE_PAGE_HTML);
		}
	}

	/**
	 * Accende il WiFi in modalita' AP_STA, avvia il WebServer + HTTPUpdateServer
	 * e memorizza la deadline (now + OTA_WINDOW_MIN). Chiamata una sola volta in setup().
	 */
	inline void begin()
	{
		using namespace detail;
		if (started) 
			return;

		WiFi.mode(WIFI_AP_STA); // AP per l'OTA + STA per il fetch meteo in parallelo
		WiFi.softAP(OTA_AP_SSID, OTA_AP_PASSWORD);
		WiFi.begin(WIFI_SSID, WIFI_PASSWORD); // La connessione STA è non bloccante: la completa il core in background

		/** Pagina nativa minimale su GET / e GET /update. Registrata PRIMA
		 *  di updater.setup() perche' WebServer matcha gli handler in ordine
		 *  di registrazione (first-match-wins): la nostra GET /update vince
		 *  sul default di HTTPUpdateServer (~600 byte) che resta registrato
		 *  ma non viene mai servito.
		 *  Niente piu' redirect 301 / -> /update: una sola GET serve la pagina
		 *  direttamente, risparmiando un round-trip al client.
		 *  L'unica sicurezza resta la password WPA2 dell'AP (OTA_AP_PASSWORD):
		 *  chi non e' sull'AP non puo' raggiungere /update. */
		server.on("/", HTTP_GET, handleUpdatePage);
		server.on("/update", HTTP_GET, handleUpdatePage);
		/** updater.setup() registra GET (default page, shadowata da noi) +
		 *  POST (handler upload firmware reale, che lasciamo intatto). */
		updater.setup(&server, "/update");
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
	 * Ritorna true finchè la finestra OTA_WINDOW_MIN è aperta. Usata dal loop()
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
	 * chiamate successive alla prima sono no-op. 
	 * Dopo questa chiamata la finestra non puo' piu' essere riaperta senza reboot.
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

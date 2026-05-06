---
name: Catena di timeout fetch (WiFi → HTTP → OTA window)
description: Timeout incrociati firmware ↔ render.com ↔ keep-warm; budget worst-case del fetch cinema
type: project
---

I timeout sono distribuiti tra firmware e infra; vanno ragionati insieme:

**Firmware (`ePaper-weather-dashboard.ino`):**
- `wifiOn()` timeout = 15s (loop `while (status != WL_CONNECTED && millis()-t0 < 15000)`).
- `http.setTimeout(45000)` per il fetch cinema (45s).
- `OTA_WINDOW_MIN = 3` → finestra OTA da 180s al boot.

**Render.com free tier:**
- Sleep dopo 15 min idle.
- Cold start tipico 10-30s (commento esplicito nel `.ino` e in `keep-warm.yml`).

**Keep-warm GitHub Actions (`webapp/.github/workflows/keep-warm.yml`):**
- Cron `55 4 * * *` UTC (= 06:55 CEST estate) e `55 5 * * *` UTC (= 06:55 CET inverno).
- Pinga `/health` 5 min prima del fetch ESP32 alle 07:00 local (CINEMA_DAILY_FETCH_HOUR, apertura finestra WiFi mattutina).

**Worst case dedotto:**
- Boot freddo render.com: 15s (WiFi) + 30s (cold start HTTP) + ~3-4s (download 100 KB sul 097c, ~123 KB sul 122c, su WiFi) ≈ 48-49s.
- Sta sotto i 45s di `http.setTimeout` solo grazie al keep-warm. SENZA keep-warm, scenario realistico = timeout HTTP a 45s e fallback PROGMEM.
- OTA window 180s contiene anche il caso freddo (~50s fetch + altri fetch in parallelo: meteo + 2 calendari + mail).

**Budget durante OTA:**
- `loop()` gira con `delay(10)` durante OTA window per `WebServer::handleClient()`. Un fetch cinema bloccante da 45s congela l'AP per 45s: utenti che provano `/update` durante quel periodo vedono timeout dal browser. Accettato perchè la finestra OTA è rara e l'utente se ne accorge.

**Daily 07:00:**
- Render.com sleep di 15 min + keep-warm alle 06:55 = sicuramente sveglio alle 07:00. Se la cron Action GitHub fallisce (ad es. account quota esaurita) il fetch ESP32 paga il cold start ma resta entro 45s. Robustezza accettabile.

**Conseguenza per modifiche future:**
- Spostare il daily fetch da 07:00 ad altra ora richiede aggiornamento sincrono di:
  1. `CINEMA_DAILY_FETCH_HOUR` nel firmware (`shouldFetchCinema()` confronta `tm_hour` con questa costante).
  2. `WIFI_ACTIVE_HOUR_START`/`_END` (la nuova ora deve cadere dentro la finestra WiFi).
  3. cron in `keep-warm.yml` (entrambi gli entry, uno per CET e uno per CEST, 5 min prima).

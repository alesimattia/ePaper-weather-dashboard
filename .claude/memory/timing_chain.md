---
name: Catena di timeout fetch (WiFi → HTTP → OTA window)
description: Timeout incrociati firmware ↔ render.com, pre-warm dal firmware, budget worst-case del fetch cinema
type: project
---

I timeout sono distribuiti tra firmware e infra; vanno ragionati insieme:

**Firmware (`ePaper-weather-dashboard.ino`):**
- `WIFI_CONNECT_TIMEOUT_MS` = 15s, usato dal loop di attesa di `wifiOn()`. `BOOT_WIFI_TIMEOUT_MS` vale lo stesso ma misura il tempo dal boot per sbloccare il primo refresh: due costanti, valore uguale di proposito.
- `http.setTimeout(45000)` per il fetch cinema (45s).
- `CINEMA_PREWARM_TIMEOUT_MS` = 1500 ms: attesa della RISPOSTA del ping `/health`. Il timeout di connessione resta il default di HTTPClient (5s) e copre TCP + handshake TLS, senza il quale la richiesta non parte.
- `OTA_WINDOW_MIN = 3` → finestra OTA da 180s al boot.

**Render.com free tier:**
- Sleep dopo 15 min idle.
- Cold start **misurato 22,4s**, quasi tutto negli import di numpy/Pillow/lxml/FastAPI.
- Dopo il boot `/cinema/arduino` risponde in **0,5s**: le cache su disco (`/tmp/movieland_scraper`, `/tmp/posters`) sopravvivono al suspend, quindi a costare è il boot del processo, non la pipeline di scraping e rendering. È il motivo per cui pingare `/health` basta e non serve pingare l'endpoint reale.

**Pre-warm dal firmware (`prewarmCinemaServer()`):**
- Manda una GET a `CINEMA_PREWARM_URL` (`/health`) e ne abbandona la risposta: su TLS l'handshake va completato perché la richiesta raggiunga il router di render, ma i 22s di boot no. Esito nominale `HTTPC_ERROR_READ_TIMEOUT` (-11); un 200 rapido significa server già caldo.
- Costo ~2-3s, gatato dagli stessi predicati di `fetchCinemaImage()`: parte solo nei giri in cui l'immagine viene davvero scaricata. Senza quel gate render resterebbe sempre sveglio e brucerebbe le 750 h/mese di ore-istanza.
- Ordine di `runNetworkFetches()`: ping → meteo → mail → Google → Outlook → cinema. I fetch intermedi valgono 8-20s (tipico ~12s) e sono la copertura del boot; il cinema, ultimo, trova il server caldo o quasi.

**Worst case dedotto:**
- Attesa residua al fetch cinema: da ~0s (copertura sufficiente) a ~15s se il boot è lento e i fetch intermedi rapidi, contro i 22s pieni senza pre-warm.
- Boot freddo con pre-warm inefficace: 15s (WiFi) + fino a 22s (cold start HTTP) + ~2-3s (download 69 KB sul 097c, ~77 KB sul 122c) ≈ 40s di wall-clock.
- `http.setTimeout(45000)` copre solo la fase HTTP, non i 15s di WiFi: il margine regge il cold start misurato, e il pre-warm serve a ridurre l'attesa, non a rientrare nel timeout.
- OTA window 180s contiene anche il caso freddo (fetch cinema + meteo + 2 calendari + mail).

**Budget durante OTA:**
- `loop()` gira con `delay(10)` durante OTA window per `WebServer::handleClient()`. Un fetch cinema bloccante da 45s congela l'AP per 45s: utenti che provano `/update` durante quel periodo vedono timeout dal browser. Accettato perchè la finestra OTA è rara e l'utente se ne accorge. Il ping aggiunge ~2-3s allo stesso blocco.

**Conseguenza per modifiche future:**
- Spostare il daily fetch da 07:00 ad altra ora richiede aggiornamento sincrono di:
  1. `CINEMA_DAILY_FETCH_HOUR` nel firmware (`shouldFetchCinema()` confronta `tm_hour` con questa costante, e lo stesso predicato gatea il ping).
  2. `WIFI_ACTIVE_HOUR_START`/`_END` (la nuova ora deve cadere dentro la finestra WiFi).
- Non c'è più nessun scheduler esterno da tenere allineato: il pre-warm parte dal consumatore, quindi è immune a DST e a deriva di cron.

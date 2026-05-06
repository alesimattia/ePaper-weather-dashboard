---
name: ESP32 cinema consumer (fetchCinemaImage)
description: Logica del firmware ePaper-weather-dashboard.ino che scarica e mostra il background cinema dalla webapp
type: project
---

Lo sketch `c:\epd\ePaper-weather-dashboard.ino` contiene `fetchCinemaImage()` che scarica i 3 piani BWRY da `Layout::CINEMA_URL` (`cinema-epd.onrender.com/cinema/arduino?width=620&height=440&colors=bwry&dither=floyd` sul 097c, `height=536` sul 122c). Allocazione preferenziale in PSRAM (`psramFound() + heap_caps_malloc(MALLOC_CAP_SPIRAM)`), fallback heap interno con `malloc()`. I 3 buffer dinamici `g_cinema_black/red/yellow` (097c: 78×440=34320 byte ciascuno, 102960 totali; 122c: 78×536=41808 ciascuno, 125424 totali) rimpiazzano il fallback PROGMEM `img_apple_bwry_desc` solo dopo download riuscito.

Il puntatore `g_cinema_desc` viene swappato a `&g_cinema_dynamic_desc` solo all'ultimo step (post readBytes OK), garantendo che durante un fetch in corso `drawTestBackground()` continui a mostrare il fallback PROGMEM invece di buffer parziali.

**Trigger del fetch (vedi `shouldFetchCinema`):**
- Primo boot, una sola volta (`g_cinema_attempted`)
- Daily refresh alle ore `CINEMA_DAILY_FETCH_HOUR` locali (default 7, apertura finestra WiFi mattutina; condizione `tm_hour == CINEMA_DAILY_FETCH_HOUR` + `tm_yday != g_cinema_last_fetch_day`)

**Integrazione con il loop:** chiamato sia nel ramo OTA window sia nel ramo on-demand WiFi, sempre tra `Weather::runFetch()` e i fetch calendari (Outlook/Google), nella stessa finestra WiFi.

**Why:** L'utente vuole il risveglio render.com alla prima connessione utile della mattina, allineato all'apertura della finestra WiFi (`WIFI_ACTIVE_HOUR_START = 7`). La costante `CINEMA_DAILY_FETCH_HOUR` è tenuta separata da `WIFI_ACTIVE_HOUR_START` per poter spostare l'una senza toccare l'altra.

**How to apply:**
- L'URL e i parametri (`CINEMA_W`, `CINEMA_H`, `CINEMA_URL`, `CINEMA_*_SZ`) stanno in `Layout_097c.h` / `Layout_122c.h` (scelta esplicita: fuori da `Env.h`). Per cambiare display: scommentare il `#define DISPLAY_VARIANT_*` in testa al `.ino`. Nessuna modifica al `.ino` o ai moduli necessaria.
- Render.com free tier dorme dopo 15 min: c'è un workflow GitHub Actions `webapp/.github/workflows/keep-warm.yml` che pinga `/health` alle 06:55 CET/CEST per scaldare il server prima del fetch ESP32 alle 07:00.
- HTTP timeout = 45s (accomoda cold start render.com 10-30s).
- Se modifichi il formato del body lato webapp, aggiorna anche il parser nel `.ino`: oggi è `for (p = 0..2) readBytes(planes[p], Layout::CINEMA_PLANE_SZ)`.

---
name: ESP32 cinema consumer (fetchCinemaImage)
description: Logica del firmware ePaper-weather-dashboard.ino che scarica e mostra il background cinema dalla webapp
type: project
---

Convenzione dimensioni in questo file: `width=N&height=M` nell'URL = N px larghezza (X) × M px altezza (Y); `NwxMh` stessa convenzione.

Lo sketch `a:\epd\ePaper-weather-dashboard.ino` contiene `fetchCinemaImage()` che scarica i 3 piani BWRY da `Layout::CINEMA_URL` (`cinema-epd.onrender.com/cinema/arduino?width=620&height=300&colors=bwry&dither=floyd` sul 097c, `height=335` sul 122c → cioè 620w × 300h sul 097c e 620w × 335h sul 122c). L'altezza si ferma prima di `BANNER_Y` perchè la fascia y=CINEMA_H..BANNER_Y (160h px sul 097c, 221h px sul 122c) è occupata dalla UI mail di `Mail.h`. Allocazione preferenziale in PSRAM (`psramFound() + heap_caps_malloc(MALLOC_CAP_SPIRAM)`), fallback heap interno con `malloc()`. I 3 buffer dinamici `g_cinema_black/red/yellow` (097c: stride 78 × altezza 300 = 23400 byte ciascuno, 70200 totali; 122c: 78 × 335 = 26130 ciascuno, 78390 totali) rimpiazzano il fallback PROGMEM `img_apple_bwry_desc` solo dopo download riuscito.

Il terzo piano (`g_cinema_yellow`) viene scaricato, allocato e passato a `writeImageYellow()`, cioè al comando `0x28`: che quel canale produca colore sul pannello non è verificato, ed è la questione aperta di [[gxepd2_097c_driver]]. Se la sonda dicesse che non produce nulla, qui cadrebbero un buffer per variante, il suo scarico HTTP e il transfer SPI relativo.

Il puntatore `g_cinema_desc` viene swappato a `&g_cinema_dynamic_desc` solo all'ultimo step (post readBytes OK), garantendo che durante un fetch in corso `drawTestBackground()` continui a mostrare il fallback PROGMEM invece di buffer parziali.

**Trigger del fetch:** è il task `cinema` della tabella dello scheduler, con cadenza `GIORNALIERA` all'ora `cine_h` (default 7). Il primo boot è coperto da `maiEseguito`, il "una volta al giorno" da `ultimoGiorno` (tm_yday). Il confronto è `tm_hour >= cine_h` e non `==`: se all'ora prevista la radio era giù, si recupera alla prima occasione utile della stessa giornata. Senza orologio sincronizzato la cadenza giornaliera resta sospesa, tranne il primo tentativo.

**Integrazione:** è l'**ultimo** task di rete della tabella, e il suo `prima()` è `prewarmCinemaServer()`. Vedi [[scheduler_task_table]].

**Why:** L'utente vuole il risveglio render.com alla prima connessione utile della mattina, allineato all'apertura della finestra WiFi (`WIFI_ACTIVE_HOUR_START = 7`). La costante `CINEMA_DAILY_FETCH_HOUR` è tenuta separata da `WIFI_ACTIVE_HOUR_START` per poter spostare l'una senza toccare l'altra.

**How to apply:**
- L'URL e i parametri (`CINEMA_W`, `CINEMA_H`, `CINEMA_URL`, `CINEMA_*_SZ`) stanno in `Layout_097c.h` / `Layout_122c.h` (scelta esplicita: fuori da `Env.h`). Per cambiare display: scommentare il `#define DISPLAY_VARIANT_*` in testa al `.ino`. Nessuna modifica al `.ino` o ai moduli necessaria.
- Render.com free tier dorme dopo 15 min: `prewarmCinemaServer()` pinga `/health` (`CINEMA_PREWARM_URL`) all'inizio del giro e ne abbandona la risposta, così render fa boot mentre l'ESP32 esegue meteo, mail e calendari. Il cinema è l'ultimo task di rete proprio per incassare quella copertura.
- `CINEMA_HTTP_TIMEOUT_MS` = 45s (accomoda il cold start render.com, misurato 22,4s). Vedi [[timing_chain]].
- Se modifichi il formato del body lato webapp, aggiorna anche il parser nel `.ino`: oggi è `for (p = 0..2) readBytes(planes[p], Layout::CINEMA_PLANE_SZ)`.

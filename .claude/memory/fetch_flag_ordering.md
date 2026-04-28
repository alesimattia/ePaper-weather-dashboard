---
name: Ordering dei flag prima del fetch (vincolo retry-storm)
description: Perche' g_cinema_attempted e g_cinema_last_fetch_day vengono settati PRIMA delle alloc/HTTP
type: feedback
---

In `ePaper-weather-dashboard.ino:fetchCinemaImage()` i flag di stato sono aggiornati IMMEDIATAMENTE dopo il check WiFi, PRIMA di qualunque operazione che possa fallire (alloc PSRAM, HTTP begin/GET, readBytes):

```cpp
g_cinema_attempted = true;
{
    time_t now = time(nullptr);
    if (now > 100000L) {
        struct tm t; localtime_r(&now, &t);
        g_cinema_last_fetch_day = t.tm_yday;
    }
}
// ... poi alloc, HTTP, readBytes che possono tutti fallire
```

**Why (vincolo dedotto, non immediatamente visibile):**
- Durante la finestra OTA il `loop()` gira ogni ~10ms (`delay(10)` per `WebServer::handleClient()`).
- Se i flag fossero settati SOLO sul successo del fetch, un fallimento (HTTP 503 cold-start, OOM PSRAM, readBytes timeout) farebbe ripetere `shouldFetchCinema()→true` ogni 10ms → retry-storm verso render.com.
- Render.com free tier ha rate limiting; un device che hammera potrebbe finire bannato e perdere anche i fetch validi.

**Conseguenze pratiche (dal commento esplicito nel `.ino`):**
- Se il fetch fallisce, NON si ritenta nello stesso giorno (per il trigger daily) o nella stessa boot session (per il trigger primo-boot). Si mostra il fallback PROGMEM fino al prossimo trigger valido.
- Se il WiFi non si connette mai, i flag restano invariati (early-return prima del set): appena la radio sale, il tentativo parte normalmente.
- Daily refresh a `CINEMA_DAILY_FETCH_HOUR` (07:00 default): se il fetch fallisce alle 07:00 di lunedi', il prossimo tentativo e' alle 07:00 di martedi'.

**Trade-off:**
- Un singolo errore transiente (cold start render.com che timeoutta) costa una giornata di immagine vecchia.
- Accettato perche' la locandina cambia raramente e l'utente non se ne accorge se il fallback e' fresco di ieri.

**Errore da evitare:**
- NON spostare `g_cinema_attempted = true` in fondo alla funzione "per pulizia": romperebbe il vincolo anti-storm. Se il refactor lo richiede, alternativa sicura: introdurre `g_cinema_last_attempt_ms` con cooldown (es. 6h) anche su errore.

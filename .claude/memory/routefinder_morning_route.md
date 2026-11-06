---
name: Percorso del mattino (Routefinder.h + submodule routefinder)
description: Riquadro in alto a sinistra con il percorso al posto del cinema; sorgenti Auto/Cinema/Route/Wallpaper, fascia ai minuti, slot inattivo di CinemaStore condiviso, cinema mai scaricato in fascia, task route e source, freschezza, Env.h escluso dai test su host
metadata:
  type: project
---

Il riquadro `CINEMA_X/Y/W/H` mostra il percorso più veloce col traffico calcolato da `routefinder` (vedi [[routefinder_submodule]]) nella fascia percorso, il cinema fuori. Il body è nel formato di `/cinema/arduino` ([[arduino_binary_format]], [[cinema_blob_math]]): stessa misura, stessi due piani `bwr`, stesso `Content-Length` atteso.

**Impostazioni** in `Routefinder.h`, NVS namespace `routefinder` (chiavi `orig_lat`, `orig_lng`, `dest_lat`, `dest_lng`, `fuel`, `source`, `win_start`, `win_end`, `schema`): coordinate come int32 in microgradi, carburante (Benzina, Diesel, Ibrido, Elettrico → `gasoline`/`diesel`/`hybrid`/`electric`), sorgente (`Auto` = percorso in fascia e cinema fuori, `Cinema`, `Route`, `Wallpaper` = PROGMEM `la_grande_onda`), fascia in minuti del giorno con fine esclusa, default 420-540. Si modificano dalla pagina `/route` della manutenzione ([[maintenance_window]]); la cadenza `route_min` (10, pavimento 5) sta invece in `Timings` ([[timings_nvs_config]]). Default da `Env.h`: `ROUTEFINDER_ORIGIN_*` (ricade su `LAT`/`LON`), `ROUTEFINDER_DESTINATION_*` (0,0 = non configurato), `ROUTEFINDER_TOKEN` (header `X-Routefinder-Token`). I default float di `Env.h` si convertono arrotondando al quinto decimale: un letterale float non porta il sesto (43.6158f vale 43.61579895); il parser della pagina tiene sei decimali.

**Vincolo fra due namespace**: la fascia percorso deve stare dentro la fascia WiFi (`start >= wifi_h_ini*60`, `end <= (wifi_h_fin+1)*60`). Lo controllano `Routefinder::apply()`, la POST di `/config` (fascia WiFi che non la contiene più = rifiuto) e `Routefinder::begin()` (combinazione non valida = default). `/config/reset` non è intercettato: degrada da sè, perchè il task non accende la radio fuori fascia WiFi e il percorso invecchia.

**Flash condivisa con il cinema** ([[esp32_cinema_consumer]]): il percorso si scrive nello slot **inattivo** di `CinemaStore` con magic `RTE1` e l'epoch del download nel campo `sequence`; `commitWrite()` per il percorso non tocca `activeSlot`. Funziona solo perchè **in fascia il cinema non si scarica mai** (`cinemaReady()` = partizione presente e `!isRouteSourceActive()`): l'immagine cinema attiva resta intatta e torna a schermo a fine fascia senza download. Un download del cinema invalida il percorso nello slot che sovrascrive. `cine_h` dentro la fascia slitta alla sua fine grazie al recupero in giornata del DAILY. Nessuna modifica a `partitions.csv`.

**Task** ([[scheduler_task_table]]): `route` PERIODIC su `route_min`, precondizione `routeReady()` = sorgente percorso attiva + `Clock::valido()` + arrivo + token + archivio; prima del cinema, con `prewarmRouteServer()` come `onRoundStart`. `routeReady()` rimette `state.neverRun = true` all'ingresso nella sorgente percorso (effetto collaterale idempotente): il primo fetch della fascia parte subito senza dipendere dall'aritmetica signed di `millis()` sull'ultimo slot. `source` locale con `customMsUntilDue = msUntilSourceCheck()`: 0 se l'immagine da mostrare differisce da quella disegnata e nessuno ha chiesto il ridisegno, altrimenti il minimo fra prossimo bordo di fascia e scadenza di freschezza; sveglia il dispositivo esattamente ai bordi. `drawTestBackground()` registra l'immagine disegnata.

**Freschezza** (`Routefinder::shownImage()`): con `Auto` il percorso si mostra se ha meno di `ROUTE_MAX_AGE_CADENCES` (3) × `route_min`, con `Route` al massimo `ROUTE_FORCED_MAX_AGE_S` (1 h), poi il cinema. Motivi: un tempo stimato vecchio è un'informazione sbagliata e i termini Google vietano di conservare a lungo i contenuti della mappa.

**Downloader comune**: `downloadImageToStore(flow, url, token, timeoutMs, kind)` e `prewarmServer(flow, url)` nel `.ino` servono cinema e percorso; il tag di log è `img` col flusso nel testo, perchè `LOG` concatena il tag come letterale ([[log_macro]]). `ROUTE_HTTP_TIMEOUT_MS` = 45 s come il cinema.

**Test su host**: `test/test_routefinder.cpp` (parser, URL, NVS, fascia anche attraverso il DST, sorgenti, freschezza). `Routefinder.h` include `Env.h` solo senza `ROUTEFINDER_HOST_TEST`: con le virgolette l'include trova sempre l'`Env.h` vero accanto all'header, quindi uno stub in `test/stub` non lo oscurerebbe. Lo stub di `Preferences` ha mappe separate per `UShort`, `Int`, `UChar`.

**Why:** richiesta dell'utente: percorso con coordinate da `Env.h` sovrascrivibili in manutenzione, benzina di default, fascia 07:00-09:00 configurabile, radio button per forzare cinema, percorso o wallpaper, cinema mai scaricato in fascia.

**How to apply:** una modifica al formato o alla misura dell'immagine va fatta insieme in webapp, routefinder e firmware. Il testo HTML delle pagine di manutenzione resta ASCII perchè le pagine non dichiarano un charset.

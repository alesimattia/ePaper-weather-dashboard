---
name: ESP32 cinema consumer (fetchCinemaImage)
description: Logica del firmware ePaper-weather-dashboard.ino che scarica e mostra il background cinema dalla webapp
type: project
---

Convenzione dimensioni in questo file: `width=N&height=M` nell'URL = N px larghezza (X) × M px altezza (Y); `NwxMh` stessa convenzione.

Lo sketch `a:\epd\ePaper-weather-dashboard.ino` contiene `fetchCinemaImage()`, che scarica `Layout::CINEMA_PLANES` piani (black e accent, `colors=bwr` su entrambi i pannelli) da un URL composto con `snprintf` da `CINEMA_ORIGIN` e dalle costanti `Layout::CINEMA_W/H`: 610w × 290h sul 097c, 610w × 325h sul 122c. Un URL troncato ferma il fetch e lascia il fallback. L'immagine si disegna in (`CINEMA_X`, `CINEMA_Y`) = (10, 10), sopra il riquadro Posta. Aritmetica completa in [[cinema_blob_math]].

**Memoria.** Allocazione preferenziale in PSRAM (`psramFound()` + `heap_caps_malloc(MALLOC_CAP_SPIRAM)`), con ripiego sull'heap interno. Sulla board di bring-up (WROOM-32E, senza PSRAM) i piani stanno sempre nell'heap interno e ci restano tutto il giorno.

**Fallback.** `img_la_grande_onda_desc` (PROGMEM, B/N), che va generato a `CINEMA_W × CINEMA_H` con il convertitore, vedi [[fallback_progmem_uniformity]]. Il puntatore `g_cinema_desc` passa al descrittore dinamico solo dopo che tutti i piani sono stati letti; all'inizio di ogni fetch torna al fallback, quindi un fetch fallito mostra il fallback per il resto della giornata.

**Trasporto.** Client TLS esplicito preparato da `Net::beginHttps()` (handshake limitato a `TLS_HANDSHAKE_TIMEOUT_S`, HTTP/1.0), e controllo `getSize() == CINEMA_TOTAL_SZ`: il server risponde con `Content-Length` anche in HTTP/1.0 (verificato con curl). Con `http.begin(url)` senza client il core creerebbe un client interno senza modo di limitarne l'handshake.

**Trigger del fetch:** è il task `cinema` della tabella dello scheduler, con cadenza `GIORNALIERA` all'ora `cine_h` (default 7). Il primo boot è coperto da `maiEseguito`, il "una volta al giorno" da `ultimoGiorno` (tm_yday). Il confronto è `tm_hour >= cine_h` e non `==`: se all'ora prevista la radio era giù, si recupera alla prima occasione utile della stessa giornata. Senza orologio sincronizzato la cadenza giornaliera resta sospesa, tranne il primo tentativo.

**Integrazione:** è l'**ultimo** task di rete della tabella, e il suo `prima()` è `prewarmCinemaServer()`. Vedi [[scheduler_task_table]].

**Why:** l'utente vuole il risveglio render.com alla prima connessione utile della mattina, allineato all'apertura della finestra WiFi (`WIFI_ACTIVE_HOUR_START = 7`). La costante `CINEMA_DAILY_FETCH_HOUR` è tenuta separata da `WIFI_ACTIVE_HOUR_START` per poter spostare l'una senza toccare l'altra.

**How to apply:**
- Le misure (`CINEMA_*`) le deriva `Layout_common.h` dalla manopola `CINEMA_H` del pannello; l'URL non è scritto da nessuna parte, lo compone il `.ino`. Il dominio compare solo in `CINEMA_ORIGIN`, da cui derivano anche `CINEMA_PREWARM_URL` e il percorso dell'immagine. Per cambiare display si scambiano i `#define DISPLAY_VARIANT_*` in testa al `.ino`.
- Render.com free tier dorme dopo 15 min: `prewarmCinemaServer()` pinga `/health` (`CINEMA_PREWARM_URL`) all'inizio del giro e ne abbandona la risposta, così render fa boot mentre l'ESP32 esegue meteo, mail e calendari. Vedi [[timing_chain]].
- `CINEMA_HTTP_TIMEOUT_MS` = 45 s (accomoda il cold start render.com, misurato 22,4 s).
- Se cambia il formato del body lato webapp va aggiornato anche il parser nel `.ino`, che legge i piani in ordine con `readBytes` dentro un ciclo su `Layout::CINEMA_PLANES`.

**Lettura del corpo (verificata su sorgenti core 3.3.12 + misura dei record).**
- Catena core: `ssl_client.cpp` mette il socket `O_NONBLOCK` per tutta la sessione → `SO_RCVTIMEO` inerte; `NetworkClientSecure::read()` = -1 se `available() <= 0`; `NetworkClient::readBytes` attende solo su r == 0, su r < 0 esce → **readBytes su TLS torna subito alla prima pausa**, `Stream::setTimeout` non lo fa attendere. Il timeout dello stream lo imposta comunque `HTTPClient::connect()` (`_client->setTimeout(_tcpTimeout)`).
- Il corpo da Render/Cloudflare arriva a blocchi 4096/8192/16384/8056/10072 B separati da pause; record ≤ 1369 B in chiaro (ok per qualunque buffer mbedTLS). Senza attesa il piano si ferma a 4096 → log `piano black letto parzialmente (4096/22330)` in ~durata di un handshake, e il fallback PROGMEM lo maschera per tutto il giorno.
- Soluzione unica: `Net::BodyStream` (Net.h), Stream sopra `Client&` di `http.getStream()`. `waitRead`: `readBytes` del client; > 0 ritorna; `!connected()` (chiusura o errore TLS: `available()` del core fa `stop()`) o `getTimeout()` senza dati → 0; altrimenti `delay(1)` (1 tick, CONFIG_FREERTOS_HZ 1000, CPU al task idle). Deadline = timeout dello stream impostato da `HTTPClient::connect()` → 45 s (`http.setTimeout(CINEMA_HTTP_TIMEOUT_MS)`) nel cinema, 5 s default negli altri. Buffer 128 B per le letture piccole; letture ≥ 128 B direttamente nella destinazione (piani cinema senza doppia copia). `using Stream::readBytes` per l'overload `uint8_t*`.
- Utenti: cinema nel `.ino` (`body.readBytes(piano, CINEMA_PLANE_SZ)` per piano), `Weather.h` `httpGetJsonFiltered`, `Calendar.h` `oauthRefreshGeneric` / `fetchOutlookEvents` / `fetchGoogleEvents`. Mail usa `getString()` (`writeToStream` prosegue alle pause con Content-Length noto).
- Perchè anche il JSON: ArduinoJson 7.4.3 legge uno Stream con `readBytes(&c, 1)` e `Latch::load()` converte -1 in `'\0'` → una pausa fra record = `IncompleteInput`. Il buffer evita 2 `mbedtls_ssl_read` per carattere; un parsing più veloce raggiunge le pause più spesso, quindi buffer e attesa stanno nella stessa classe.
- Alternative scartate: `Stream::readBytes` base (byte per byte + `timedRead` che gira a vuoto); `http.writeToStream()`/`getString()` per i JSON (corpo intero in RAM — OWM decine di KB —, attesa `delay(0)`, nessuna guardia di tempo → può restare appeso); StreamUtils `ReadBufferingStream` (non installato e riempie con `readBytes` del client → stessa fine anticipata); socket riportato bloccante via `fcntl` (stato interno del core, renderebbe bloccanti anche `available()`/`connected()` usati da HTTPClient); `select()` sul fd (risparmia ≤ 1 ms per pausa, codice lwIP a basso livello). Il costo dell'attesa è trascurabile rispetto all'handshake (~2 s).
- `DebugLevel=none`: i `log_e` del core sono compilati via, quindi la loro assenza dal log non esclude un errore TLS.

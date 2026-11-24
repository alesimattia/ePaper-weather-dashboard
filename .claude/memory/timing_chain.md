---
name: Catena dei tempi (scheduler, sleep dinamico, pavimenti, tetti di rete)
description: Chi possiede il timing, sleep fino alla scadenza esatta del sensore, pavimenti di ogni flusso, tetti di tempo delle connessioni (handshake 15 s, HTTP/1.0 contro chunked), riconnessione WiFi veloce, notte con il solo sensore, light sleep sul BUSY, bilancio energetico
type: project
---

Tutti i tempi stanno in `Timings.h`; il **quando** lo decide `Scheduler.h`, mai i moduli. Vedi [[scheduler_task_table]] per la struttura della tabella e [[timings_nvs_config]] per gli override a runtime.

**Pavimenti, sotto i quali interrogare non produce informazione nuova:**

| Flusso | Pavimento | Natura |
|---|---|---|
| BME680 | 300 s (`BSEC_SAMPLE_RATE_ULP`) | hard: è un *modo* della libreria, non un intervallo. Gli altri sono LP 3 s e CONT 1 s |
| Meteo OWM | 10 min (cadenza dei dati); quota 1000/giorno = 1,44 min | soft sui dati, hard sulla quota |
| Google, Outlook | nessuno sui dati, ma pavimento 5 min = un ciclo | il costo è la radio: 5-10 s per fetch |
| Cinema | cache del server a fasce nel fuso di Movieland (un solo scraping 08:00-17:00, 30 min dalle 17:00 alle 20:55, un solo scraping 20:55-08:00); render dorme a 15 min e con lo sleep perde la cache; ETag/304 solo su `/cinema`, non su `/cinema/arduino` | hard lato server: `cine_min` ha default e pavimento 30, pari al rinnovo della fascia serale, e il cinema si scarica solo nella sua fascia |
| Display | 24 s (refresh pieno) o ~0,64 s più rampe (partial, solo se il rosso è invariato, vedi [[differential_refresh]]); un frame per ciclo in fascia, senza refresh se è identico a quello sul vetro ([[frame_skip_unchanged]]) | hard fisico |
| Radio | 2-15 s per accensione | costo |

**Sleep:** `Scheduler::sleepUntilNextEvent()` dorme esattamente fino alla scadenza del sensore (~301 s dall'ultimo poll), senza pavimento, perchè BSEC tollera solo il 6,25% di scarto; senza sensore fino al ciclo interno successivo o, di notte, all'apertura della fascia. `SLEEP_MAX_S` (24 h) è solo una guardia. Un pavimento di 30 s con tetto a 300 s porterebbe il poll a ~330 s, il 10% di ritardo, con due risvegli per ciclo.

## Tetti di tempo delle connessioni

I produttori girano in sequenza nello stesso `loop()`, quindi il tempo che uno consuma ritarda tutti gli altri. Ogni connessione HTTPS passa da `Net.h`:
- `Net::prepareTlsClient()` mette `setInsecure()` e `setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S)` (15 s, l'argomento è in secondi). Il default del core 3.3.11 è **120 s** (`NetworkClientSecure.cpp`), e il ciclo di `ssl_starttls_handshake()` continua su `WANT_READ` fino a quel tetto: un server che accetta il TCP senza completare il TLS fermerebbe tutto per due minuti.
- La connessione TCP ha già il suo tetto, `_connectTimeout` di HTTPClient (5 s), e ogni lettura `_tcpTimeout` (5 s). Il DNS è limitato da lwIP (4 ritenti, timer 1 s). Resta senza tetto totale solo una risposta "a gocce", un byte ogni meno di 5 s.
- `Net::beginHttps()` aggiunge `useHTTP10(true)`. Misurato con curl: in HTTP/1.1 `oauth2.googleapis.com`, `www.googleapis.com` e `graph.microsoft.com` rispondono `Transfer-Encoding: chunked`, mentre OpenWeather, il token Microsoft e `/cinema/arduino` usano `Content-Length`. `deserializeJson()` legge lo stream grezzo (via `Net::BodyStream`, che non decodifica il chunked) e con il chunked fallisce sempre. In HTTP/1.0 tutti e sette gli endpoint rispondono senza chunked e chiudono la connessione; `/cinema/arduino` manda `Content-Length` anche lì.
- Costo stimato dell'handshake a 80 MHz: 1-2 s con X25519 (Google, OpenWeather, render.com), 2-4 s con P-384 (Microsoft impone quella curva). Lato ESP32 il core offre solo TLS 1.2, accelera MPI, AES e SHA ma non le curve ellittiche.

**Sonda e preparazione di cinema e percorso** (`wakeOrPrepare()`, `onRoundStart()`): girano prima di tutti gli altri fetch e usano anch'esse il tetto di 15 s dell'handshake, che non tocca lo spin-up di render.com (riguarda la **risposta**; l'handshake lo chiude l'edge di render anche a istanza ferma). Spin-up del free tier ~1 min (render.com/docs/free; misurato una volta 22,4 s), file system perso a ogni spin-down. Lo spin-up non si attende MAI dentro un'accensione: sonda `/health`, e con l'istanza in avvio il download si rinvia al ciclo dopo (`Outcome::DEFERRED`); a istanza su la preparazione avvia la generazione e il download attende al più `CINEMA_HTTP_TIMEOUT_MS` / `ROUTE_HTTP_TIMEOUT_MS` = `uint16_t{30000}`. A server caldo `/cinema/arduino` risponde in 0,54 s; a cache vuota la pipeline misura 1,5-9,1 s. Dettaglio in [[esp32_cinema_consumer]].

**Worst case di un'immagine:** server addormentato = immagine al ciclo dopo (~301 s), con solo ~3,4 s di sonda nel passo; a istanza su 15 s (WiFi) + ~3,4 s (preparazione) + fino a 30 s (generazione e download). Se la generazione supera i 30 s, l'app la finisce comunque e il ritento al ciclo dopo la trova in cache.

## Radio e pannello

- **Riconnessione WiFi veloce.** `wifiOn()` ricorda in RAM canale e BSSID dell'ultima connessione e li passa a `WiFi.begin()`. Senza, il core scandisce i canali dal primo (`WIFI_FAST_SCAN`) finchè trova la rete: il risparmio cresce col numero del canale, da circa 0 a 1,5 s. Se il tentativo veloce non riesce entro `WIFI_FAST_CONNECT_TIMEOUT_MS` (5 s) si ripiega sulla connessione normale nel resto di `WIFI_CONNECT_TIMEOUT_MS`. Il canale compare nel log di connessione.
- **Notte.** Fuori dalla fascia WiFi il ciclo fa solo il passo del sensore: niente radio, Tuya, frame o SNTP, 12 risvegli brevi all'ora, il minimo che conserva l'accuratezza di BSEC. Con l'ora valida al boot (riavvio software) la finestra di manutenzione non si apre. Il Bluetooth è sempre spento: il `.ino` definisce `btClassicInUse()`/`bleInUse()` a `false`, e `initArduino()` rilascia la memoria del controller. Fra mezzanotte e l'apertura della fascia il pannello mostra la data del giorno prima.
- **Light sleep durante il refresh.** `initDisplay()` registra `setBusyCallback()` di GxEPD2: a radio spenta l'attesa del BUSY (24 s per refresh) avviene in light sleep, con risveglio sul BUSY basso (`Layout::BUSY_WAKE_PIN`) e un timer di sicurezza `DISPLAY_BUSY_SLEEP_SLICE_MS`. Il timeout del driver resta valido perchè `micros()` avanza durante il light sleep. A radio accesa, nella finestra di manutenzione, resta l'attesa attiva. Dopo il risveglio va tolta la sorgente GPIO, altrimenti il BUSY basso sveglierebbe subito ogni sleep dello scheduler.

**Bilancio in fascia, per ora (stime):** 12 cicli, ciascuno con un risveglio, una finestra radio (Tuya ogni ciclo, meteo e calendari un ciclo sì e uno no, percorso a ogni ciclo della sua fascia, cinema ogni 30 minuti della sua) e un frame, che va al pannello come refresh pieno o partial secondo il rosso, a meno che sia identico a quello sul vetro ([[frame_skip_unchanged]]). La leva residua è `TUYA_REQUEST_ACK` a 0 dopo il bring-up, che toglie fino a 4 s di radio a publish; i frame li decide la sola fascia WiFi (192 al giorno con 7-23), i refresh sono quelli dei frame non invariati.

**Durante la finestra di manutenzione** `loop()` gira con `delay(10)`, lo scheduler valuta il giro al più ogni `MAINTENANCE_SCHEDULER_PERIOD_MS` (1 s) e aspetta finchè un client usa le pagine (vedi [[maintenance_window]]), ma il passo del sensore gira lo stesso con `Scheduler::serviceSensor()`. Un fetch già partito blocca comunque il web server per la sua durata.

**Ogni confronto sull'ora locale di questa catena** (fascia WiFi, fasce di percorso e cinema) vale solo con l'orologio sincronizzato: vedi [[clock_sntp]].

**Per spostare il cinema** si cambia la sua fascia da `/route` (o i default `CINEMA_WINDOW_*` in `Routefinder.h`): `static_assert` e validazione a runtime la tengono dentro la fascia WiFi e separata da quella del percorso ([[routefinder_morning_route]]). Vedi [[github_cron_inaffidabile]] per perchè il pre-warm sta nel firmware.

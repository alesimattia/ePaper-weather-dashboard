---
name: Catena dei tempi (scheduler, sleep dinamico, pavimenti, tetti di rete)
description: Chi possiede il timing, come si calcola il risveglio, pavimenti di ogni flusso, tetti di tempo delle connessioni (handshake 15 s, HTTP/1.0 contro chunked), riconnessione WiFi veloce, ridisegno notturno, light sleep sul BUSY, bilancio energetico
type: project
---

Tutti i tempi stanno in `Timings.h`; il **quando** lo decide `Scheduler.h`, mai i moduli. Vedi [[scheduler_task_table]] per la struttura della tabella e [[timings_nvs_config]] per gli override a runtime.

**Pavimenti, sotto i quali interrogare non produce informazione nuova:**

| Flusso | Pavimento | Natura |
|---|---|---|
| BME680 | 300 s (`BSEC_SAMPLE_RATE_ULP`) | hard: è un *modo* della libreria, non un intervallo. Gli altri sono LP 3 s e CONT 1 s |
| Meteo OWM | 10 min (cadenza dei dati); quota 1000/giorno = 1,44 min | soft sui dati, hard sulla quota |
| Mail, Google, Outlook | nessuno sui dati | il costo è la radio: 5-10 s per fetch |
| Cinema | `CACHE_TTL_SECONDS=3600` in produzione; render dorme a 15 min; ETag/304 | hard lato server. Il palinsesto cambia a giorni, quindi cadenza **giornaliera** |
| Display | 24 s (refresh pieno) | hard fisico |
| Radio | 2-15 s per accensione | costo |

**Sleep dinamico:** `Scheduler::dormi()` dorme fino al minimo delle scadenze effettive, clampato in `[SLEEP_MIN_S 30, SLEEP_MAX_S 300]`. Il tetto **è** il periodo ULP.

## Tetti di tempo delle connessioni

I produttori girano in sequenza nello stesso `loop()`, quindi il tempo che uno consuma ritarda tutti gli altri. Ogni connessione HTTPS passa da `Net.h`:
- `Net::prepareTlsClient()` mette `setInsecure()` e `setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S)` (15 s, l'argomento è in secondi). Il default del core 3.3.11 è **120 s** (`NetworkClientSecure.cpp`), e il ciclo di `ssl_starttls_handshake()` continua su `WANT_READ` fino a quel tetto: un server che accetta il TCP senza completare il TLS fermerebbe tutto per due minuti.
- La connessione TCP ha già il suo tetto, `_connectTimeout` di HTTPClient (5 s), e ogni lettura `_tcpTimeout` (5 s). Il DNS è limitato da lwIP (4 ritenti, timer 1 s). Resta senza tetto totale solo una risposta "a gocce", un byte ogni meno di 5 s.
- `Net::beginHttps()` aggiunge `useHTTP10(true)`. Misurato con curl: in HTTP/1.1 `oauth2.googleapis.com`, `www.googleapis.com`, `gmail.googleapis.com` e `graph.microsoft.com` rispondono `Transfer-Encoding: chunked`, mentre OpenWeather, il token Microsoft e `/cinema/arduino` usano `Content-Length`. `deserializeJson()` legge lo stream grezzo (via `Net::BodyStream`, che non decodifica il chunked) e con il chunked fallisce sempre. In HTTP/1.0 tutti e sette gli endpoint rispondono senza chunked e chiudono la connessione; `/cinema/arduino` manda `Content-Length` anche lì.
- Mail è l'eccezione: resta in HTTP/1.1 per riusare la connessione fra lista e batch e legge le risposte con `getString()`, che gestisce il chunked (vedi [[mail_module]]).
- Costo stimato dell'handshake a 80 MHz: 1-2 s con X25519 (Google, OpenWeather, render.com), 2-4 s con P-384 (Microsoft impone quella curva). Lato ESP32 il core offre solo TLS 1.2, accelera MPI, AES e SHA ma non le curve ellittiche.

**Ping del cinema.** È un `prima()`, quindi gira prima di tutti gli altri fetch: per questo usa anch'esso il tetto di 15 s. Il tetto non tocca il cold start di render.com, che riguarda la **risposta** (misurato 22,4 s, coperto da `CINEMA_HTTP_TIMEOUT_MS` 45 s), non l'handshake, che chiude l'edge di render anche a istanza ferma. Dopo il boot `/cinema/arduino` risponde in 0,54 s.

**Worst case del cinema:** 15 s (WiFi) + fino a 22 s (cold start) + 2-3 s (download) ≈ 40 s, dentro i 45 s.

## Radio e pannello

- **Riconnessione WiFi veloce.** `wifiOn()` ricorda in RAM canale e BSSID dell'ultima connessione e li passa a `WiFi.begin()`. Senza, il core scandisce i canali dal primo (`WIFI_FAST_SCAN`) finchè trova la rete: il risparmio cresce col numero del canale, da circa 0 a 1,5 s. Se il tentativo veloce non riesce entro `WIFI_FAST_CONNECT_TIMEOUT_MS` (5 s) si ripiega sulla connessione normale nel resto di `WIFI_CONNECT_TIMEOUT_MS`. Il canale compare nel log di connessione.
- **Ridisegno notturno.** Il campione del sensore chiede il ridisegno solo dentro la fascia WiFi (`dopoIndoor()` con `Scheduler::inFascia()`): di notte non c'è nè radio nè refresh, restano 12 risvegli brevi all'ora per il sensore. Fra mezzanotte e l'apertura della fascia il pannello mostra la data del giorno prima.
- **Light sleep durante il refresh.** `initDisplay()` registra `setBusyCallback()` di GxEPD2: a radio spenta l'attesa del BUSY (24 s per refresh) avviene in light sleep, con risveglio sul BUSY basso (`Layout::BUSY_WAKE_PIN`) e un timer di sicurezza `DISPLAY_BUSY_SLEEP_SLICE_MS`. Il timeout del driver resta valido perchè `micros()` avanza durante il light sleep. A radio accesa, nella finestra di manutenzione, resta l'attesa attiva. Dopo il risveglio va tolta la sorgente GPIO, altrimenti il BUSY basso sveglierebbe subito ogni sleep dello scheduler.

**Bilancio in fascia, per ora (stime):** circa 12 finestre radio (6 complete ogni 10 minuti più 6 per il solo Tuya a 5 minuti), 100-130 s di radio, 12 refresh del pannello. Le leve residue sono di configurazione: `TUYA_REQUEST_ACK` a 0 dopo il bring-up toglie fino a 4 s di radio a publish, `disp_min` a 10 dimezza i refresh.

**Durante la finestra di manutenzione** `loop()` gira con `delay(10)` e lo scheduler aspetta finchè un client usa le pagine (vedi [[maintenance_window]]). Un fetch già partito blocca comunque il web server per la sua durata.

**Ogni confronto su `tm_hour` di questa catena** (giornaliera del cinema, fascia WiFi) vale solo con l'orologio sincronizzato: vedi [[clock_sntp]].

**Se sposti il fetch giornaliero del cinema** basta cambiare `cine_h` (da `/config` o in `Timings.h`): una `static_assert` e il controllo a runtime impongono che resti dentro la fascia `wifi_h_ini..wifi_h_fin`. Vedi [[github_cron_inaffidabile]] per perchè il pre-warm sta nel firmware.

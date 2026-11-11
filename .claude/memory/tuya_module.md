---
name: Modulo Tuya (TuyaLink su MQTT)
description: Tuya.h pubblica il BME680 su TuyaLink con un client MQTT minimale su esp-tls; la connessione resta aperta dopo il PUBACK e si rilascia a radio spenta perchè Tuya segna offline sulla chiusura; formato accettato dal cloud, device model, catena TLS, diagnostica dai Device Logs, task dello scheduler, badge TUYA ERR
type: project
---

`Tuya.h` (header-only, namespace `Tuya` + `Tuya::detail`) pubblica le cinque
grandezze di `Indoor::sample()` sul cloud Tuya via **TuyaLink**, il canale MQTT
che Tuya offre per hardware non suo. Il dispositivo si associa all'app Android
Smart Life scansionando il QR code generato dalla console; il firmware non fa
nessun pairing.

API: `begin()`, `readyToPublish()` (pura), `runPublish()`, `releaseSession()`,
`hasFailed()`. Una sessione MQTT per messaggio: connect, publish QoS 1, PUBACK;
la connessione resta poi aperta fino a `releaseSession()`.

## Posto nell'architettura

Il modulo non possiede nessun tempo: è una riga della tabella dei task del
`.ino` (vedi [[scheduler_task_table]]), con gli adattatori `runTuyaPublish`,
`tuyaReady` e `onTuyaOutcome`. Conseguenze da tenere a mente:

- `readyToPublish()` è l'hook `preconditionsMet()`: credenziali presenti (riconosciute
  con `Net::isCredentialSet()`, lo stesso criterio degli altri produttori),
  campione BSEC valido **e orologio sincronizzato**. Il campione scade al
  secondo campione perso (vedi [[bsec_modi_campionamento]]), quindi con il
  sensore guasto Tuya si sospende invece di ripubblicare un valore vecchio. Falso
  sospende il task e lo esclude dal calcolo del prossimo risveglio, e soprattutto
  **non consuma lo slot**: con un solo tentativo per cadenza, uno slot speso senza
  poter pubblicare sarebbe perso fino alla cadenza piena. La condizione
  sull'orologio conta al boot, dove la finestra di manutenzione non chiama
  `wifiOn()` e quindi non sincronizza: senza quella guardia il primo giro
  segnalerebbe un guasto Tuya che è solo un'ora non ancora pronta, pagandolo con
  due refresh pieni.
- **`maxAttempts` a 0**, unico task del firmware: un tentativo per slot
  comunque vada. I modi di fallimento di Tuya sono permanenti sulla scala dei
  minuti (credenziali, region, device model) e ogni ritento costerebbe un altro
  handshake TLS senza cambiare l'esito.
- **Segue il sensore**: nel ciclo dello scheduler il publish viene dopo il passo
  del sensore, quindi pubblica il campione appena letto. Fuori dalla fascia WiFi non
  si pubblica mai: di notte la radio resta spenta e nell'app resta l'ultimo dato
  della sera.
- Radio giù non alza il guasto: lo scheduler registra `SKIPPED` senza chiamare
  `runPublish()`, quindi lo slot resta intatto e il badge non compare.
- **`wifiOff()` del `.ino` chiama `Tuya::releaseSession()`** subito dopo
  `WiFi.mode(WIFI_OFF)`: è l'unico punto in cui lo scheduler spegne la radio, e
  copre anche la guardia "radio ancora accesa" prima del light sleep.

Cadenza in `Timings.h` come `tuya_min`, quindi modificabile da `/config` con
persistenza NVS. **Il pavimento è `BSEC_PERIODO_ULP_S / 60`**, cioè 5 min:
sotto, la cache di Indoor non ha un campione nuovo da dare e si ripubblica lo
stesso. **Il tetto utile è sotto i 30 minuti**: oltre, il dispositivo risulta
offline fra un publish e il successivo (static_assert sul default).
`TUYA_CONNECT_TIMEOUT_MS` / `TUYA_PUBACK_TIMEOUT_MS` / `TUYA_ACK_TIMEOUT_MS` /
`TUYA_KEEPALIVE_S` stanno in `Timings.h`; in `Tuya.h` restano solo i valori che
non sono tempi (URI del broker, identifier dei DP, `TUYA_REQUEST_ACK`).
`TUYA_PUBACK_TIMEOUT_MS` vale anche come attesa del resto di un pacchetto
iniziato.

L'ora valida la dice `Clock::valido()`, non una copia locale della soglia: il
modulo include `Clock.h` e non `Scheduler.h`, perchè la dipendenza fra moduli e
scheduler va in un verso solo.

## Stato online: la connessione non si chiude con la radio accesa

**Tuya segna offline il dispositivo nell'istante in cui la connessione si chiude**,
e l'app Smart Life nasconde i valori di un dispositivo offline dietro un overlay.
Misurato nei Device Logs della console (eu.platform.tuya.com, Device Logs del
device, con evento, motivo e payload di ogni report):

| Chiusura | Evento nei log |
|---|---|
| DISCONNECT MQTT | `Offline`, motivo `client_disconnect`, subito |
| chiusura TCP subito dopo il publish, senza DISCONNECT | `Offline`, motivo `client_closed`, circa 0,5 s |
| connessione aperta e muta per 8 minuti, keepalive 1200 | nessun `Offline`, app senza overlay |

La chiusura TCP dopo gli 8 minuti di silenzio non ha generato nessun evento: la
ragione non è chiarita. Un dispositivo che mantiene la connessione aperta e muta
resta online per **1,5 volte il keepalive dall'ultimo traffico** (keepalive
ammesso 30..1200 s). Una nuova connessione con lo stesso client id sostituisce
la precedente.

Conseguenze di design:

- Dopo il PUBACK `runPublish()` **lascia la connessione aperta**, senza PINGREQ e con
  keepalive 1200 dichiarato nel CONNECT; su ogni fallimento la chiude subito (il
  dispositivo non ha pubblicato comunque).
- `releaseSession()` la rilascia **a radio già spenta**, quando nessun FIN può uscire:
  il broker vede solo silenzio e il dispositivo resta online fino al publish
  successivo (30 minuti con 1200 s). Prima di ogni light sleep la connessione è già
  rilasciata.
- Una connessione rimasta aperta (radio mai spenta dallo scheduler, come nella
  finestra di manutenzione) si chiude all'inizio del `runPublish()` successivo.
- **Costo**: una sessione TLS (circa 40 KB di heap) resta occupata durante i task che
  seguono Tuya nella tabella (percorso e cinema, che fanno TLS a loro volta). Il log
  `[Tuya] connessione tenuta aperta ... heap libero` lo riporta a ogni publish. Se
  l'heap non bastasse, il rimedio è spostare Tuya in fondo alla tabella, perdendo la
  copertura del boot di render.com che l'ordine attuale dà.
- Di notte, fuori dalla fascia WiFi, il dispositivo risulta offline dopo 30 minuti:
  atteso.
- **Non verificato (solo sul dispositivo)**: che lo spegnimento della radio non faccia
  uscire un FIN o un RST dallo stack lwIP (distruzione dell'interfaccia con socket
  aperti). Si controlla nei Device Logs: dopo ogni `Online` non deve comparire un
  `Offline` con motivo `client_closed`.
- `esp_tls_conn_destroy()` non invia nessun close_notify (`libesp-tls.a` non riferisce
  `mbedtls_ssl_close_notify`): è solo il FIN TCP.

**Perchè il client non è esp-mqtt**: la connessione deve restare aperta senza un task
che la serva, e `esp_mqtt_client_stop()` invia un DISCONNECT quando il client è
connesso (lo stato `MQTT_STATE_CONNECTED` lo decide, nessuna API pubblica lo evita;
`esp_mqtt_abort_connection` è statica). Chiudere il trasporto dall'event handler
lascerebbe il task a leggere un socket chiuso (`FD_SET(-1)` nel `select` del
transport). Il core è IDF 5.5.5, esp-mqtt in `0e9ec17`.

## Protocollo TuyaLink, formule verificate

Confermate dalla doc Tuya, dal componente ESPHome `hzkincony/esphome-tuya-iot`, dall'SDK
ufficiale `tuya/tuya-iot-core-sdk` (`src/tuyalink_core.c`) e dal broker reale
(CONNACK 0 e `report_response` con `code` 0).

```
broker    mqtts://m1.tuyaeu.com:8883        (EU centrale; us / ueaz.us / weaz.eu / cn / in)
clientId  tuyalink_{deviceId}
username  {deviceId}|signMethod=hmacSha256,timestamp={ts10},secureMode=1,accessType=1
password  hex_lower(HMAC_SHA256(key=deviceSecret,
                    msg="deviceId={deviceId},timestamp={ts10},secureMode=1,accessType=1"))
topic     tylink/{deviceId}/thing/property/report
risposta  tylink/{deviceId}/thing/property/report_response
modello   tylink/{deviceId}/thing/model/get  ->  .../model/get_response
```

Vincoli che non si vedono dal codice:

- **MQTT 3.1.1 obbligatorio**: 3.1 e 5 rifiutati. QoS 0 e 1, QoS 2 non supportato.
- CONNECT con flag `0xC2` (username, password, sessione pulita).
- `ts10` in **secondi, 10 cifre**, e deve essere **identico** in username e nella
  stringa firmata: due `time(nullptr)` separati sbagliano a cavallo del secondo,
  producendo un fallimento intermittente.
- Ordine e presenza dei campi `secureMode=1,accessType=1` sono parte della firma:
  non riordinabili.
- Serve una licenza per device (authorization code gratuiti in sviluppo).

Il **device model** si legge dal dispositivo con `thing/model/get` e payload
`{"msgId":..,"time":..,"data":{"format":"complete"}}`: la risposta porta, per ogni
property, `code`, `abilityId`, `accessMode` e `typeSpec` (tipo, min, max, step, scale).
È il modo di verificare identifier e tipi senza passare dalla console.

## Payload e device model

```json
{"msgId":"7","time":1757246400,"data":{"temp_value":{"value":23.4},"hum_value":{"value":47},"press_value":{"value":1013},"iaq_value":{"value":48},"iaq_acc":{"value":2}},"sys":{"ack":1}}
```

- **`data` vuole oggetti con `value`**, non il valore piatto (il componente ESPHome
  scrive la forma piatta: non conforme, e il fallimento è silenzioso).
- **Il `time` per property è omesso e il cloud accetta lo stesso** (`code` 0): la doc lo
  marca "Required" e l'SDK lo manda, ma il messaggio senza passa; passano anche le forme
  con `time` per property in secondi e in millisecondi. Il `time` di messaggio vale per
  tutte le property e accetta 10 o 13 cifre; il firmware usa i secondi.
- **`msgId` è una stringa di max 32 caratteri**; l'SDK usa un contatore incrementale.
- L'SDK pubblica in QoS 0. Il modulo usa QoS 1: il PUBACK è l'unico segnale di
  consegna disponibile prima di tornare a dormire.
- Il `time` del messaggio è l'epoch del campione (`millis() - lastUpdateMs`), non l'istante
  del publish: il task del sensore gira a valle dei task di rete, quindi la cache
  contiene il campione del giro precedente, vecchio fino a cinque minuti.

**Device model della console** (letto con `thing/model/get`): tutte le property sono
`ro`. Gli identifier coincidono con i `TUYA_DP_*`.

| Campo di Sample | Tipo C | Identifier | Tipo nel modello | Range |
|---|---|---|---|---|
| `temperature` | `float` | `temp_value` | `float`, step 1.0 | -25..60 °C |
| `humidity` | `uint8_t` | `hum_value` | `value`, scale 0, step 1 | 0..100 % |
| `pressure` | `uint16_t` | `press_value` | `value`, scale 0, step 1 | 0..1500 mBar |
| `iaq` | `uint16_t` | `iaq_value` | `value`, scale 0, step 1 | 0..500 |
| `iaqAccuracy` | `uint8_t` | `iaq_acc` | `value`, scale 0, step 1 | 0..5 |

Il cloud accetta `22.5` su `temp_value` malgrado lo step 1.0. Il catalogo dei tipi della
console elenca nove tipi senza Float (value, string, date, boolean, enum, raw, struct,
array, fault) e l'SDK dell'app TuyaLink mappa sui DP solo value, string, date, bool,
enum, bitmap, array, struct; la Cloud API invece definisce `float` e `double`. Per un
decimale la forma portabile è Value con scale 1 e valore intero già scalato (23,4
diventa 234, a cura del dispositivo); il firmware non la usa perchè il modello ha il
`float`. La scale va tenuta a zero sulle quattro Value: una scale diversa scala il valore
nell'app senza che niente segnali l'errore.

## Il fallimento silenzioso, e come si diagnostica

**Il PUBACK conferma la consegna MQTT, non l'accettazione dei datapoint.** Con un
identifier fuori dal modello (2003), un valore fuori dai limiti o una scale non
combaciante il publish risulta riuscito e i valori non compaiono nell'app.

Il `report_response` **non arriva di default**: richiede `"sys":{"ack":1}`. Il modulo lo
implementa sotto `TUYA_REQUEST_ACK` (default 0 in `Tuya.h`; il `.ino` lo alza a 1 per il
bring-up e va riabbassato a regime, perchè costa fino a 4 s di radio a publish): a 1
aggiunge il campo, sottoscrive il topic di risposta attendendo il SUBACK prima di
pubblicare, e logga il `code` o il corpo intero in caso di rifiuto. Codici: 0 ok, 1003
formato dati, 1004 device inesistente, 2002 modello non definito, 2003 property non
definita, 2006 verifica dati fallita. Un rifiuto **non alza il badge**: l'esito di
`runPublish()` resta quello del PUBACK.

Strumenti di diagnosi: i **Device Logs** della console mostrano ogni report (payload,
risultato, valori per property) e gli eventi Online/Offline con il motivo; "Online
Debugging" e "Device Debugging" funzionano solo con il device online, quindi con
sessioni brevi non servono. Senza dispositivo si prova tutto dal PC con un client MQTT
minimale su `ssl` (connect, model/get, publish con sys.ack): il device model e il
`report_response` sono la verità.

## TLS: la catena del broker e il bundle del core

La catena presentata da `m1.tuyaeu.com:8883` cambia: sono state osservate catene
DigiCert (RapidSSL) e GoDaddy. La corrente è leaf `*.tuyaeu.com` → GoDaddy TLS
Intermediate CA DV R1v1 → GoDaddy TLS Root CA R1 → Go Daddy Root Certificate Authority G2
(autofirmata), con verifica piena riuscita. Il SAN wildcard copre `m1.tuyaeu.com`, quindi
**non serve `skip_cert_common_name_check`**.

Il bundle del core (`libmbedtls.a`, `esp_crt_bundle_attach`, 112 root CA) contiene Go
Daddy Root G2, DigiCert Global Root G2 e Starfield Root G2: per questo il modulo usa il
bundle e non un PEM, che andrebbe rifatto a ogni rotazione di CA. A differenza degli
altri moduli, che fanno `setInsecure()`, qui la verifica va fatta: su questa connessione
transita l'HMAC del DeviceSecret.

## Cosa offre il core, e i gotcha di esp-tls

Zero librerie da installare, tutto in `esp32-libs/3.3.11`: `esp-tls` per il trasporto,
`mbedtls/md.h` per l'HMAC (`mbedtls_md_hmac()` one-shot), `lwip/sockets.h` per `select()`.

- `esp_tls_conn_new_sync()` ritorna 1 o -1; su -1 l'handle va comunque distrutto con
  `esp_tls_conn_destroy()`, e il motivo si legge con `esp_tls_get_error_handle()`.
- `cfg.timeout_ms` limita la connessione; i timeout di lettura li fa il modulo con
  `select()` sul socket (`esp_tls_get_conn_sockfd()`), dopo aver controllato
  `esp_tls_get_bytes_avail()`: i byte già decifrati da mbedtls non si vedono dal socket.
- `esp_tls_conn_read/write` ritornano `ESP_TLS_ERR_SSL_WANT_READ/WRITE` da riprovare;
  0 in lettura è connessione chiusa.
- L'handshake gira nello stack del task Arduino, come quello dei `WiFiClientSecure`
  degli altri moduli, e costa circa 32 KB di heap per sessione
  (`CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384`, asymmetric non attivo).
- Un pacchetto in ingresso più grande del buffer di ricezione (384 byte) si legge e si
  scarta, per restare allineati ai pacchetti successivi; il resto di un pacchetto
  iniziato che non arriva fa considerare persa la sessione.

## Decisioni di design non ovvie

1. **`failedLast` pessimista**: alzato prima di qualunque operazione fallibile e
   azzerato **solo** dal PUBACK, così ogni uscita anticipata lascia il guasto
   segnalato anche se in futuro si aggiunge un `return` in mezzo alla sequenza.
   Non è la politica dello slot, che è dello scheduler: è solo lo stato del badge.
2. **`snprintf` e non ArduinoJson** per il payload: schema fisso noto a compile
   time, niente da parsare, e un `JsonDocument` allocherebbe nel momento di
   massima pressione di heap. Il campo `sys` è concatenato al format come macro,
   così la composizione resta un unico `snprintf` senza rami.
3. **Posizione in tabella: prima del cinema.** I secondi dell'handshake TLS diventano
   altro tempo di copertura del boot di render.com, e il cinema resta l'ultimo; con la
   connessione tenuta aperta è anche la posizione che sovrappone una sessione TLS
   inattiva ai fetch di percorso e cinema.
4. **Guardia difensiva su `s.valid` dentro `runPublish()`**, benchè `readyToPublish()` la
   copra già: pubblicare senza campione manderebbe cinque zeri al cloud,
   indistinguibili nell'app da una misura vera.
5. **Buffer statici** (trasmissione da 512 byte con intestazione scritta a ritroso, corpo
   di ricezione da 384): niente stack nè heap per i pacchetti, e un pacchetto parte con
   una sola scrittura.

## Badge TUYA ERR sul pannello

`Weather::detail::drawIndoorTuyaBadge()` disegna `TUYA ERR` in FONT_SMALL rosso,
seduto sul bordo inferiore del riquadro Interno e allineato al suo margine destro
(con la striscia bianca che interrompe il bordo), quando `Tuya::hasFailed()`. **Tuya
non comanda il refresh**: il badge si legge al momento del disegno, e il frame si
disegna comunque a ogni ciclo della fascia WiFi, quindi compare o sparisce nel frame
del ciclo in cui è cambiato l'esito.

Perchè sul bordo inferiore, misurato dalle tabelle GFXglyph: sul bordo superiore
il titolo "Interno" in FONT_TITLE occupa 80 px e il badge in FONT_SMALL 100,
contro i 168 utili del riquadro largo 200. Accanto al titolo entrerebbe solo in
Picopixel, che sul vetro è alto circa 1 mm e il firmware non usa.

## Test su host

`test/test_tuya.cpp` compila il `Tuya.h` vero contro un `esp-tls` finto che fa da broker
scriptabile (`test/stub/esp_tls.h`) e un HMAC-SHA256 indipendente
(`test/stub/mbedtls/md.h`, vettore RFC 4231), due volte: con `TUYA_REQUEST_ACK` a 1 e a 0.
Controlla il contenuto di CONNECT, SUBSCRIBE e PUBLISH, che non partano mai DISCONNECT
nè PINGREQ, che la connessione resti aperta dopo il PUBACK e si rilasci senza scrivere
byte, la chiusura su ogni fallimento, i timeout con broker muto, le letture frammentate
e i pacchetti oltre il buffer. Il test dichiara da sè le guardie di `Env.h`, `Indoor.h`
e `Net.h`, che veri tirerebbero dentro credenziali, BSEC e `WiFiClientSecure`. Rete, TLS
e comportamento dello stack lwip allo spegnimento della radio non sono coperti.

## How to apply

Al bring-up, in quest'ordine: log seriale (`publish ok`, poi `il cloud ha accettato`),
poi i **Device Logs** della console per verificare i valori e che dopo ogni `Online` non
segua un `Offline` con motivo `client_closed`, poi l'app (dispositivo online, nessun
overlay, valori visibili).

Su un errore di connessione, `connect_return_code` 0x04/0x05 punta a credenziali, firma
o timestamp; un errore di trasporto punta a CA, DNS o **region del broker diversa da
quella dell'account Smart Life**, che dà un errore indistinguibile da credenziali
sbagliate.

Correlate: [[scheduler_task_table]] per la tabella dei task e i suoi hook,
[[timings_nvs_config]] per pavimenti e vincoli incrociati delle cadenze,
[[clock_sntp]] per la dipendenza dall'ora assoluta, [[mail_module]] per il
contratto dei moduli di rete, [[fetch_flag_ordering]] per l'ordering dei flag.

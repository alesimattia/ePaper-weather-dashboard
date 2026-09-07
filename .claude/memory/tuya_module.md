---
name: Modulo Tuya (TuyaLink su MQTT)
description: Tuya.h pubblica il BME680 su TuyaLink; protocollo verificato sull'SDK ufficiale, catena TLS reale del broker EU, esp-mqtt e bundle CA nativi del core, badge [TUYA ERR] sul pannello
type: project
---

`Tuya.h` (header-only, namespace `Tuya` + `Tuya::detail`) pubblica le cinque
grandezze di `Indoor::sample()` sul cloud Tuya via **TuyaLink**, il canale MQTT
che Tuya offre per hardware non suo. Il dispositivo si associa all'app Android
Smart Life scansionando il QR code generato dalla console; il firmware non fa
nessun pairing.

API: `begin()`, `pendingPublish()` (pura), `runPublish()`, `hasFailed()`.
Una sessione MQTT per messaggio: connect, publish QoS 1, PUBACK, teardown.

## Protocollo TuyaLink, formule verificate

Confermate tre volte: doc Tuya, componente ESPHome `hzkincony/esphome-tuya-iot`,
e SDK ufficiale `tuya/tuya-iot-core-sdk` (`src/tuyalink_core.c`).

```
broker    mqtts://m1.tuyaeu.com:8883        (EU centrale; us / ueaz.us / weaz.eu / cn / in)
clientId  tuyalink_{deviceId}
username  {deviceId}|signMethod=hmacSha256,timestamp={ts10},secureMode=1,accessType=1
password  hex_lower(HMAC_SHA256(key=deviceSecret,
                    msg="deviceId={deviceId},timestamp={ts10},secureMode=1,accessType=1"))
topic     tylink/{deviceId}/thing/property/report
risposta  tylink/{deviceId}/thing/property/report_response
```

Vincoli che non si vedono dal codice:

- **MQTT 3.1.1 obbligatorio**: 3.1 e 5 rifiutati. QoS 0 e 1, QoS 2 non supportato.
- `ts10` in **secondi, 10 cifre**, e deve essere **identico** in username e nella
  stringa firmata: due `time(nullptr)` separati sbagliano a cavallo del secondo,
  producendo un fallimento intermittente.
- Ordine e presenza dei campi `secureMode=1,accessType=1` sono parte della firma:
  non riordinabili.
- Serve una licenza per device (authorization code gratuiti in sviluppo).

## Payload, e le tre divergenze fra le fonti

```json
{"msgId":"7","time":1757246400,"data":{"temp_current":{"value":235}},"sys":{"ack":1}}
```

L'SDK ufficiale (`tuyalink_message_send`) compone
`{"msgId":"%d", "time":%d, "data":%s [,"sys":{"ack":%d}]}` e il suo esempio
`data_model_basic_demo.c` passa `{"power":{"value":1234,"time":...}}`. Da qui:

1. **`data` vuole oggetti con `value`**, non il valore piatto. Il componente
   ESPHome scrive `root["data"][key] = value`, cioè la forma piatta: **è non
   conforme**, e nessuno se ne accorge perchè il fallimento è silenzioso. Se un
   giorno il cloud rifiutasse la forma annidata, la piatta è il fallback.
2. **`time` accetta 10 o 13 cifre**, e l'SDK usa i **secondi**: il suo
   `system_timestamp()` ritorna `uint32_t`, che non basta per un epoch in ms.
   Il `time` per property è opzionale, quello di messaggio vale per tutte.
3. **`msgId` è una stringa di max 32 caratteri**; l'SDK usa un contatore
   incrementale reso come stringa, non un id casuale.

L'SDK pubblica in **QoS 0**. Il modulo usa QoS 1 di proposito: il PUBACK è
l'unico segnale di consegna disponibile prima di tornare a dormire.

## Il fallimento silenzioso, e come si diagnostica

**Il PUBACK conferma la consegna MQTT, non l'accettazione dei datapoint.** Con
un identifier fuori dal device model, un valore fuori dai `min`/`max` dichiarati
o una `scale` non combaciante, il publish risulta riuscito e i valori non
compaiono nell'app.

Il `report_response` **non arriva di default**: richiede `"sys":{"ack":1}` nel
messaggio, forma che non compare nelle pagine di documentazione consultabili ed
è stata presa dall'SDK. Il modulo la implementa sotto `TUYA_REQUEST_ACK`
(default 0): a 1 aggiunge il campo, sottoscrive il topic di risposta attendendo
il SUBACK prima di pubblicare, e logga il `code` (0 = accettato) o il corpo
intero in caso di rifiuto. Costa ~1148 byte di flash, 120 di RAM e fino a
`TUYA_ACK_TIMEOUT_MS` per publish.

**Corrispondenza moltiplicatore-scale**: i DP numerici Tuya sono interi con una
`scale` dichiarata in console. `TUYA_MUL_*` nel firmware deve corrispondere: con
scale 1 e moltiplicatore 1 la temperatura appare divisa per dieci, con scale 0 e
moltiplicatore 10 decuplicata. In nessuno dei due casi c'è un errore. È il
rischio di configurazione numero uno del modulo, e si vede solo in "Device
Debugging" sulla console.

## TLS: la catena reale del broker, misurata

`openssl s_client -connect m1.tuyaeu.com:8883` sul DC EU restituisce:

```
leaf        CN=*.tuyaeu.com        SAN: *.tuyaeu.com, tuyaeu.com
intermedia  DigiCert Inc, RapidSSL TLS RSA CA G1
root        DigiCert Inc, DigiCert Global Root G2
Verify return code: 0 (ok)
```

Tre conseguenze:

- la root è **DigiCert Global Root G2**, non la GoDaddy Root G2 che riportano la
  doc Tuya e il componente ESPHome (che la incorpora in un PEM costante, quindi
  sul DC EU sta verificando contro una CA che non è nella catena);
- il SAN wildcard copre `m1.tuyaeu.com`, quindi **non serve
  `skip_cert_common_name_check`**: la verifica piena passa;
- il certificato leaf viene rinnovato ogni ~6 mesi, quindi un PEM hardcoded è
  esattamente la scelta fragile. Il modulo usa `esp_crt_bundle_attach`.

Il bundle del core contiene quella root: simbolo `esp_crt_bundle_attach` in
`libmbedtls.a` (oggetto `esp_crt_bundle.c.obj`), blob `x509_crt_bundle` di
69788 byte con 112 root CA. A differenza di tutti gli altri moduli, che fanno
`setInsecure()`, qui la verifica va fatta: su questa connessione transita
l'HMAC del DeviceSecret.

## Cosa offre il core, e i gotcha dell'API esp-mqtt

Zero librerie da installare, tutto verificato in `esp32-libs/3.3.11`:

- **esp-mqtt**: include path `mqtt/esp-mqtt/include` in `flags/includes`,
  `-lmqtt` in `flags/ld_libs`. `CONFIG_MQTT_PROTOCOL_311=y` con `PROTOCOL_5`
  non impostato, cioè esattamente la versione che Tuya pretende.
  `CONFIG_MQTT_USE_CUSTOM_CONFIG` non impostato, quindi valgono i default: task
  stack 6144, buffer 1024, network timeout 10 s, reconnect 10 s.
- **mbedtls**: `mbedtls/md.h` in include path, `-lmbedtls`/`-lmbedcrypto`
  linkate, `mbedtls_md_hmac()` one-shot.
- `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384` con asymmetric non attivo:
  **~32 KB di heap per sessione TLS**, stesso profilo dei `WiFiClientSecure`
  degli altri moduli, che già convivono con i 46,8 KB dei piani cinema.

Trappole dell'API, tutte pagate almeno una volta:

- la struct di config di IDF 5.x annida struct anonime: i **designated
  initializer annidati non sono portabili in C++**, servono `cfg = {}` più
  assegnazioni campo per campo;
- **`network.disable_auto_reconnect = true` è obbligatorio**: col default, dopo
  un fallimento il task riprova in background dopo 10 s, cioè mentre il `.ino`
  ha già spento la radio e sta entrando in light sleep;
- `esp_mqtt_client_publish()` **può bloccare** fino al network timeout, e con
  client connesso scrive il PUBLISH nel contesto del task chiamante;
- la doc di `esp_mqtt_client_destroy()` dichiara solo di distruggere l'handle e
  **non garantisce di fermare il task**: chiamare `stop()` prima, ignorandone il
  ritorno (`ESP_FAIL` su client non avviato è un esito accettabile);
- `esp_mqtt_client_subscribe` in C++ è una macro per `_single`;
- l'event handler gira nel task esp-mqtt: bastano `volatile bool` **finchè
  l'handler non passa dati al chiamante**, quindi logga da sè codici di errore e
  message id. Se un giorno servisse passare un payload, servirebbe un event
  group o una coda.

## Decisioni di design non ovvie

1. **Invariante di `runPublish()`**: al ritorno non esiste nessun task esp-mqtt
   vivo nè handle allocato, in qualunque modo sia andata. È ciò che rende sicuro
   il light sleep che parte poco dopo, e il motivo per cui i passi della
   sequenza sono concatenati in un solo `if`: senza RAII è l'unico modo di
   garantire che il cleanup sia raggiunto da ogni percorso. Per la stessa
   ragione il modulo **non espone nessun `stop()`**, che sarebbe codice morto
   suggerendo il contrario.

2. **Slot consumato prima di tutto, stato pessimista.** `lastPublishMs`,
   `firstPublish` e `failedLast = true` vengono scritti prima di qualunque
   operazione fallibile e **prima della verifica del WiFi** (vedi
   [[fetch_flag_ordering]]). Due effetti volutamente cercati: nessun
   fallimento, radio assente compresa, viene ritentato prima del prossimo
   `TUYA_PUBLISH_MIN`; e ogni uscita anticipata lascia il guasto segnalato,
   perchè solo il PUBACK azzera `failedLast`. Le sole condizioni che non
   spendono lo slot sono "niente da pubblicare": modulo senza credenziali e
   campione BSEC non ancora valido.

3. **Nessun backoff a soglia** come i calendari: i modi di fallimento di Tuya
   sono permanenti su scala di minuti e ogni tentativo costa un handshake TLS,
   quindi un retry ravvicinato non ha valore.

4. **Publish solo nel ramo normale del `loop()`, non in finestra OTA**, e fuori
   dal blocco di `wifiOn()` così da essere tentato anche a radio assente. In
   finestra OTA il loop gira ogni ~10 ms e un handshake TLS congelerebbe AP e
   web server durante un upload firmware; il costo è che il primo publish cade
   a ~3 min dal boot invece di ~1.

5. **`snprintf` e non ArduinoJson** per il payload: schema fisso noto a compile
   time, niente da parsare, e un `JsonDocument` allocherebbe nel momento di
   massima pressione di heap. Il campo `sys` è concatenato al format come macro,
   così la composizione resta un unico `snprintf` senza rami.

6. **`TUYA_EPOCH_FLOOR` si aggancia a `TIME_VALID_EPOCH_MIN`** del `.ino` con
   `#ifdef`, con fallback autonomo: le due soglie non possono divergere.
   Verificato col preprocessore, non per ispezione.

7. **Il `time` del messaggio è l'epoch del campione**, ricavato da
   `millis() - s.lastUpdateMs`, non l'istante del publish: `Indoor::refresh()`
   gira a valle del blocco dei fetch, quindi la cache contiene il campione del
   wake precedente, vecchio fino a cinque minuti. Datarlo "adesso" sarebbe
   sbagliato, e questo evita di dover riordinare il `loop()`.

## Badge [TUYA ERR] sul pannello

`Weather::detail::drawIndoorTuyaBadge()` affianca `[TUYA ERR]` al titolo del
riquadro Indoor quando `Tuya::hasFailed()`, con `markDirty()` chiamato dal
`.ino` **solo quando il badge compare o sparisce** (un refresh pieno costa 24 s).

Le misure che vincolano la resa, dalle tabelle GFXglyph: la riga del titolo ha
`INDOOR_RR_W - BANNER_TITLE_LEFT_OFFSET` = **140 px utili**, di cui "Indoor" in
FONT_BODY occupa 67 e ne restano 67 per il badge. `[NO TUYA]`/`[TUYA ERR]` in
FONT_BODY misurano 121/122 px, quindi **in FONT_BODY il badge non ci sta**; in
FONT_SMALL 90, ancora fuori; in FONT_MICRO 36, entra. Il titolo intero
`"Indoor [NO TUYA]"` in FONT_BODY vuole 194 px.

Due dettagli che evitano bug:

- il badge va disegnato **dopo tutti e tre i fieldset**, perchè cambia il font:
  inserito fra le chiamate, "Weather" e "Forecast" escono in Picopixel;
- con la baseline del titolo il testo in FONT_MICRO resta 5 px **sotto** il
  bordo superiore del riquadro, quindi **non serve** la striscia di sfondo che
  `Graphics::drawFieldsetRect` usa per spezzare il bordo.

## Dipendenza dall'orologio

TuyaLink firma un timestamp Unix reale, e il firmware non aveva alcuna sorgente
di ora assoluta: nessun `configTime`/`configTzTime`/`settimeofday`, e il core
avvia SNTP solo da quelle. Vedi [[clock_sntp]] per la correzione e i tre
comportamenti che ripara. `runPublish()` ha comunque la sua guardia
`clockIsValid()` e salta il publish senza ritentare.

## How to apply

Al bring-up, in quest'ordine: log seriale (CONNECTED, poi PUBACK), poi "Device
Debugging" in console per verificare **valori e scala**, poi QR code e app. Il
passo in console non è salvabile: è l'unico punto in cui si vede un
moltiplicatore sbagliato, a meno di alzare `TUYA_REQUEST_ACK`.

Su un errore di connessione, `connect_return_code` 0x04/0x05 punta a
credenziali, firma o timestamp; un errore di trasporto punta a CA, DNS o
**region del broker diversa da quella dell'account Smart Life**, che dà un
errore indistinguibile da credenziali sbagliate.

Rischio aperto non verificabile a tavolino: lo stato online di un device
TuyaLink segue la connessione MQTT, che qui dura pochi secondi ogni
`TUYA_PUBLISH_MIN`. Tuya documenta che sensori e device low-power si mostrano
offline solo dopo 24 h, ma dipende dalla categoria scelta in console. Se l'app
lo mostrasse sempre offline pur aggiornando i valori, l'unica alternativa
sarebbe tenere l'MQTT vivo, incompatibile con il light sleep.

Correlate: [[mail_module]] per il contratto dei moduli di rete,
[[fetch_flag_ordering]] per l'ordering dei flag, [[timing_chain]] per il budget
di timeout in cui il publish si inserisce, [[layout_separation]] per la regola
delle coordinate nei `Layout_*.h` (`INDOOR_TUYA_BADGE_GAP` sta in entrambi).

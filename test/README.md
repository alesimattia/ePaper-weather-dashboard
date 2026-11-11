# Test su host

Verificano la logica **pura** di [`Timings.h`](../Timings.h) e
[`Scheduler.h`](../Scheduler.h): validazione dei tempi e calcolo delle
scadenze; più il protocollo di [`Tuya.h`](../Tuya.h), contro un broker finto.
Sono su host perché sul dispositivo la stessa verifica richiederebbe
di aspettare ore (cadenze giornaliere, rollover di `millis()`, cambio dell'ora
legale) e di provocare a mano condizioni scomode come una NVS corrotta.

```sh
./test/esegui.sh
```

Serve solo `g++` con C++20. Gli eseguibili vanno in una cartella temporanea:
nel repo non resta niente.

Su Windows `g++` non c'è e il runner è un altro:

```powershell
powershell -ExecutionPolicy Bypass -File test\esegui.ps1
```

Compila con il toolset MSVC di Visual Studio, già installato sulla macchina di
sviluppo, e mette gli artefatti in `A:\tmp\epd-test`. Copre la **stessa suite**
di `esegui.sh`. Due dettagli che non si indovinano: serve `/Zc:preprocessor`,
perchè `Log.h` usa `__VA_OPT__` e il preprocessore tradizionale di MSVC si ferma
con un errore di sintassi invece che con un avviso; e il fuso orario dei test
non viene dalla libc, per il motivo nella sezione qui sotto.

## Come sono costruiti

`stub/` contiene il minimo indispensabile di piattaforma — `Arduino.h`,
`Preferences.h`, `WiFi.h`, `esp_sleep.h`, e per Tuya `esp_tls.h`,
`esp_crt_bundle.h` (che esiste solo perchè `Tuya.h` lo include per nome), `mbedtls/md.h`, `lwip/sockets.h` — e **nient'altro**. Gli header del
firmware sono inclusi da `..`, quindi i test esercitano il codice vero: una
copia non direbbe niente.

Due dettagli non ovvi:

- `Serial.printf` dello stub ritorna `size_t` come `Print::printf` del core.
  Deve: la forma spenta di `LOG` è `((void)sizeof(...))`, e `sizeof(void)` non
  compila. È anche il motivo per cui il backend del log non può diventare una
  funzione `void`.
- `test_scheduler.cpp` sostituisce con delle macro tre funzioni della
  piattaforma: `time()`, perchè `Scheduler.h` chiama `time(nullptr)`, più
  `localtime_r()` e `mktime()`, che passano dalle regole POSIX di
  [`stub/fuso_posix.h`](stub/fuso_posix.h) invece che dalla libc dell'host.

  Il fuso non è quindi quello di sistema, ed è voluto. Sull'ESP32 **non esiste
  nessun database IANA**: newlib interpreta esattamente la stringa
  `CAL_POSIX_TZ`, cioè fa lo stesso lavoro dello stub, che è quindi più vicino
  al dispositivo della libc di un PC. E il CRT Windows non legge affatto i
  campi di transizione di quella stringa (accetta solo la forma storica
  `tzn[+|-]hh[dzn]` e poi applica le regole statunitensi), quindi appoggiarsi
  alla libc renderebbe il test inattendibile fuori da macOS e Linux.

  Perchè la cosa non diventi circolare lo stub è ancorato in due modi: una
  batteria di epoch di riferimento scritti come letterali, ricavati dal
  calendario (ultima domenica di marzo e di ottobre, transizione alle 01:00
  UTC per direttiva UE) e verificati contro il database dei fusi di Windows;
  e, su host POSIX, il confronto diretto con la libc su ogni istante del 2026
  a passi di 20 minuti, in entrambe le direzioni.

## Cosa coprono

**`test_timings`** — default; applicazione **transazionale** (allargare la
fascia WiFi e spostare l'ora del cinema sono legittimi insieme e illegittimi
presi uno alla volta); ogni vincolo incrociato respinto senza toccare lo
stato; round-trip su NVS; clamp al boot di un valore diventato illegale
perché un aggiornamento ha alzato il pavimento, con riscrittura; cadenza fuori
dal passo del ciclo respinta con il motivo e, se arriva da NVS, portata al
multiplo successivo; schema precedente che azzera il namespace; NVS non
disponibile; schema assente; ripristino dei predefiniti.

**`test_scheduler`** — interpretazione della stringa POSIX del fuso e le due
ore patologiche del cambio (quella inesistente di marzo e quella ripetuta di
ottobre, con la convenzione fissata invece che sperata). Poi il ciclo, con
sensore, radio e pannello finti che registrano la sequenza dei passi: ordine
sensore, radio, task, radio giù, frame; cadenza da 10 minuti un ciclo sì e uno
no per venti cicli con fetch di durata variabile; sleep fino alla scadenza
esatta del sensore, senza pavimento; fase di rete più lunga del ciclo con il
sensore interrogato fra un task e l'altro entro il 6,25%; ritento al ciclo
successivo con lo slot ancorato e soglia dopo due fallimenti; connessione
fallita senza tentativi spesi; notte con il solo sensore, e senza sensore sonno
fino all'apertura della fascia anche attraverso l'ora legale; primo ciclo del
mattino che rende dovuti i periodici ma non il giornaliero; giornaliero che
aspetta l'ora valida senza doppioni; finestra di manutenzione con STA in
ritardo, STA mai salita, ora da sincronizzare e client attivo; ciclo interno
senza sensore; rollover di `millis()` a 49,7 giorni; fascia fail-open con
orologio non sincronizzato; cambio dell'ora legale.

**`test_routefinder`** — parser delle coordinate "lat, lng" (segni, spazi,
decimali oltre il sesto, fuori range, forme malformate); URL con coordinate
negative; round-trip su NVS e ritorno ai default quando la fascia WiFi non
contiene più la fascia del percorso; fascia ai minuti con fine esclusa;
immagine del riquadro nelle quattro sorgenti e freschezza del percorso, con la
sorgente forzata a Percorso attiva anche fuori dalla fascia del fetch. Il test dichiara da
sè le coordinate con `ROUTEFINDER_HOST_TEST`: con le virgolette l'include di
`Env.h` troverebbe sempre quello vero accanto agli header del firmware.

**`test_tuya`** — il client MQTT di `Tuya.h` contro un `esp-tls` finto
([`stub/esp_tls.h`](stub/esp_tls.h)) che fa da broker scriptabile e un
HMAC-SHA256 indipendente ([`stub/mbedtls/md.h`](stub/mbedtls/md.h), verificato
con il vettore RFC 4231). Il punto che conta è che la sessione non mandi mai
**DISCONNECT nè PINGREQ** e che, dopo un PUBACK, la connessione **resti aperta**
fino a `releaseSession()`, perchè Tuya dichiara offline il dispositivo nell'istante
in cui la connessione si chiude: il test lo controlla, insieme alla chiusura
immediata su ogni fallimento e a quella di una sessione rimasta aperta prima di
aprirne una nuova. Poi contenuto di CONNECT (flag, keepalive, client id,
username, password), SUBSCRIBE e PUBLISH, con il payload identico a quello che
il cloud ha accettato nella prova dal vivo; CONNACK rifiutato, broker muto con
i timeout rispettati, PUBACK per un altro packet id, connessione chiusa a metà,
handshake fallito, campione non valido; letture di un byte alla volta con
`WANT_READ` e pacchetti più grandi del buffer. Gira due volte, con
`TUYA_REQUEST_ACK` a 1 e a 0, perchè i due rami hanno sequenze diverse. Il test
dichiara da sè le guardie di `Env.h`, `Indoor.h` e `Net.h`, che veri
tirerebbero dentro credenziali, BSEC e `WiFiClientSecure`.

## Cosa NON coprono

Tutto ciò che richiede l'hardware: rendering del pannello, fetch HTTP reali,
sensore BME680, radio, upload del firmware, e per Tuya la rete, il TLS e la
verifica del certificato. Per quelli vale il collaudo sul
dispositivo descritto nel [README](../README.md).

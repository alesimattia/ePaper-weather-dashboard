# Test su host

Verificano la logica **pura** di [`Timings.h`](../Timings.h),
[`Scheduler.h`](../Scheduler.h) e [`Routefinder.h`](../Routefinder.h):
validazione dei tempi, calcolo delle scadenze e scelta dell'immagine del
riquadro; l'archivio in RAM di [`CinemaStore.h`](../CinemaStore.h); il
protocollo di [`Tuya.h`](../Tuya.h), contro un broker finto; e il
refresh differenziale, cioè la politica di [`PartialRefresh.h`](../PartialRefresh.h)
e la macchina a stati del driver
([`GxEPD2_SOLUM_Differential.h`](../GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM_Differential.h)),
che non ha I/O proprio perchè si possa provare qui, e il confronto del frame con
quello sul vetro
([`GxEPD2_SOLUM_FrameCrc.h`](../GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM_FrameCrc.h)).
`test_partial` gira due volte, con l'interruttore `DISPLAY_PARTIAL_REFRESH` spento
e acceso.
Sono su host perchè sul dispositivo la stessa verifica richiederebbe
di aspettare ore (cadenze da 24 ore, fasce orarie, scadenza di una forzatura,
rollover di `millis()`, cambio dell'ora legale) e di provocare a mano condizioni
scomode come una NVS corrotta o un heap esaurito.

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

`stub/` contiene il minimo indispensabile di piattaforma: `Arduino.h`,
`Preferences.h`, `WiFi.h`, `esp_sleep.h`, per CinemaStore `esp_heap_caps.h`
(allocazioni su `malloc()`, che il test può far fallire con `g_heapEsaurito`), e per
Tuya `esp_tls.h`, `esp_crt_bundle.h` (che esiste solo perchè `Tuya.h` lo include per
nome), `mbedtls/md.h`, `lwip/sockets.h`; **nient'altro**. Gli header del
firmware sono inclusi da `..`, quindi i test esercitano il codice vero: una
copia non direbbe niente.

Due dettagli non ovvi:

- `Serial.printf` dello stub ritorna `size_t` come `Print::printf` del core.
  Deve: la forma spenta di `LOG` è `((void)sizeof(...))`, e `sizeof(void)` non
  compila. È anche il motivo per cui il backend del log non può diventare una
  funzione `void`.
- `test_scheduler.cpp` e `test_routefinder.cpp` sostituiscono con delle macro tre
  funzioni della piattaforma: `time()`, perchè `Scheduler.h` e `Routefinder.h`
  chiamano `time(nullptr)`, più
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

**`test_timings`**: default, compreso `cine_min` a 60; applicazione
**transazionale** (fascia WiFi e cadenza del cinema cambiate insieme passano per
intero); cadenza del cinema sotto il pavimento di 60 minuti respinta senza
toccare lo stato, come ogni vincolo incrociato; round-trip su NVS; clamp al boot
di un valore diventato illegale perchè un aggiornamento ha alzato il pavimento,
con riscrittura; cadenza fuori
dal passo del ciclo respinta con il motivo e, se arriva da NVS, portata al
multiplo successivo; schema precedente che azzera il namespace; NVS non
disponibile; schema assente; ripristino dei predefiniti.

**`test_scheduler`**: interpretazione della stringa POSIX del fuso e le due
ore patologiche del cambio (quella inesistente di marzo e quella ripetuta di
ottobre, con la convenzione fissata invece che sperata). Poi il ciclo, con
sensore, radio e pannello finti che registrano la sequenza dei passi: ordine
sensore, radio, task, radio giù, frame; cadenza da 10 minuti un ciclo sì e uno
no per venti cicli con fetch di durata variabile; sleep fino alla scadenza
esatta del sensore, senza pavimento; fase di rete più lunga del ciclo con il
sensore interrogato fra un task e l'altro entro il 6,25%; ritento di un
fallimento sempre al ciclo successivo, con lo slot ancorato e soglia dopo due
fallimenti; connessione fallita senza tentativi spesi; notte con il solo
sensore, e senza sensore sonno fino all'apertura della fascia anche attraverso
l'ora legale; primo ciclo del mattino che riapre lo slot di tutti i task, con un
periodico da 24 ore dovuto dopo 8 ore e il ritento rimasto in sospeso dalla sera
che non si trascina (il fallimento del mattino conta come primo dello slot);
fronte di salita della precondizione che riapre lo slot e lo rende dovuto, con il
ritento in sospeso azzerato e nessuna riapertura a precondizione stabile;
finestra di manutenzione con STA in ritardo, STA mai salita o caduta, ora da
sincronizzare e client attivo; ora non valida senza task dovuti, con la radio
riaccesa ogni `CLOCK_RECOVERY_CYCLES`, il frame a ogni ciclo e i task che
chiedono l'ora nello stesso giro del recupero; primo frame dopo l'accensione, con
l'ora che arriva dopo i 15 s dal boot, mai, o in piena notte, e con i task che
dipendono dall'ora nello stesso passo prima del frame; nessun secondo tentativo
nello stesso ciclo, sia nella finestra di manutenzione (fallimento al boot con il
frame subito, doppio fallimento, `maxAttempts` a 0, STA giù al ciclo del ritento)
sia con la radio dello scheduler (nessuna attesa dentro il passo, radio caduta al
fallimento, due task falliti nello stesso passo); ciclo interno senza sensore;
rollover di `millis()` a 49,7 giorni; fascia fail-open con orologio non
sincronizzato; cambio dell'ora legale.

**`test_routefinder`**: parser delle coordinate "lat, lng" (segni, spazi,
decimali oltre il sesto, fuori range, forme malformate); URL con coordinate
negative; round-trip su NVS di arrivo, carburante e fascia cinema; fasce vuote,
rovesciate, fuori dalla fascia WiFi o sovrapposte respinte, e ritorno ai default
al boot quando la fascia WiFi non contiene più le fasce; immagine programmata
nelle fasce ai minuti con fine esclusa e senza orologio; immagine mostrata
(`shownImage()`) con il percorso entro e oltre 3 cadenze, il cinema di oggi e
quello della sera prima, il buffer che contiene l'altra immagine o niente;
forzatura a Cinema fuori fascia, che mostra solo un cinema scaricato dopo il suo
inizio e scade dopo `SOURCE_OVERRIDE_MS` (un millisecondo prima ancora attiva);
forzatura a Percorso fuori fascia, a Wallpaper dentro la fascia percorso, e
Automatica che annulla la forzatura. Il test dichiara da sè le coordinate con
`ROUTEFINDER_HOST_TEST`: con le virgolette l'include di `Env.h` troverebbe sempre
quello vero accanto agli header del firmware.

**`test_cinemastore`**: l'archivio in RAM di percorso e cinema, con l'heap dello
stub. Allocazione fallita al boot che non disattiva l'archivio ma si ritenta al
download successivo; corpo scritto a blocchi di misura qualunque, con un blocco a
cavallo fra piano nero e piano colore e i byte verificati in entrambi; immagine
valida solo dopo il commit di un corpo completo, con tipo, epoch e `millis()` del
download; nessuna scrittura fuori da un download nè oltre la misura; immagine
precedente invalidata dall'inizio di un nuovo download, commit di un corpo
interrotto rifiutato, `abortWrite()` che lascia il buffer vuoto.

**`test_tuya`**: il client MQTT di `Tuya.h` contro un `esp-tls` finto
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

**`test_framecrc`**: il confronto del frame con quello già sul vetro
([`GxEPD2_SOLUM_FrameCrc.h`](../GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM_FrameCrc.h)), con
una CRC software che ha la stessa semantica di `esp_rom_crc32_le`. Copre il primo
frame (niente di mostrato, quindi non invariato), l'uguaglianza a frame uguali, un
solo bit diverso in una pagina qualunque del piano nero o colore, due pagine
scambiate di posto, i frame non confrontabili (ultima o prima pagina mancante,
pagina fuori ordine, piano colore mancante, scrittura estranea), la validità del
frame mostrato (buttato da un refresh esterno, rimesso solo dal commit di un frame
completo, invalido dopo il commit di un frame incompleto), l'accumulo chiuso dopo il
commit, il confronto spento e l'altezza coperta.
`test_partial` copre anche l'esito `Unchanged`: contatori a parte, ultimo frame
aggiornato, partial ancora ammesso a un ciclo di distanza, e il trait del salto
con driver finti (con o senza differenziale, con l'API completa o a metà).

## Cosa NON coprono

Tutto ciò che richiede l'hardware: rendering del pannello, fetch HTTP reali,
sensore BME680, radio, upload del firmware e del wallpaper con la scrittura della
partizione ([`WallpaperStore.h`](../WallpaperStore.h)), e per Tuya la rete, il TLS e
la verifica del certificato. Per quelli vale il collaudo sul
dispositivo descritto nel [README](../README.md).

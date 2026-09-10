# Test su host

Verificano la logica **pura** di [`Timings.h`](../Timings.h) e
[`Scheduler.h`](../Scheduler.h): validazione dei tempi e calcolo delle
scadenze. Sono su host perché sul dispositivo la stessa verifica richiederebbe
di aspettare ore (cadenze giornaliere, rollover di `millis()`, cambio dell'ora
legale) e di provocare a mano condizioni scomode come una NVS corrotta.

```sh
./test/esegui.sh
```

Serve solo `g++` con C++20. Gli eseguibili vanno in una cartella temporanea:
nel repo non resta niente.

## Come sono costruiti

`stub/` contiene il minimo indispensabile di piattaforma — `Arduino.h`,
`Preferences.h`, `WiFi.h`, `esp_sleep.h` — e **nient'altro**. Gli header del
firmware sono inclusi da `..`, quindi i test esercitano il codice vero: una
copia non direbbe niente.

Due dettagli non ovvi:

- `Serial.printf` dello stub ritorna `size_t` come `Print::printf` del core.
  Deve: la forma spenta di `LOG` è `((void)sizeof(...))`, e `sizeof(void)` non
  compila. È anche il motivo per cui il backend del log non può diventare una
  funzione `void`.
- `test_scheduler.cpp` sostituisce `time()` con una macro, perché
  `Scheduler.h` chiama `time(nullptr)`. `localtime_r` e `mktime` restano
  quelli di libc, con `TZ` impostato a Europe/Rome: così il fuso e il cambio
  dell'ora legale sono quelli veri e non una simulazione.

## Cosa coprono

**`test_timings`** — default; applicazione **transazionale** (allargare la
fascia WiFi e spostare l'ora del cinema sono legittimi insieme e illegittimi
presi uno alla volta); ogni vincolo incrociato respinto senza toccare lo
stato; round-trip su NVS; clamp al boot di un valore diventato illegale
perché un aggiornamento ha alzato il pavimento, con riscrittura; NVS non
disponibile; schema assente; ripristino dei predefiniti.

**`test_scheduler`** — cadenza periodica e coalescing; ritento a 30 s e soglia
dopo due fallimenti; `SALTATO` che lascia lo slot intatto; cadenza giornaliera
con il recupero in giornata (`tm_hour >= ora`); fascia oraria che rimanda i
task di rete alla sua apertura; orologio non sincronizzato con fascia
fail-open e giornaliera sospesa; cambio dell'ora legale; rollover di
`millis()` a 49,7 giorni; clamp del light sleep fra 30 e 300 s.

## Cosa NON coprono

Tutto ciò che richiede l'hardware: rendering del pannello, fetch HTTP reali,
sensore BME680, radio, upload del firmware. Per quelli vale il collaudo sul
dispositivo descritto nel [README](../README.md).

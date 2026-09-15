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
`Preferences.h`, `WiFi.h`, `esp_sleep.h` — e **nient'altro**. Gli header del
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
perché un aggiornamento ha alzato il pavimento, con riscrittura; NVS non
disponibile; schema assente; ripristino dei predefiniti.

**`test_scheduler`** — interpretazione della stringa POSIX del fuso e le due
ore patologiche del cambio (quella inesistente di marzo e quella ripetuta di
ottobre, con la convenzione fissata invece che sperata); cadenza periodica e
coalescing; ritento a 30 s e soglia
dopo due fallimenti; `SALTATO` che lascia lo slot intatto; cadenza giornaliera
con il recupero in giornata (`tm_hour >= ora`); fascia oraria che rimanda i
task di rete alla sua apertura; orologio non sincronizzato con fascia
fail-open e giornaliera sospesa; cambio dell'ora legale; rollover di
`millis()` a 49,7 giorni; clamp del light sleep fra 30 e 300 s.

## Cosa NON coprono

Tutto ciò che richiede l'hardware: rendering del pannello, fetch HTTP reali,
sensore BME680, radio, upload del firmware. Per quelli vale il collaudo sul
dispositivo descritto nel [README](../README.md).

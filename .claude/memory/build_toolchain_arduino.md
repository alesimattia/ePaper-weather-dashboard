---
name: Toolchain compilazione Arduino/ESP32 su questa macchina
description: Come compilare il firmware A:\epd da qui - installazione arduino-cli unica in C:\tmp\arduino (core esp32 3.3.11 con index filtrato, librerie, junction sketch), output su A:\tmp\arduino-build, FQBN e partizioni no_fs; più i test su host con MSVC
type: reference
---

**Questa macchina COMPILA il firmware.** Solo sviluppo + verifica di compilazione: nessun
upload/flash da qui (decisione dell'utente), il flash avviene da un altro PC.

**Comando unico:** `C:\tmp\arduino\build.ps1` (opzioni `-Clean`, `-Dettagli`,
`-Board <fqbn>`, `-Partizioni <schema>`). Ricrea la junction sketch se manca e copia
`Env_template.h` in `Env.h` se assente.

**Installazione unica in `C:\tmp\arduino`**, deroga autorizzata dall'utente al vincolo "tutto su
`A:\tmp\sandbox`" perchè il core non entra nello spazio libero di `A:`. La stessa installazione
la usano `A:\rd` e `A:\obd` con i propri `build.ps1`.
- `bin\arduino-cli.exe` (1.5.2) + `arduino-cli.yaml`. Il config NON è nel percorso di default:
  passare SEMPRE `--config-file C:/tmp/arduino/arduino-cli.yaml`.
- `data\` (core + toolchain), `staging\` (archivi scaricati), `user\libraries\` (librerie),
  `sketch\` (junction), `setup\` (index filtrati).
- Gli output delle build stanno su `A:`, in `A:\tmp\arduino-build\<sketch>`: tutti e tre gli
  script passano `--build-path`. La build cache `C:\tmp\arduino\cache` dello yaml la crea
  arduino-cli solo per una compilazione lanciata a mano senza quel flag.
- L'installazione non contiene percorsi assoluti (verificato con una ricerca su tutto `data\`):
  si sposta con un rename della cartella più la correzione dello yaml e degli script.

**Core esp32 3.3.11 installato con index FILTRATO** `C:\tmp\arduino\setup\package_esp32_min_index.json`
(rigenerabile da `pkg_esp32.json` nella stessa cartella, referenziato in `board_manager.additional_urls`
come `file:///`). Tenuti solo `esp-x32`, `esp32-libs`, `esptool_py`, `mkspiffs`, `mklittlefs`; esclusi
`esp-rv32`, i gdb, openocd, dfu-util e le libs di c3/c5/c6/h2/p4/s2/s3. Nella 3.3.x le SDK sono
per-target (`esp32-libs`), non più il monolite `esp32-arduino-libs` della 3.0/3.1: è ciò che rende
il filtro efficace. Reinstallando o cambiando versione: rigenerare l'index con lo stesso filtro.
L'index ufficiale Arduino contiene anch'esso il package `esp32`, ma gli `additional_urls` vincono sul
merge (verificato con `core download`), quindi il filtro ha effetto. Lo stesso index ufficiale, che
`core update-index` scarica comunque, porta però anche le versioni esp32 **più recenti** della 3.3.11,
non filtrate, e `core list` le mostra nella colonna Ultimo: si installa quindi sempre con la versione
esplicita (`core install esp32:esp32@3.3.11`) e mai con `core upgrade`.

**Core esp8266 3.1.2** (lo usa `A:\obd`), installato con l'index filtrato
`C:\tmp\arduino\setup\package_esp8266_min_index.json`, generato dall'index upstream
`pkg_esp8266.json` nella stessa cartella. Tiene la sola piattaforma 3.1.2 e due dipendenze:
`xtensa-lx106-elf-gcc` e `python3`, che le ricette di compilazione usano per gli hook di prebuild,
`elf2bin` e `sizes`. Esclusi `mkspiffs` e `mklittlefs`, che servono solo a creare immagini di
filesystem per l'upload. Verificato compilando `tools\canbox_sniffer` di obd.

**Una junction, non una copia** (Arduino pretende che la cartella si chiami come il `.ino`):
`C:\tmp\arduino\sketch\ePaper-weather-dashboard` → `A:\epd`. Le junction non vanno mai spostate
con un Move-Item fra volumi, che copierebbe il contenuto puntato: si tolgono con `rmdir` e le
ricrea `build.ps1`.

La libreria `GxEPD2` in `C:\tmp\arduino\user\libraries\GxEPD2` è una **copia potata** del clone
`A:\tmp\GxEPD2-master`, che rigenera `C:\tmp\arduino\rigenera-libreria-gxepd2.ps1`: tiene
`library.properties`, tutti gli header tranne `src\bitmaps\` e il solo `src\GxEPD2_EPD.cpp`.
Arduino compila **tutti** i sorgenti di una libreria e di quelli di GxEPD2 qui serve uno: la
potatura accorcia la build in modo netto (misurato). Gli header dei driver restano perchè `GxEPD2_3C.h` li
include sotto `__has_include`, quindi togliendoli cambierebbe cosa vede il compilatore.
**1.6.9 è la head upstream**: `de82887` è il merge commit del tag e `library.properties` di master
dichiara 1.6.9, quindi non c'è niente da aggiornare finchè non esce una release nuova; il clone è
identico a upstream a meno dei CRLF, verificato con diff. `GxEPD2_SOLUM_ESL` NON va installato come
libreria: lo sketch lo include per path relativo ed è header-only (installarlo creerebbe due path
per lo stesso header).

Le build stanno in `A:\tmp\arduino-build\<sketch>`, passata con `--build-path`: senza quel flag
arduino-cli mette gli intermedi in `C:\tmp\arduino\cache\sketches\<hash>` e chi esporta i binari
li lascia in una cartella `build\` dentro lo sketch. Con un build path esplicito la cache del core
non viene usata, quindi una build da cartella vuota ricompila anche gli oggetti del core.

**Librerie:** ArduinoJson 7.4.3, Adafruit GFX 1.12.6 + BusIO 1.17.4, bsec2 1.10.2610 (usa la
precompilata `src\esp32`), BME68x 1.3.40408.

**La compilazione non esporta nessun binario** (decisione dell'utente, comune ad
`A:\rd`): lo script non passa `--output-dir`, perchè su questa macchina non si flasha
e un `.bin` qui non servirebbe a niente. Gli oggetti intermedi in
`A:\tmp\arduino-build\<sketch>` bastano all'incrementale. Se servisse il
`.merged.bin`, si aggiunge `--output-dir` allo script.

**Trappole già pagate:**
- `directories.builtin.libraries` nel yaml fa fallire `lib install` con "la directory dell'utente non
  è configurata": non reintrodurla.
- `arduino-cli` rifiuta lo sketch se la cartella non si chiama come il `.ino`, e non basta passare il
  path del `.ino`: serve la junction.
- `Env.h` è gitignored e obbligatorio per compilare; se `build.ps1` lo rigenera dal template il
  firmware compila e gira, ma ogni produttore con credenziali al placeholder resta sospeso (vedi
  [[scheduler_task_table]]). `Env.h` non va letto per valore: contiene credenziali vere, e si
  verifica solo se le macro coincidono con il template.

**Entrambe le varianti compilano**, FQBN `esp32:esp32:esp32`, ed è il default di `build.ps1`.

**La build che va sulla board la fa il Mac** con la configurazione di `.vscode/arduino.json`:
`PartitionScheme=no_fs`, `CPUFreq=80`, `EraseFlash=all`, `PSRAM=disabled`, `LoopCore=1`,
`EventsCore=1` e altre. `build.ps1` usa i default della board (CPU a 240 MHz), quindi una verifica
fedele si fa con `arduino-cli compile -b "esp32:esp32:esp32:<stringa configuration di
arduino.json>"` e un build path separato (`A:\tmp\arduino-build\ePaper-weather-dashboard-mac`).
`arduino-cli board details` conferma che tutte quelle opzioni, `ZigbeeMode` compresa, esistono per
la board. `EraseFlash=all` cancella anche la NVS a ogni flash via seriale: calibrazione BSEC e
cadenze di `/config` ripartono dai default (scelta dell'utente).

**Lo schema di partizioni deve essere `no_fs`**, ed è il default dello script. Le CSV del core lo mostrano senza ambiguità: `huge_app.csv` ha la sola `app0` da 3 MB, `no_fs.csv` ha `app0` e `app1` da 1984 KB ciascuna, `default.csv` due slot da 1280 KB più SPIFFS. Servono **due** slot perchè l'aggiornamento dalla finestra di manutenzione scrive nella slot inattiva (vedi [[maintenance_window]]), e il firmware non usa nessun filesystem, quindi lo spazio di SPIFFS è meglio darlo alle due slot. Con `huge_app` il firmware compila e gira, ma `Update.begin()` fallisce: il guasto si manifesta solo al primo aggiornamento via web.

Il vincolo di dimensione è quindi la slot da 1984 KB, non i 4 MB di flash. I valori assoluti di flash e RAM non si annotano qui: cambiano a ogni wallpaper e a ogni modulo, e li stampa la build. Quello che resta vero è la differenza fra le due varianti: la 122c usa ~3,2 KB di RAM in più della 097c, che sono i buffer di page più alti (96 righe invece di 84), mentre il flash è praticamente identico. Si passa da una variante all'altra scambiando i `#define DISPLAY_VARIANT_*` in testa al `.ino`.

**Dipendenza non ovvia:** il wallpaper di fallback in `wallpaper/` (ad esempio `img_la_grande_onda.h`) definisce il proprio `Descriptor` sotto `#ifdef _GxEPDImage_H_`, cioè la guardia dell'header che definisce il namespace, non quella di un driver. Se quella guardia tornasse a nominare un driver specifico, la build fallirebbe con `<nome>_desc was not declared` su tutte le varianti tranne una. La emette `GxEPD2_SOLUM_ESL/tools/epd_image_converter.pyw`, raggiungibile anche dal symlink `tools/epd_image_converter.pyw` (riga con `#ifdef _GxEPDImage_H_`), quindi va corretta lì e non solo nel file generato.

**Gli examples del submodule non li compila questo build** (Arduino concatena i `.ino` solo dalla root dello sketch). Si compilano a parte passando la libreria per quella build, senza installarla: comando e caveat in [[gxepd2_solum_esl_library]].

## Test su host da questa macchina

`test/esegui.sh` pretende `g++`, che qui **non c'è**, e in `A:\tmp` non c'è nessuna toolchain C++
generica. Le uniche in `C:\tmp\arduino\...\esp-x32\2601\bin` sono cross-compiler
`xtensa-esp32-elf`: producono ELF per l'MCU, non eseguibili Windows, quindi non servono allo scopo.

Il compilatore host disponibile è **MSVC di Visual Studio 2022 Community**, già installato
(`VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe`): usarlo non installa niente e non viola il
vincolo "solo `A:\tmp\sandbox`". L'ambiente del toolset si prende chiamando `vcvars64.bat` **nello
stesso processo di `cl`**, quindi dentro un `.bat` generato, non con un `cmd /c "call ... && cl ..."`
che perderebbe il quoting: INCLUDE, LIB e PATH non sopravvivono al processo. Lo fa
`test\esegui.ps1`, runner Windows accanto a `esegui.sh`, con artefatti in `A:\tmp\epd-test`.

- Riga verificata: `cl /nologo /std:c++20 /Zc:preprocessor /EHsc /W4 /I test\stub /I . test_timings.cpp`.
- `Scheduler.h` include `Clock.h`, che usa SNTP: lo stub `test\stub\esp_sntp.h` fornisce le due
  funzioni usate e, con MSVC dove `sys/time.h` non esiste, anche `timeval` e `gettimeofday`.
- **`/Zc:preprocessor` non è opzionale**: `Log.h` usa `__VA_OPT__` e il preprocessore tradizionale
  di MSVC si ferma con errore di sintassi, non con un avviso. Misurato: senza il flag, C2146 +
  C3861 su ogni chiamata di `LOG`.
- Il backslash finale di `/Fo"<dir>\"` va **raddoppiato**: `cl` legge `\"` come virgoletta letterale
  e perde il resto della riga di comando (D8003 "nome del file di origine mancante").
- Localizzato: la console di Windows PowerShell parte in codepage OEM e i `.ps1` con accenti vanno
  salvati **UTF-8 con BOM**, come `build.ps1`, più `[Console]::OutputEncoding` a UTF-8 nello script.

**La suite gira interamente qui**, `test_timings` e `test_scheduler`. Il secondo non usa il fuso
della libc ma le regole POSIX di `test\stub\fuso_posix.h`, ed è ciò che lo rende eseguibile su
Windows: il CRT non legge i campi di transizione di una stringa TZ POSIX (accetta solo
`tzn[+|-]hh[dzn]` e poi applica le regole statunitensi), e vale anche per MinGW, che usa lo stesso
CRT. Il confronto fra stub e libc dentro il test sta sotto `#ifndef _WIN32`, quindi **su questa
macchina non viene compilato**: quel ramo lo verifica solo il Mac.

Docker Desktop è installato (daemon fermo) e resta la via per girare la suite su glibc vera:
`docker run --rm -v A:\epd:/src gcc:14 sh -c "cd /src/test && ./esegui.sh"`. WSL2 Ubuntu 26.04
esiste ma è **senza compilatore**, e installarcelo ricadrebbe fuori da `A:\tmp\sandbox`.

---
name: Toolchain compilazione Arduino/ESP32 su questa macchina
description: Come compilare il firmware A:\epd da qui - arduino-cli su A:\tmp, core esp32 3.3.11 ridotto su C:\xz, junction sketch/GxEPD2, FQBN e esito verificato; piu' i test su host con MSVC (test_timings si', test_scheduler no per le regole DST del CRT Windows)
type: reference
---

**Questa macchina COMPILA il firmware.** Configurata il 20/08/2026. Solo sviluppo + verifica di
compilazione: nessun upload/flash da qui (decisione dell'utente), il flash avviene da un altro PC
usando i binari esportati.

**Comando unico:** `A:\tmp\arduino\build.ps1` (opzioni `-Clean`, `-Dettagli`,
`-Board <fqbn>`, `-Partizioni <schema>`). Ricrea la junction sketch se manca e copia
`Env_template.h` in `Env.h` se assente.

**Layout dell'installazione (spezzata su due dischi):**
- `A:\tmp\arduino\bin\arduino-cli.exe` (1.5.2) + `A:\tmp\arduino\arduino-cli.yaml`.
  Il config NON è nel percorso di default: passare SEMPRE `--config-file A:/tmp/arduino/arduino-cli.yaml`.
- `A:\tmp\arduino\user\libraries` (librerie), `A:\tmp\arduino\sketch` (junction). Nessuna cartella di uscita:
  vedi la nota sull'export più sotto.
- `C:\xz\arduino\{data,staging,cache}` = core + toolchain + build cache (~1,9 GB). Su A: c'erano solo
  1,58 GB liberi e la 3.3.11 completa ne chiede ~3,4 GB: l'utente ha autorizzato `C:\xz` come deroga
  al vincolo "tutto su A:\tmp".

**Core esp32 3.3.11 installato con index FILTRATO** `A:\tmp\arduino-setup\package_esp32_min_index.json`
(rigenerabile da `pkg_esp32.json` nella stessa cartella, referenziato in `board_manager.additional_urls`
come `file:///`). Tenuti solo `esp-x32`, `esp32-libs`, `esptool_py`, `mkspiffs`, `mklittlefs`; esclusi
`esp-rv32` (673 MB), i gdb, openocd, dfu-util e le libs di c3/c5/c6/h2/p4/s2/s3 → 1,76 GB di archivi
ridotti a 0,53 GB. Nella 3.3.x le SDK sono per-target (`esp32-libs`), non più il monolite
`esp32-arduino-libs` della 3.0/3.1: è ciò che rende il filtro efficace.
Reinstallando o cambiando versione: rigenerare l'index con lo stesso filtro, altrimenti servono ~3,4 GB.
L'index ufficiale Arduino contiene anch'esso il package `esp32`, ma gli `additional_urls` vincono sul
merge (verificato con `core download`), quindi il filtro ha effetto.

**Una junction, non una copia** (Arduino pretende che la cartella si chiami come il `.ino`):
`A:\tmp\arduino\sketch\ePaper-weather-dashboard` → `A:\epd`.

La libreria `GxEPD2` in `A:\tmp\arduino\user\libraries\GxEPD2` è una **copia potata** del clone
`A:\tmp\GxEPD2-master`, che rigenera `A:\tmp\arduino\rigenera-libreria-gxepd2.ps1`: tiene
`library.properties`, tutti gli header tranne `src\bitmaps\` e il solo `src\GxEPD2_EPD.cpp`.
Arduino compila **tutti** i sorgenti di una libreria e dei 104 di GxEPD2 qui serve uno: misurati
696 s di build contro 345 s dopo la potatura. Gli header dei driver restano perchè `GxEPD2_3C.h` li
include sotto `__has_include`, quindi togliendoli cambierebbe cosa vede il compilatore.
**1.6.9 è la head upstream**: `de82887` è il merge commit del tag e `library.properties` di master
dichiara 1.6.9, quindi non c'è niente da aggiornare finchè non esce una release nuova; il clone è
identico a upstream a meno dei CRLF, verificato con diff. `GxEPD2_SOLUM_ESL` NON va installato come
libreria: lo sketch lo include per path relativo ed è header-only (installarlo creerebbe due path
per lo stesso header).

Le build stanno in `A:\tmp\arduino-build\<sketch>`, passata con `--build-path`: senza quel flag
arduino-cli mette gli intermedi in `C:\xz\arduino\cache\sketches\<hash>` e chi esporta i binari
li lascia in una cartella `build\` dentro lo sketch. Con un build path esplicito la cache del core
non viene usata, quindi una build da cartella vuota ricompila anche i 59 oggetti del core.

**Librerie:** ArduinoJson 7.4.3, Adafruit GFX 1.12.6 + BusIO 1.17.4, bsec2 1.10.2610 (usa la
precompilata `src\esp32`), BME68x 1.3.40408.

**Dal 25/08/2026 la compilazione non esporta più nessun binario** (decisione
dell'utente, presa sul progetto `A:\rd` ed estesa qui): `--output-dir` è stato tolto
dallo script e la cartella `A:\tmp\arduino\out` è stata rimossa. Su questa
macchina non si flasha, quindi un `.bin` qui non serviva a niente; gli oggetti
intermedi restano in `build\` dentro la cartella dello sketch e bastano
all'incrementale. Se un giorno servisse davvero il `.merged.bin`, si rimette
`--output-dir` nello script.

La RAM globale della 097c è coerente con la stima ~69 KB in [[esp32_cinema_consumer]].

**Trappole già pagate:**
- `directories.builtin.libraries` nel yaml fa fallire `lib install` con "la directory dell'utente non
  è configurata": non reintrodurla.
- `arduino-cli` rifiuta lo sketch se la cartella non si chiama come il `.ino`, e non basta passare il
  path del `.ino`: serve la junction.
- `Env.h` è gitignored e obbligatorio per compilare; quello attuale ha solo placeholder, quindi il
  firmware compila ma non funziona in campo finchè non ci sono credenziali vere.

**Entrambe le varianti compilano**, FQBN `esp32:esp32:esp32`, ed e' il default di `build.ps1`.

**Lo schema di partizioni deve essere `no_fs`**, ed e' il default dello script. Le CSV del core lo mostrano senza ambiguita': `huge_app.csv` ha la sola `app0` da 3 MB, `no_fs.csv` ha `app0` e `app1` da 1984 KB ciascuna, `default.csv` due slot da 1280 KB piu' SPIFFS. Servono **due** slot perche' l'aggiornamento dalla finestra di manutenzione scrive nella slot inattiva (vedi [[maintenance_window]]), e il firmware non usa nessun filesystem, quindi lo spazio di SPIFFS e' meglio darlo alle due slot. Con `huge_app` il firmware compila e gira, ma `Update.begin()` fallisce: il guasto si manifesta solo al primo aggiornamento via web.

Il vincolo di dimensione e' quindi la slot da 1984 KB, non i 4 MB di flash. I valori assoluti di flash e RAM non si annotano qui: cambiano a ogni wallpaper e a ogni modulo, e li stampa la build. Quello che resta vero e' la differenza fra le due varianti: la 122c usa ~3,2 KB di RAM in piu' della 097c, che sono i buffer di page piu' alti (96 righe invece di 84), mentre il flash e' praticamente identico. Si passa da una variante all'altra scambiando i `#define DISPLAY_VARIANT_*` in testa al `.ino`.

**Dipendenza non ovvia:** il wallpaper di fallback in `wallpaper/` (oggi `img_la_grande_onda.h`) definisce il proprio `Descriptor` sotto `#ifdef _GxEPDImage_H_`, cioè la guardia dell'header che definisce il namespace, non quella di un driver. Se quella guardia tornasse a nominare un driver specifico, la build fallirebbe con `<nome>_desc was not declared` su tutte le varianti tranne una. La emette `epd_image_converter.pyw` (riga con `#ifdef _GxEPDImage_H_`), quindi va corretta lì e non solo nel file generato.

**Gli examples del submodule non li compila questo build** (Arduino concatena i `.ino` solo dalla root dello sketch). Si compilano a parte passando la libreria per quella build, senza installarla: comando e caveat in [[gxepd2_solum_esl_library]].

## Test su host da questa macchina

`test/esegui.sh` pretende `g++`, che qui **non c'è**, e in `A:\tmp` non c'è nessuna toolchain C++
generica. Le uniche in `C:\xz\arduino\...\esp-x32\2601\bin` sono cross-compiler
`xtensa-esp32-elf`: producono ELF per l'MCU, non eseguibili Windows, quindi non servono allo scopo.

Il compilatore host disponibile è **MSVC di Visual Studio 2022 Community**, già installato
(`VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe`): usarlo non installa niente e non viola il
vincolo "solo `A:\tmp\sandbox`". L'ambiente del toolset si prende chiamando `vcvars64.bat` **nello
stesso processo di `cl`**, quindi dentro un `.bat` generato, non con un `cmd /c "call ... && cl ..."`
che perderebbe il quoting: INCLUDE, LIB e PATH non sopravvivono al processo. Lo fa
`test\esegui.ps1`, runner Windows accanto a `esegui.sh`, con artefatti in `A:\tmp\epd-test`.

- Riga verificata: `cl /nologo /std:c++20 /Zc:preprocessor /EHsc /W4 /I test\stub /I . test_timings.cpp`.
- **`/Zc:preprocessor` non è opzionale**: `Log.h` usa `__VA_OPT__` e il preprocessore tradizionale
  di MSVC si ferma con errore di sintassi, non con un avviso. Misurato: senza il flag, C2146 +
  C3861 su ogni chiamata di `LOG`.
- Il backslash finale di `/Fo"<dir>\"` va **raddoppiato**: `cl` legge `\"` come virgoletta letterale
  e perde il resto della riga di comando (D8003 "nome del file di origine mancante").
- Localizzato: la console di Windows PowerShell parte in codepage OEM e i `.ps1` con accenti vanno
  salvati **UTF-8 con BOM**, come `build.ps1`, più `[Console]::OutputEncoding` a UTF-8 nello script.

**La suite gira interamente qui**, `test_timings` e `test_scheduler`. Il secondo non usa il fuso
della libc ma le regole POSIX di `test\stub\fuso_posix.h`, ed e' cio' che lo rende eseguibile su
Windows: il CRT non legge i campi di transizione di una stringa TZ POSIX (accetta solo
`tzn[+|-]hh[dzn]` e poi applica le regole statunitensi), e vale anche per MinGW, che usa lo stesso
CRT. Il confronto fra stub e libc dentro il test sta sotto `#ifndef _WIN32`, quindi **su questa
macchina non viene compilato**: quel ramo lo verifica solo il Mac.

Docker Desktop e' installato (daemon fermo) e resta la via per girare la suite su glibc vera:
`docker run --rm -v A:\epd:/src gcc:14 sh -c "cd /src/test && ./esegui.sh"`. WSL2 Ubuntu 26.04
esiste ma e' **senza compilatore**, e installarcelo ricadrebbe fuori da `A:\tmp\sandbox`.

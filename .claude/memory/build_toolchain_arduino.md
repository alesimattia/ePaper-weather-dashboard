---
name: Toolchain compilazione Arduino/ESP32 su questa macchina
description: Come compilare il firmware A:\epd da qui - arduino-cli su A:\tmp, core esp32 3.3.11 ridotto su C:\xz, junction sketch/GxEPD2, FQBN e esito verificato
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

**Entrambe le varianti compilano** (FQBN `esp32:esp32:esp32`, `PartitionScheme=huge_app`):

| variante | flash | RAM globali |
|---|---|---|
| `DISPLAY_VARIANT_097C` | 1 358 336 B (43%) | 78 368 B (23%) |
| `DISPLAY_VARIANT_122C` | 1 359 044 B (43%) | 81 592 B (24%) |

I ~3,2 KB di RAM in più della 122c sono i buffer di page più alti (96 righe invece di 84). Si passa da una variante all'altra scambiando i `#define DISPLAY_VARIANT_*` in testa al `.ino`.

**Dipendenza non ovvia:** `wallpaper/img_apple_bwry.h` definisce il proprio `Descriptor` sotto `#ifdef _GxEPDImage_H_`, cioè la guardia dell'header che definisce il namespace, non quella di un driver. Se quella guardia tornasse a nominare un driver specifico, la build fallirebbe con `img_apple_bwry_desc was not declared` su tutte le altre varianti. La emette `epd_image_converter.pyw`, quindi va corretta lì e non solo nel file generato.

**Gli examples del submodule non li compila questo build** (Arduino concatena i `.ino` solo dalla root dello sketch). Si compilano a parte passando la libreria per quella build, senza installarla: comando e caveat in [[gxepd2_solum_esl_library]].

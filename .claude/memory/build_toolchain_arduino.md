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
- `A:\tmp\arduino\user\libraries` (librerie), `A:\tmp\arduino\sketch` (junction), `A:\tmp\arduino\out` (binari).
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

**Due junction, non copie** (Arduino pretende che la cartella si chiami come il `.ino`):
- `A:\tmp\arduino\sketch\ePaper-weather-dashboard` → `A:\epd`
- `A:\tmp\arduino\user\libraries\GxEPD2` → `A:\epd\GxEPD2-master` (identico a upstream 1.6.9 a meno
  dei CRLF, verificato con diff). `GxEPD2_SOLUM_ESL` NON va installato come libreria: lo sketch lo
  include per path relativo ed è header-only (installarlo creerebbe due path per lo stesso header).

**Librerie:** ArduinoJson 7.4.3, Adafruit GFX 1.12.6 + BusIO 1.17.4, bsec2 1.10.2610 (usa la
precompilata `src\esp32`), BME68x 1.3.40408.

**Esito verificato 20/08/2026** (FQBN `esp32:esp32:esp32`, `PartitionScheme=huge_app`, variante 097c):
flash 1358220 B = 43% di 3145728; RAM globali 78368 B = 23% di 327680 — coerente con la stima ~69 KB
in [[esp32_cinema_consumer]]. Binari in `A:\tmp\arduino\out`, incluso `.merged.bin` per il flash altrove.

**Trappole già pagate:**
- `directories.builtin.libraries` nel yaml fa fallire `lib install` con "la directory dell'utente non
  è configurata": non reintrodurla.
- `arduino-cli` rifiuta lo sketch se la cartella non si chiama come il `.ino`, e non basta passare il
  path del `.ino`: serve la junction.
- `Env.h` è gitignored e obbligatorio per compilare; quello attuale ha solo placeholder, quindi il
  firmware compila ma non funziona in campo finchè non ci sono credenziali vere.

**Variante 122c non compilabile da qui:** `Layout_122c.h` include `GxEPD2_SOLUM_122c_960x768/` che vive nel branch `Solum_12_2` e non esiste su `main` (vedi [[layout_separation]]). Con `DISPLAY_VARIANT_122C` la build fallisce sull'include, non sul layout.

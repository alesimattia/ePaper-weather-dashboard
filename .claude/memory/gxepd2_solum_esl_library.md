---
name: Architettura della libreria GxEPD2_SOLUM_ESL (submodule bi-driver)
description: Come è fatto il submodule con più driver custom - ombrello di selezione GxEPD2_SOLUM.h, pinout uniforme, namespace GxEPDImage condiviso, contratto che un nuovo driver deve rispettare, come si compilano i suoi examples
type: reference
---

`A:\epd\GxEPD2_SOLUM_ESL` (branch `main`, <https://github.com/alesimattia/GxEPD2_SOLUM_ESL>,
GPL-3.0) è una **libreria Arduino a sè stante** che estende GxEPD2 con i driver dei pannelli SOLUM
recuperati da ESL. Header-only, `architectures=esp32`,
`depends=GxEPD2 (>=1.6.9),Adafruit GFX Library`. Ospita **due driver** e la struttura è pensata per
N: quello che segue è il meccanismo, non un dettaglio di uno dei due.

```
src/GxEPD2_SOLUM.h                  ombrello di selezione, unico header che gli sketch includono
src/GxEPD2_SOLUM_Pins.h             struct di pinout uniforme fra i driver
src/GxEPDImage.h                    namespace GxEPDImage + template showImage(), condiviso
src/GxEPD2_SOLUM_097c_960x672.h     driver 9.7" SSD1677, vedi [[gxepd2_097c_driver]]
src/GxEPD2_SOLUM_122c_960x768.h     driver 12.2" UC8179 assunto, vedi [[gxepd2_122c_driver]]
examples/panel_diagnostic/          sonda del 9.7"
examples/12_2c/dual_panel_finder/   sonda del 12.2"
examples/12_2c/color_cycle/         primo uso del driver 12.2"
docs/                               PDF e sorgenti OEPL; docs/122c/ cablaggi del 12.2"
README.md                           libreria + driver 9.7"; README_122c.md il 12.2"
```

**Ombrello `src/GxEPD2_SOLUM.h`.** È l'idioma di `GxEPD2_display_selection_new_style.h` upstream
(dove `GxEPD2_DRIVER_CLASS` e `MAX_HEIGHT()` sono macro), portato **dentro** la libreria invece che
negli esempi. Uno sketch non nomina mai la classe concreta:

```cpp
#define SOLUM_PANEL_122C            // unica riga che cambia fra un pannello e l'altro
#include <GxEPD2_3C.h>
#include <GxEPD2_SOLUM.h>
GxEPD2_3C<GxEPD2_SOLUM_DRIVER_CLASS, SOLUM_MAX_HEIGHT(GxEPD2_SOLUM_DRIVER_CLASS)>
    display(GxEPD2_SOLUM_DRIVER_CLASS(GxEPD2_SOLUM_Pins{ 15, 27, 26, 25, 33, 35, 13, 12, 14 }));
```

- `SOLUM_PANEL_097C` / `SOLUM_PANEL_122C`: da definire **prima** dell'include. Zero o due danno
  `#error` (verificato: la build fallisce al preprocessore con quel messaggio).
- `GxEPD2_SOLUM_DRIVER_CLASS`: nome della classe selezionata.
- `SOLUM_MAX_HEIGHT(EPD)`: altezza della page dal budget `SOLUM_MAX_DISPLAY_BUFFER_SIZE` (default
  65536 su ESP32, lo stesso di upstream; 15000 altrove), diviso 2 perchè i pannelli hanno due piani
  a 1 bpp, con cap a `EPD::HEIGHT`. Su un pannello 960 px di larghezza dà **273 righe = ~65 KB** di
  buffer: va bene per uno sketch di prova, non per un firmware che ha altro in RAM. Il consumer
  `A:\epd` non la usa e tiene `Panel::HEIGHT / 8` (~20 KB, otto page).

**Pinout uniforme `GxEPD2_SOLUM_Pins`.** È ciò che rende un driver sostituibile con l'altro: i
pannelli non hanno lo stesso numero di segnali (il 12.2" ha due CS e due BUSY perchè ha due
controller) e senza una firma comune cambiare pannello vorrebbe dire riscrivere la riga di
costruzione del display. Campi in ordine: `cs, dc, rst, busy, cs2, busy2, sck, miso, mosi`; `-1`
significa assente e i campi non passati lo prendono dal default member initializer (serve C++14+,
il core ESP32 compila a gnu++17). Ogni driver ha un `explicit Driver(const GxEPD2_SOLUM_Pins&)` che
delega al proprio costruttore nativo leggendo i campi che gli servono.

**Contratto di `GxEPDImage.h`.** Il namespace e il template `showImage()` sono **unici** per la
libreria, non duplicati per driver: due header driver possono stare nella stessa translation unit.
Il template è indipendente dal silicio ma pretende cinque metodi pubblici da ogni driver —
`setPaged()`, `showImagePageHint()`, `writeImageYellow()`, `preserveYellow()`,
`isYellowPreserved()` — elencati in testa a quel file. Un driver a due piani dichiara le tre del
giallo come **no-op**: il ramo `FORMAT_BWRY_1BPP` è guardato dal formato del descrittore, non dal
tipo del driver, quindi non le chiama mai. Conseguenza pratica: i moduli applicativi scritti per il
9.7" (`Weather.h` chiama `preserveYellow(true)`, il `.ino` costruisce un descrittore BWRY)
compilano contro un driver a 3 colori senza rami condizionali.

**Regole per aggiungere un driver** (scritte anche nel README, sezione *Aggiungere un driver alla
libreria*):

1. header in `src/` che include `GxEPDImage.h` e `GxEPD2_SOLUM_Pins.h` e implementa il contratto;
2. costruttore `explicit Driver(const GxEPD2_SOLUM_Pins&)` oltre a quelli nativi. Da qui segue che
   ogni pin va guardato con `>= 0` prima di `pinMode()` / `digitalWrite()`, come fa
   `GxEPD2_EPD::init()` e `_writeCommand()` della base: `-1` è un valore legale della struct e non
   deve arrivare all'API Arduino;
3. bus SPI **sempre** da `_pSPIx` / `_spi_settings` della base `GxEPD2_EPD`, mai dall'oggetto `SPI`
   globale: un default proprio si imposta chiamando `selectSPI()` nel costruttore, non cablando le
   `SPISettings` nelle primitive. Altrimenti un `selectSPI()` dello sketch è silenziosamente inerte;
4. membro `panel`: i driver prendono in prestito un valore di `GxEPD2::Panel` upstream. **Non**
   scegliere `GDEW0154Z04` nè `GDE0213B1`: i template li trattano in modo speciale
   (`GxEPD2_3C.h:373` e `:504`, `GxEPD2_BW.h:249` in `A:\epd\GxEPD2-master`) e il driver si
   porterebbe dietro il workaround di un altro pannello. Il 097c usa `GDEM133Z91`, il 122c
   `GDEY1248Z51`, nessuno dei due nelle liste dei quirk;
5. tre righe nell'ombrello: ramo `#elif`, include, `GxEPD2_SOLUM_DRIVER_CLASS`;
6. `library.properties` non si tocca: `includes=GxEPD2_SOLUM.h`, non i driver.

**Compilare gli examples del submodule.** Non li compila il build del firmware (Arduino concatena i
`.ino` solo dalla root dello sketch), e la libreria **non** va installata come libreria Arduino —
lo sketch consumer la include per path relativo e installarla creerebbe due path per lo stesso
header (vedi [[build_toolchain_arduino]]). Si compilano passando la cartella come libreria solo per
quella build:

```
A:\tmp\arduino\bin\arduino-cli.exe compile --config-file A:/tmp/arduino/arduino-cli.yaml \
  -b esp32:esp32:esp32 --library A:/epd/GxEPD2_SOLUM_ESL <cartella-sketch>
```

Per forzare il define di selezione da riga di comando:
`--build-property "compiler.cpp.extra_flags=-DSOLUM_PANEL_122C"` (cambia i flag, quindi ricompila
tutto il core: ~10 min).

**Consumer.** `A:\epd` usa la libreria come submodule: i suoi `Layout_097c.h` / `Layout_122c.h`
definiscono il proprio `SOLUM_PANEL_*`, includono `GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM.h` ed
espongono `Layout::Panel`, `Layout::PAGE_HEIGHT`, `Layout::makePanel()` — vedi
[[layout_separation]].

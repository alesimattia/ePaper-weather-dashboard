---
name: Driver custom GxEPD2_SOLUM_122c_960x768 (pannello SOLUM 12.2")
description: Il driver del 12.2" nel submodule - base UC8179 dual-controller assunta e non validata, controller in discussione, cosa devono stabilire le sonde, split master/slave e le altre assumption, perchè i fix del 097c non sono portabili
type: reference
---

Driver del pannello **SOLUM Newton-Core 12.2"** (960w × 768h, bianco/nero/rosso, 2 FFC da 21 pin,
uno per controller). Vive in `A:\epd\GxEPD2_SOLUM_ESL\src\GxEPD2_SOLUM_122c_960x768.h`, dentro la
libreria bi-driver: meccanismo di selezione, pinout uniforme e contratto in
[[gxepd2_solum_esl_library]]. Doc dedicata: `GxEPD2_SOLUM_ESL/README_122c.md`.

**NON È UN DRIVER FUNZIONANTE. È una proposta di init.** Nessuna delle sue assumption è confermata
sul pannello. Compila per entrambe le varianti del firmware, ma che il pannello risponda non è
dimostrato. Non scrivere nè cancellare righe sulla base di quanto segue prima delle misure.

**QUESTIONE CONTROLLER: APERTA, e le fonti si contraddicono.** Il datasheet
`Newton-Core_Specifications.pdf` è marketing e non dichiara l'IC.

- Il driver assume **UC8179 dual-controller master/slave**, scheletro strutturale da
  `GxEPD2_1248c` (GDEY1248Z51 12.48"). Motivazioni: 2 FFC = pattern dual-controller, diagonale
  vicina. Command set: `0x00` panel setting, `0x04`/`0x02` power on/off, `0x06` booster, `0x10`
  piano B/N, `0x13` piano rosso, `0x12` refresh, `0x15` DUSPI, `0x50` VCOM/data interval, `0x60`
  TCON, `0x61` resolution, `0x90`/`0x91`/`0x92` partial window, `0xE0` cascade, `0xE3` spacing,
  `0xE5` force temperature, `0x07 0xA5` deep sleep.
- Contro: `examples/12_2c/dual_panel_finder` esiste **per dimostrare il contrario**, cioè che il
  Newton-Core risponda a **SSD16xx**. Usa il driver stock `GxEPD2_1160c_GDEY116Z91` (960×640, ne
  scrive le prime 640 righe delle 768) su un controller alla volta, con un pattern asimmetrico
  disegnato con primitive GFX. Va eseguito due volte, `TEST_TARGET = TEST_MASTER` e poi
  `TEST_SLAVE`, e dice sia quale silicio risponde sia come i due FFC si dividono il pannello.
- Sospetta **architettura master-slave gerarchica**: collegando solo il FFC #2 al connettore
  Waveshare, con lo stesso codice single-CS, il display non aggiorna nulla. L'ipotesi è che le
  tensioni high-voltage (VGH/VGL/VCOM) le generi il master e le distribuisca via PCB interno: lo
  slave isolato ha SPI ma non può fare elettroforesi. Si conferma col multimetro pin-a-pin fra i 2
  FFC a pannello scollegato: 5+ pin condivisi oltre a GND e VCC → confermata.

Se la misura dice SSD16xx, il driver va **riscritto** sulla base SSD1677 del 9.7"
(`_InitDisplay`/`_PowerOn`/`_PowerOff`/`_Update_Full`/`hibernate`) tenendo lo scheletro
dual-controller, che resta valido in entrambi i casi: `ScreenPart` M/S, `_writeCommandAll` /
`_writeDataAll`, `_waitWhileAnyBusy`, il dispatch outer-class.

**I fix del 097c non sono portabili su questo driver.** I due driver non condividono **un solo
opcode**: il 097c è SSD1677 (`0x24`/`0x26`/`0x28`, `0x44`/`0x45`, `0x4E`/`0x4F`, `0x22`/`0x20`).
Auto Write Pattern `0x46`/`0x47`, polarità RAM, piano giallo `0x28`, hibernation `0x11`→`0x03`:
comandi che su UC8179 non esistono. Quello che i due condividono è solo l'infrastruttura —
`GxEPDImage` (ora in un header a parte, quindi automaticamente allineato), bulk-SPI con
`writeBytes`, page-hint di `showImage`, dirty flag dell'accent, `_cleanAccentIfDirty`.

**Assumption da validare** (tabella completa in `README_122c.md` §5, marcate `TODO[VERIFY]` nel
codice):

| Cosa | Default nel codice | Sintomo se sbagliato |
|---|---|---|
| controller | UC8179 | pannello muto, BUSY mai rilasciato |
| split master/slave | verticale, M = colonne 0..479, S = 480..959 | aggiorna mezzo schermo |
| reverse scan | `0x00`: M = `0x0f` normale, S = `0x03` reverse | metà destra specchiata |
| resolution `0x61` | 480 × 768 per controller (`0x01E0`, `0x0300`) | bordo nero o artefatti sulla giunzione |
| booster `0x06` | `0x27 0x27 0x18 0x17` dal 1248c | contrasto basso, ghosting |
| refresh | `full_refresh_time = 25000` ms | refresh troncato |
| LUT | OTP, nessuna LUT custom | ghosting |

**Niente giallo.** `preserveYellow`, `isYellowPreserved`, `writeImageYellow` sono **no-op**: le
pretende il template `showImage()` condiviso e le chiamano i moduli applicativi scritti per il 9.7"
senza sapere quale pannello è montato. Il ramo `FORMAT_BWRY_1BPP` non arriva mai a loro, lo esclude
il formato del descrittore.

**Bus SPI**: passa da `_pSPIx` / `_spi_settings` della base `GxEPD2_EPD`, ScreenPart comprese — che
ne tengono un **riferimento**, non una copia, così un `selectSPI()` vale anche per loro. I tre
costruttori impostano come default `SPI` globale a 20 MHz chiamando `selectSPI()`; lo sketch lo
sostituisce chiamandola prima di `init()`. Il driver apre il bus da sè dentro `init()` →
`_initSPI()`, per questo il pinout gli passa anche `sck`/`miso`/`mosi`.

**Costruttori**: ESP32 a 9 pin (sck, miso, mosi, cs_m, cs_s, dc, rst, busy_m, busy_s), 6 pin
(senza bus), single-CS a 4 pin per il bring-up con un solo controller cablato (`cs_s = -1`, le
scritture verso S sono no-op), e quello a `GxEPD2_SOLUM_Pins` che delega ai precedenti secondo i
campi valorizzati.

**Hardware di bring-up**: Waveshare E-Paper ESP32 Driver Board, FFC #1 nel connettore interno
(CS=GPIO15, BUSY=GPIO25, SCK=13, MISO=12, MOSI=14, DC=27, RST=26) + breakout 24-pin esterno per il
secondo controller (CS_S=GPIO33, BUSY_S=GPIO35, gli altri segnali in parallelo). Schemi, procedura
col multimetro e caveat sull'allineamento del FFC in
`GxEPD2_SOLUM_ESL/docs/122c/connessioni.html` + i due SVG.

**Sketch**: `examples/12_2c/color_cycle` cicla bianco/nero/rosso full-screen ogni 60 s via
`clearScreen(black, color)` e stampa i tempi di refresh; è anche l'esempio di come si usa
l'ombrello di selezione. `examples/12_2c/dual_panel_finder` è la sonda descritta sopra e non usa
questo driver.

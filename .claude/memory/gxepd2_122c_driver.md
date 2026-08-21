---
name: Driver custom GxEPD2_SOLUM_122c_960x768 (pannello SOLUM 12.2")
description: Il 12.2" nel submodule - identità Newton PRO EL122H6W4A, controller SSD16xx 2x960x384 con split sull'asse corto, command set e geometria già allineati nel codice, mirror della banda nel data path, seconda coda FFC muta e le tre ipotesi, dove stanno le evidenze FCC
type: reference
---

Driver del pannello **SOLUM Newton PRO 12.2"** (768w × 960h nativi, bianco/nero/rosso,
2 code FFC, una per controller). Vive in `A:\epd\GxEPD2_SOLUM_ESL\src\GxEPD2_SOLUM_122c_960x768.h`,
dentro la libreria bi-driver: meccanismo di selezione, pinout uniforme e contratto in
[[gxepd2_solum_esl_library]]. Doc: `GxEPD2_SOLUM_ESL/README_122c.md` +
`GxEPD2_SOLUM_ESL/docs/122c/identificazione_pannello.md` (evidenze e misure) +
`docs/122c/connessioni.html` (cablaggio).

**Identità. Due linee di prodotto, stesso pannello**: non correggere una nell'altra. Le pratiche
FCC del 12.2" sono tre — `2AFWN-EL122H6W4A` Newton **PRO** (tag `PRO_12.2_Nordic_TAG_R02`,
nRF52811, etichetta vetro `Newton PRO 12.2" BWR normal`, barcode FF00C9A6E59B),
`2AFWN-EL122H5CRC` Newton **Core / M3** (tag `M3_NEWTON_CORE_12.2_TAG_H07` del 2024.02.22,
etichetta vetro `M3 12.2" NEWTON BWR Normal`, barcode 0B1A1612E5D2; foto a risoluzione più alta,
4032x3024 salvate a strisce da 252 righe), `2AFWN-EL122R2WRN` generazione precedente. Geometria,
colori e code identiche: cambiano scheda tag e guscio. Quale linea sia un dato esemplare lo dice
**l'etichetta serigrafata sul vetro**. Part number delle code: **`FPC-7717`** sul 12.2",
`FPC-7711` sull'11.6", `FPC-77xx` sul 9.7" — stessa serie di progetto, ma conteggio contatti
diverso fra le taglie (24 sul 9.7", 21 misurati sul 12.2"), quindi la serie non implica pinout
identico. 768×960 px, 102 dpi, area attiva
190.1×237.6 mm, pitch quadrato 0.2475 mm. Label 216.2×260×15.35 mm, 586 g, 6 CR2450.
Tag di fabbrica: scheda `PRO_12.2_Nordic_TAG_R02`, MCU **nRF52811**, nessun TCON discreto
(i controller sono i COF alla base delle due code), **due reti di boost** una per controller.
Famiglia: il 9.7" è **BWRY**, l'11.6" e il 12.2" sono **BWR** (etichette dei pannelli).

**GEOMETRIA E CONTROLLER: RISOLTI, e il driver è allineato.**

- Ogni controller pilota **960 × 384**, split sull'**asse corto** (i 768 px). Misurato:
  con ESP32 su una sola coda FFC si stampa correttamente un rettangolo 960×384.
- In coordinate driver (`WIDTH 960` = source, `HEIGHT 768` = gate) le metà sono **bande
  orizzontali**: righe 0..383 / 384..767. `PART_WIDTH`/`PART_HEIGHT` = 960/384 e il dispatch
  trasla lo slave di `y - M.HEIGHT`.
- Controller **SSD16xx**, non UC8179. Argomento chiuso senza bisogno di serigrafia:
  SSD1677 = 960 source × 680 gate; il 9.7" (672 gate) e l'11.6" (640 gate) stanno in un
  chip e hanno **una** coda, il 12.2" (768 gate) non ci sta e ne ha **due** → 2 × 384 gate.
  UC8179 arriva a 800×600: con 2 controller nè 960×384 nè 480×768 ci stanno, quindi la base
  UC8179 è impossibile a prescindere. I 960 source coincidono con l'asse lungo di tutta la
  famiglia, ed è il bordo su cui si attaccano le code.
- Il command set nel driver è **SSD16xx**, preso dal 9.7" ([[gxepd2_097c_driver]]) e concorde
  con lo stock `GxEPD2_1160c_GDEY116Z91` (che è il driver con cui il pannello ha stampato, non
  il modello del pannello). Init in broadcast a entrambi i controller: `0x12` SWRESET +
  delay 200, `0x0C` soft start `AE C7 C3 C0 80`, `0x01` MUX = 383 (`7F 01 00`), `0x3C` = 0x01,
  `0x18` = 0x80, `0x11` = 0x03. Nessun power on in init: lo fa la sequenza `0x22` = 0xF7.
  Piani `0x24` B/N e `0x26` rosso, finestra `0x44`/`0x45` + contatore `0x4E`/`0x4F` in
  **pixel** (XEnd POR = 0x3BF = 959), power on/off `0x22` = 0xC0/0xC3 + `0x20`, refresh
  `0x22` = 0xF7 + `0x20`, deep sleep `0x10` = 0x03. Lo scheletro dual-controller del 1248c
  resta quello che era (`ScreenPart` M/S, `_writeCommandAll`/`_writeDataAll`,
  `_waitWhileAnyBusy`, dispatch outer-class).
- **BUSY attivo alto**: i costruttori passano `HIGH` alla base, non `LOW` come vuole l'UC8179.
- `_waitWhileAnyBusy` ignora lo slave se manca `busy2` **oppure** se manca `cs2`: un controller mai
  selezionato non può essere occupato, e con una sola coda cablata il suo BUSY è flottante. Senza la
  guardia su `_cs_s` un pinout con `cs2 = -1` e `busy2` valorizzato manderebbe ogni attesa al
  timeout di 30 s, perchè GPIO35 è input-only senza pull. Non "semplificare" via la doppia guardia.
- **Le due code escono da bordi opposti alla stessa altezza**, verificato sulle strisce singole
  delle foto Core (non sul montaggio, che ha offset per striscia e inganna). Le due COF sono
  quindi legate da una rotazione di 180° **geometrica**; l'orientamento **elettrico** no, perchè
  il fan-out sul vetro può incrociare le linee. Da qui il default `setSlaveMirror(true, true)` e
  il fatto che resti configurabile.
- **Ribaltamento della banda nel data path**, non nei registri: l'SSD1677 non ha reverse scan
  hardware (`0x01` bit TB = 1 è Reserved). `ScreenPart` porta `_mirror_x`/`_mirror_y` e
  inverte ordine righe nella banda, ordine byte nella riga e bit nel byte (`_reverseBits`),
  riposizionando la finestra RAM in modo simmetrico. Default: master normale, slave 180°;
  `setMasterMirror()` / `setSlaveMirror()` provano le combinazioni dallo sketch.

**SECONDA CODA MUTA.** Lo stesso cablaggio spostato sull'altro FFC non aggiorna nulla.
Ipotesi in ordine, da distinguere con una sola sessione di multimetro (mappa GND/VCI su
entrambe le code + verifica che i pin di boost siano cablati anche sul secondo attacco):

1. **ordine dei pin ribaltato**: le code escono da bordi opposti, se sono la stessa parte la
   seconda è ruotata di 180° e il pin *n* cade sul pin *N+1-n*;
2. **rail di boost non portati** sul secondo attacco → logica sì, elettroforesi no;
3. **BUSY o RST fuori posizione** → il driver resta appeso e nessun comando arriva.

L'ipotesi "lo slave non genera le alte tensioni e dipende dal master" è **indebolita** dalle
foto del tag di fabbrica: due reti di boost identiche, una per coda, mentre 9.7" e 11.6"
single-controller ne hanno una sola.

**OEPL, stato corrente** (`Shared_OEPL_Definitions` letto live, non lo snapshot in `docs/`):
niente 12.2". Le taglie grandi arrivano a `STYPE_SIZE_116` 0x65, `STYPE_SIZE_116B` 0x4A,
`STYPE_SIZE_116_BWRY` 0x7D, più `STYPE_SIZE_075_BWRY` 0x7B. Rilevante anche per il 9.7": OEPL ha
aggiunto varianti BWRY per 7.5" e 11.6" e continua a non averne una per il 9.7"
([[gxepd2_097c_driver]]).

**Ancora aperti**: part number esatto del controller (COF schermato, nessuna serigrafia). Si
chiuderebbe leggendo `0x2E` (User ID da OTP), che pretende un SDO: il connettore interno della board
non lo porta (solo SDI, verificato sullo schematico), ma la coda cablata a mano forse sì — se lo
porta, collegarlo a GPIO12 rende possibile la lettura, con il caveat che GPIO12 è un pin di
strapping ([[waveshare_esp32_driver_board]]).
Restano aperti anche:
quale banda sta su quale coda; se lo slave vada davvero specchiato e su quali assi;
`full_refresh_time` (25000 ms nel codice, da allineare al tempo misurato); LUT (OTP).
Il driver compila in entrambe le vie: example `12_2c/dual_panel_finder` (fase driver) e firmware
in variante `DISPLAY_VARIANT_122C` (1 359 044 B flash, 81 592 B RAM globali).

**Evidenze in repo**: `GxEPD2_SOLUM_ESL/docs/` e `examples/` sono separate per pannello — `122c/fcc/` tiene le
foto interne FCC del 12.2" e quelle dell'11.6" usate come confronto, `097c/fcc/` quelle del
9.7", la radice quello che vale per entrambi (SSD1677, datasheet Newton, board Waveshare,
sorgenti OEPL). Block diagram, schematici e operational description delle
stesse pratiche sono coperti da confidentiality letter: non ottenibili. I datasheet di
prodotto (`docs/Newton-*.pdf`) sono marketing e non dichiarano il controller.

**Niente giallo.** `preserveYellow`, `isYellowPreserved`, `writeImageYellow` sono no-op: le
pretende il template `showImage()` condiviso e le chiamano i moduli applicativi scritti per il
9.7" senza sapere quale pannello è montato. Il ramo `FORMAT_BWRY_1BPP` non ci arriva mai, lo
esclude il formato del descrittore.

**Bus SPI**: passa da `_pSPIx` / `_spi_settings` della base `GxEPD2_EPD`, ScreenPart comprese —
che ne tengono un **riferimento**, non una copia, così un `selectSPI()` vale anche per loro. I
costruttori impostano come default `SPI` globale a 20 MHz; lo sketch lo sostituisce chiamando
`selectSPI()` prima di `init()`. Il driver apre il bus da sè dentro `init()` → `_initSPI()`,
per questo il pinout gli passa anche `sck`/`miso`/`mosi`.

**Costruttori**: ESP32 a 9 pin (sck, miso, mosi, cs_m, cs_s, dc, rst, busy_m, busy_s), 6 pin
(senza bus), single-CS a 4 pin per il bring-up con una sola coda cablata (`cs_s = -1`, le
scritture verso S sono no-op), e quello a `GxEPD2_SOLUM_Pins` che delega ai precedenti secondo
i campi valorizzati.

**Hardware di bring-up**: Waveshare E-Paper ESP32 Driver Board, FFC #1 nel connettore interno
(CS=GPIO15, BUSY=GPIO25, SCK=13, MISO=12, MOSI=14, DC=27, RST=26) + breakout 24-pin esterno per il
secondo controller (BUSY_S=GPIO35, gli altri segnali in parallelo). Caratteristiche della board,
pinout completo e caveat in [[waveshare_esp32_driver_board]]. Le code sono a 21 pin per misura
fisica (il conteggio dei contatti dalle foto FCC dà ~24 ma è una stima con errore di scala:
prevale la misura sul cavo).

**CS_S = GPIO33 è da sostituire.** Nello schematico della board quel GPIO non compare fra le net
(ci sono 32, 34, 35 su pin consecutivi dell'header), quindi quasi certamente non è portato fuori:
il candidato è **GPIO32**, output-capable, sull'header, non usato da nient'altro e adiacente al 35.
Lo stato attuale è disallineato di proposito, in attesa di decisione: il cablaggio documentato dice
ancora 33 in `README_122c.md`, `docs/122c/connessioni.html`, i due SVG di `docs/122c/`, i commenti
d'esempio di `src/GxEPD2_SOLUM.h` e `src/GxEPD2_SOLUM_Pins.h`, e in `A:\epd\Layout_122c.h`
(`PIN_CS_S`, che è firmware di produzione). Nel finder il valore è ancora 33 ma la sezione dei pin
porta l'avvertenza: se la coda lunga non risponde, il pin è la prima cosa da verificare, prima del
pannello.

**Sketch**. `examples/12_2c/dual_panel_finder` è una sonda **a SPI diretta** (nè GxEPD2 nè il
driver custom, come `examples/097c/panel_diagnostic`), su una coda alla volta. Tre switch a
compile time, tutti in un blocco IMPOSTAZIONI in testa al file chiuso da FINE IMPOSTAZIONI, con
ogni sezione marcata [OBBLIGATORIO] / [OPZIONALE] e il suo costo in tempo: `TEST_TARGET` (coda corta
= quella che risponde, connettore interno / coda lunga = breakout), `INIT_CANDIDATE`
(`CAND_MINIMAL` stile 1160c, `CAND_SOLUM` stile 097c/1330c, `CAND_OEPL` init di fabbrica OEPL 9.7"
con i pattern `0x46`/`0x47` e `0x21` = 08 00 che lì raddrizza l'immagine — da ripetere una volta per
candidata, il confronto è il punto), `MUX_LINES` (384 misurate / 680 POR per confrontare i tempi),
più gli opzionali `SHOW_SOLID_COLORS` (+3:15) e `SHOW_FAST_CLOCK_FRAME` (+1:05) e `OBSERVE_MS`
(pausa fra i frame, 10 pause pesano 7:30). Durata con i default **circa 11 minuti**: 7 refresh + 6
pause nel probe, 4 + 3 nella fase driver; giro minimo utile 4 refresh, ~1:20. Il banner a runtime
stampa quali fasi e quali opzionali sono attivi. Il pattern è
composto riga per riga tutto allineato al byte: cornice, blocco nero nell'origine RAM, blocco
accent nell'angolo opposto in X, righelli numerati X e Y con font 5x7 scalato, scaletta diagonale,
lettera della coda, barra accent sulle ultime 64 righe della banda attesa. Poi altri frame: **4
bande numerate** con le quattro combinazioni dei piani (dice se l'accent di questo film è rosso o
giallo, e se la coppia BW=0/RED=1 è un quarto stato) e i **colori pieni** bianco/nero/accent
(uniformità e ghosting), con pausa di osservazione fra i frame perchè ognuno cancella il
precedente (`OBSERVE_MS`, `SHOW_SOLID_COLORS`).

Misura senza guardare un pixel — è la parte che vale sulla coda muta: pattern hardware
`0x46`/`0x47` come prova di vita che non dipende dal push SPI, power on `0x22`=0xC0, **HV Ready
Detection `0x14` = 0x77** (cool down 80 ms x 7 cicli, massimo 560 ms: il datasheet dice che la
detection si conclude *quando HV è pronta*, quindi un BUSY molto più corto del massimo = alte
tensioni presenti, un BUSY che arriva al massimo = mai arrivate), **VCI Detection `0x15`**, e i
registri in lettura `0x2F` / `0x2E` / `0x1B` con `0x2F` (POR 0x01) come prova di validità del
percorso — i suoi **bit 5 e 4 sono i flag HV Ready e VCI**, cioè l'esito esplicito invece che
dedotto dai tempi. Sul 9.7" la linea di lettura non esiste sul FPC 24 pin; sulla coda 21 pin del
12.2" è da vedere, e se rispondesse `0x2E` (User ID da OTP) chiuderebbe la questione del part
number del controller.

Misura anche: gate line reali (ultima etichetta Y leggibile), verso della banda su entrambi gli
assi, tempi per piano e per refresh con ms per gate line, BUSY dell'altra coda a riposo e durante
il refresh, **BUSY della coda sotto test a riposo prima di ogni comando** (se è alto tutto il resto
del report non vale: pin flottante o coda non alimentata), **finestra parziale con x diverso da
zero** (tre box a x = 0/448/896, l'unico frame che esercita il percorso di `_setPartialRamArea`),
power on e power off cronometrati (tarano `power_on_time`/`power_off_time`), **entrambi i parametri
di deep sleep** 0x03 e 0x11 col BUSY come testimone (decide il byte di `hibernate()`), e la
ripetizione del pattern a **20 MHz** per validare il default di `selectSPI` del driver.
Fuori scope di proposito: DISPLAY Mode 2 / 0xFC (sul 9.7", stesso silicio, è già stabilito che la
seconda RAM è l'accent, quindi `hasFastPartialUpdate = false` è una conseguenza e non
un'assunzione), varianti di `0x3C` e LUT via `0x32`. Chiude con una scheda di osservazione esito -> conseguenza che copre tutte queste
voci. Il primo bring-up era stato fatto con lo stock
`GxEPD2_1160c_GDEY116Z91`, che comanda 640 righe e sul pannello ne materializza 384.
**Lo sketch è uno solo per il 12.2"**: `color_cycle` è stato eliminato e assorbito nella seconda
fase del finder. Le due fasi si accendono separatamente con `RUN_PROBE_PHASE` / `RUN_DRIVER_PHASE`
(più `DRIVER_DUAL` per costruire il driver col solo master): la fase probe misura il **silicio** a
SPI diretta su una coda alla volta, la fase driver costruisce `GxEPD2_SOLUM_DRIVER_CLASS`
dall'ombrello e misura il **driver** su entrambe le code — `clearScreen()` sui tre colori e un
frame di 5 tile 64x64 in PROGMEM di cui uno a cavallo di `PART_HEIGHT`, che è il test del dispatch
per righe. Se il probe stampa e la fase driver no, il difetto è nel driver: è la distinzione per
cui le due fasi stanno nello stesso sketch. Con `DRIVER_DUAL 0` il master della fase driver **segue
`TEST_TARGET`** invece di essere fisso su CS=15: con una coda alla volta le due fasi devono parlare
allo stesso silicio, altrimenti provando la sola coda lunga il driver piloterebbe un attacco non
collegato e sembrerebbe rotto. Compila in tutte le combinazioni di fasi
(298 036 B flash con entrambe attive).

**I fix del 097c ora sono portabili**: i due driver condividono command set oltre che
infrastruttura (`GxEPDImage` in un header a parte, bulk-SPI con `writeBytes`, page-hint di
`showImage`, dirty flag dell'accent, `_cleanAccentIfDirty`). Le differenze che restano sono il
piano giallo `0x28`, che qui non esiste, e la geometria per controller.

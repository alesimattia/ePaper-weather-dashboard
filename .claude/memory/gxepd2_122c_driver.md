---
name: Driver custom GxEPD2_SOLUM_122c_960x768 (pannello SOLUM 12.2")
description: Il 12.2" nel submodule - identità Newton PRO EL122H6W4A, controller SSD16xx 2x960x384 con split sull'asse corto, command set e geometria già allineati nel codice, mirror della banda nel data path, seconda coda FFC muta e le quattro ipotesi (compreso il secondo controller indirizzato con opcode|0x80 senza secondo CS), la CASCADE ESCLUSA dal datasheet (raddoppia le sorgenti, non i gate) e la topologia a due controller indipendenti a rail condivisi, con GxEPD2_1248c come base architetturale giusta e 579c_Z93 come contro-modello, come si decodifica il codice modello SOLUM, dove stanno le evidenze FCC
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
(i controller sono i COF alla base delle due code), e **una sola rete di boost, accanto a un solo
dei due connettori FFC**: l'altro è nudo (vedi la sezione sulla cascade).
Famiglia: il 9.7" è **BWRY**, l'11.6" e il 12.2" sono **BWR** (etichette dei pannelli).

**LA CORRELAZIONE CHE CHIUDE L'ARCHITETTURA**, dal datasheet SOLUM Newton PRO §3.1 letto su tre
taglie invece che su una:

| Taglia | Datasheet, verbatim | Asse gate | Code |
|---|---|---|---|
| 9.7" | `672 x 960 Pixel (121dpi) / 141.1 x 201.6 mm` | 672 | una |
| 11.6" | `640 x 960 Pixel (100dpi) / 163.0 x 244.5 mm` | 640 | una |
| **12.2"** | `768 x 960 Pixel (102dpi) / 190.1 x 237.6 mm` | **768** | **due** |

Pitch 0,2475 mm su tutte e tre, e **solo il 12.2" sfora i 680 gate dell'SSD1677 — ed è l'unico con
due COF**. Le altre due stanno sotto e hanno una coda sola. Non è una deduzione dal tetto dei gate:
è una correlazione completa fra i numeri del produttore e il conteggio delle code.

**La disposizione, dalla foto** (`docs/122c/fcc/122_pannello_retro.jpg`, col metro accanto): il
vetro misura ~190 × 237 mm, e le due code escono dai **due bordi lunghi opposti alla stessa
altezza**, ognuna con il **proprio COF** — la barra metallica alla base è visibile su entrambe.
Quindi non c'è un COF passivo alimentato dall'altro: **sono due driver IC**. Il bordo di bonding è
quello da 237,6 mm, cioè l'asse da 960 px = le 960 **source**; l'asse perpendicolare, 768 px, si
divide in **384 gate per controller**. E i due COF si guardano da bordi opposti, quindi il secondo
è ruotato di 180°: la specchiatura è un effetto atteso, non un'ipotesi. Che la banda dipinta sia
quella **adiacente alla propria COF** — osservato — è ciò che questa disposizione predice, ed è
incompatibile con uno split di sorgenti.

**Temperatura di esercizio: `32°F ~ 104°F (0°C ~ 40°C)`** per il pannello BWRY (§3.1). Lo zero è il
**pavimento di specifica**, non un estremo, e allo zero il 9.7" sullo stesso silicio ha misurato
**59067 ms** di refresh — la waveform dell'OTP si allunga di 2,5x verso il freddo. È il caso
peggiore *in specifica* che `full_refresh_time` e `busy_timeout` devono coprire.

**Il quarto colore e il codice modello.** §3.6 decodifica il campo ⑤ come **"4 = RED, YELLOW
(BWRY)"** e lista il 12.2" PRO come `EL122F6W4A`. Ma la riga *Display Colors* di §3.1 porta la nota
del produttore stesso — **"color options are not available for all sizes"** — quindi il campo è una
designazione di **linea**, non del film montato, e la nota lo dice. Il dato per esemplare è la
serigrafia sul vetro: `Newton PRO 12.2" BWR normal`. Il frame a bande non può chiudere la questione
da solo, perchè la Table 6-4 aliasa LUT3 su LUT2 e le due combinazioni con l'accent acceso rendono
lo stesso colore per costruzione: risponde il probe dei livelli di sorgente, che pilota LUT2 a VSH1
e LUT3 a VSH2.

**GEOMETRIA E CONTROLLER: RISOLTI, e il driver è allineato.**

- Ogni controller pilota **960 × 384**, split sull'**asse corto** (i 768 px). Con ESP32 su una
  sola coda FFC si stampa correttamente un rettangolo 960×384 — ma quella osservazione è
  **circolare** sul conteggio gate: è stata ottenuta CON IL MUX PROGRAMMATO A 383, quindi conferma
  che il MUX funziona, non quante gate line il controller piloti. Il conteggio vero lo legge la
  sonda `0` del finder, con un righello e il MUX spazzato fino fuori specifica.
- In coordinate driver (`WIDTH 960` = source, `HEIGHT 768` = gate) le metà sono **bande
  orizzontali**: righe 0..383 / 384..767. `PART_WIDTH`/`PART_HEIGHT` = 960/384 e il dispatch
  trasla lo slave di `y - M.HEIGHT`.
**Costanti e comportamenti attuali del driver**, con la ragione di ognuno:

| Voce | Valore | Perchè |
|---|---|---|
| `power_on_time` / `power_off_time` | 100 / 250 ms | misurati 82 e 221 sul controller che risponde |
| `full_refresh_time` / `partial_refresh_time` | **60000 ms** | 18,3 s a ambiente, ma il margine si prende sul pavimento di specifica: 0 °C, dove il 9.7" misura 59067 ms. È anche il `delay()` di ripiego quando il pin BUSY non è cablato |
| `busy_timeout` (ai costruttori) | **120 s** | il doppio del peggiore misurato in specifica |
| `hasFastPartialUpdate` | `false` | misurato: `0xFC` su 64 e 24 righe dà 18167 e 18170 ms contro i 18308 del frame intero. E su SSD16xx a tre colori il piano `0x26` è il rosso, non il buffer *previous* |
| `hasPartialUpdate` | `true` | vale l'indirizzamento a finestra, non un refresh più corto |
| `isYellowPreserved()` | `false` | non c'è un terzo piano da preservare: le tre primitive del giallo restano senza corpo |
| `hibernate()` | `0x10 = 0x03` più `_initial_write = true` | con quel parametro la RAM non sopravvive ("Cannot retain RAM data"), e sul 9.7" al risveglio i piani contengono pixel casuali. L'alternativa `0x10 = 0x01` — Mode 1 del SSD1683, misurato sul 9.7" — ritiene la RAM a ~2 µA in più, e renderebbe inutile il riarmo |
| `_InitDisplay()` in cascade | init mandato **due volte**, nuda e con `\| CASCADE_CMD_OFFSET` | `_writeCommandAll()` in cascade abbassa il solo `_cs_m` e trasmette l'opcode nudo: senza il secondo giro lo slave non vedrebbe mai SWRESET, soft start, MUX, border, temperatura nè entry mode. `0x21` col secondo byte va invece al **solo master**, perchè arma lui a emettere CL |
| `ScreenPart::_setPartialRamArea` | riposizionamento simmetrico con guardia | la sottrazione è su `uint16_t`: una finestra che sfora andrebbe in underflow silenzioso |
| clock SPI dei costruttori | 10 MHz | scelta, non limite: il datasheet dà *"Maximum 20MHz for write"* |

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
- **Ribaltamento della banda nel data path**, che è una scelta di implementazione e non un
  obbligo: quello che manca è la reverse scan delle **gate** (`0x01` bit TB = 1 è Reserved), mentre
  il verso del **contatore di indirizzo** è configurabile da `0x11` A[1:0] su entrambi gli assi.
  Specchiare col solo entry mode è la strada che usa GxEPD2 sul GDEY0579Z93 (entry mode diverso per
  chip più coordinate rimappate, nessun dato toccato) e risparmierebbe reverse di byte e bit per
  riga; resta da verificare sul pannello che l'ordine dei bit dentro il byte torni da sè. `ScreenPart` porta `_mirror_x`/`_mirror_y` e
  inverte ordine righe nella banda, ordine byte nella riga e bit nel byte (`_reverseBits`),
  riposizionando la finestra RAM in modo simmetrico. Default: master normale, slave 180°;
  `setMasterMirror()` / `setSlaveMirror()` provano le combinazioni dallo sketch.

**SECONDA CODA MUTA: è la coda dello SLAVE di una coppia in CASCADE, e da sola non può
funzionare.** Il firmware di produzione stampa già correttamente la metà servita dalla coda che
risponde; lo stesso cablaggio spostato sull'altro FFC non aggiorna nulla, ed è il comportamento
previsto.

**LA CASCADE È ESCLUSA SU QUESTO PANNELLO, e la esclude il datasheet.** SSD1683 §6.12, letto per
intero: *"The SSD1683 has a cascade mode that can cascade 2 chips to achieve the display resolution
up to **800 (sources) x 300 (gates)**"*. Il chip singolo è 400x300: la cascade **raddoppia l'asse
SOURCE e lascia i gate dove sono**. Due argomenti indipendenti convergono:

1. **aritmetica**: una coppia in cascade di chip di questa famiglia darebbe 1920 source x **680
   gate**, e 680 < 768 — non copre l'asse gate del 12.2" per nessun cablaggio;
2. **osservazione**: con la coda lunga sola il pannello dipinge una banda larga **960 px pieni**,
   mentre in cascade il master coprirebbe metà delle sorgenti, cioè 480 px.

Lo split è quindi sull'asse gate, e la topologia sono **due controller INDIPENDENTI**, ciascuno col
proprio oscillatore, che condividono i **rail di pilotaggio** — che il datasheet SSD1677 dà per
alimentabili dall'esterno (Features p.5 e Table 5-4: *"VGH, VGL, VSH1, VSH2, VSL can be connected
to external power supply"*). È la spiegazione del secondo connettore nudo che le evidenze qui sotto
descrivono, e fa una previsione che DISCRIMINA: uno slave in cascade ha il BUSY fermo a tutto,
perchè non ha oscillatore; un chip indipendente senza rail RISPONDE ai comandi (SWRESET, `0x47`,
CRC `0x34`) e non dipinge, e la sua `0x14` HV Ready parte e va al massimo. La misura è la sonda `4`
di `examples/12_2c/dual_panel_finder`.

**Conseguenza sul driver**: `ADDRESSING_DUAL_CS` non è "il modello storico da sostituire", è quello
che il datasheet sostiene; e il ramo `ADDRESSING_CASCADE` poggia su un meccanismo che non si
applica. Conseguenza sulla base upstream: `GxEPD2_579c_GDEY0579Z93` è il **contro-modello**, non il
modello — è 792x272, cioè 2x396 SORGENTI x 272 gate, un pannello a split di sorgenti servito da un
CS unico proprio perchè è una coppia in cascade. L'architettura giusta per uno split di gate è
quella che il driver ha già, e viene da `GxEPD2_1248c`: l'unico modello upstream a controller
indipendenti con split a bande. Di `1248c` non va solo il command set, che è UC8179, e il driver lo
prende già da `GxEPD2_1330c_GDEM133Z91`. Del 579 resta utile una sola cosa come TECNICA: il suo
`_setPartialRamArea` a quattro entry mode.

Il meccanismo della cascade, per riferimento: il pin `M/S#` fissa il ruolo (VDDIO master, VSS
slave); **nello slave oscillatore e booster/regolatore sono disabilitati**, e clock CL più tutte le
tensioni **devono arrivare dal master**. Il master si arma dal registro **`0x21`, secondo
parametro, bit B[4] *ckouten*** (*"0: Single chip application, 1: Cascade application"*, con la
nota *"For cascade mode, connect CL pin between Master sample with Slave sample"*).

**Quel bit è scrivibile, e la misura viene dal 9.7".** Sul silicio gemello `0x21` nella forma a due
byte **funziona**, verificato sul vetro: `{08 00}` manda lo schermo in negativo e `{40 00}` fa
sparire l'accent, che sono esattamente le due opzioni del primo byte. Di per sè non riapre niente —
l'esclusione della cascade su questo pannello poggia sull'aritmetica dei gate e sui 960 px dipinti
da una coda sola, e nessuna delle due dipende da `0x21` — ma **toglie un dubbio**: se un giorno si
volesse provare l'armamento, il registro risponde e il meccanismo esiste. Il caveat è che la misura
è del 9.7", quindi vale finchè i due controller sono della stessa famiglia.
Dettaglio in [[ssd1677_command_set]].

Le evidenze che seguono descrivono l'hardware — un connettore col boost e l'altro nudo, un solo CS
sul tag, `opcode|0x80` in OEPL — e vanno lette come **rail condivisi**, non come cascade.

Evidenze, dalla più diretta:

1. sul tag di fabbrica **un connettore FFC ha tutta l'elettronica analogica** (switcher, induttore,
   diodi, condensatori) e **l'altro è nudo**, con un fascio di piste che arriva dal primo —
   verificato sulle foto PRO (`docs/122c/fcc/122_pcb_ffc_con_boost.jpg` contro
   `122_pcb_ffc_secondo.jpg`) e su quelle Core a 4032x3024. **Questa è la correzione di una
   conclusione precedente**: non ci sono due reti di boost, ce n'è una sola;
2. l'HAL del tag nRF52811 ha **un solo `EPD_CS`** per il pannello: due chip su un CS non sono
   separabili se non per opcode ([[oepl_nrf52811_tag_fw]]);
3. `dualssd.cpp` di OEPL indirizza il secondo chip con **opcode|0x80** e scrive `0x21` con
   **B = 0x10**, che è esattamente il bit di cascade;
4. GxEPD2 upstream fa lo stesso per il Good Display **GDEY0579Z93** (5.79" 792x272, SSD1683, due
   chip): `A:\tmp\GxEPD2-master\src\gdey3c\GxEPD2_579c_GDEY0579Z93.cpp`.

Conseguenza sul cablaggio: il secondo CS **serve** (GPIO32, perchè il 33 non è portato fuori sulla board Waveshare), perchè i due
controller sono indipendenti e ognuno va selezionato. Il ponte fra le due code serve solo per i
**cinque rail di pilotaggio** — VGH, VGL, VSH1, VSH2, VSL — e **`CL` non serve**, perchè ogni chip
ha il proprio oscillatore e il datasheet lo dà *"should be left open in application"*. L'ipotesi cascade si esclude col multimetro: continuità di `M/S#` verso VDDIO o VSS su ciascuna
coda. Il suo cablaggio sarebbe un solo CS/SCK/MOSI/DC/RST/BUSY condivisi più un **ponte passivo fra
le due code** che portasse CL e i rail dal
connettore master a quello slave, cioè quello che fa il tag di fabbrica. `BS1` basso su entrambe.

Restano possibili, ma spiegano solo il silenzio e non l'assenza di secondo CS e secondo boost sul
tag: ordine dei pin ribaltato sulla seconda coda (le code escono da bordi opposti, il pin *n* può
cadere sul pin *N+1-n*), BUSY o RST fuori posizione. Si distinguono con una sessione di
multimetro: mappa GND/VCI su entrambe le code e continuità dei rail fra i due FFC.

**Base per riscrivere il driver**: `GxEPD2_579c_GDEY0579Z93` di GxEPD2, non più lo scheletro del
1248c (che è UC8179 con CS separati — come `gdem/GxEPD2_1085_GDEM1085T51`, l'altro modo, quello
sbagliato per questa famiglia). Da lì si prende: init in broadcast senza offset (`0x12`, `0x18`,
`0x1A`, `0x22`=0xB1 + `0x20`), `_setPartialRamArea(..., target)` con target 0x00/0x80 che offsetta
`0x11/0x44/0x45/0x4E/0x4F`, piani `0x24|target` / `0x26|target`, refresh e power on in broadcast, e
soprattutto **il mirror di una metà fatto con l'entry mode** (`0x11` = 0x02 su un chip, 0x03
sull'altro) invece che ribaltando byte e bit nel data path come fa oggi il nostro driver.

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
Restano aperti anche: se lo slave vada davvero specchiato e su quali assi, e quale banda sta su
quale coda.
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
costruttori impostano come default `SPI` globale a **10 MHz** — era 20, abbassato perchè alcuni
SSD1677 non tollerano clock superiori e con una sola banda validata un clock fuori specifica è una
variabile in più; costa ~130 ms su un refresh da 25 s. Lo sketch lo sostituisce chiamando
`selectSPI()` prima di `init()`. Il driver apre il bus da sè dentro `init()` → `_initSPI()`,
per questo il pinout gli passa anche `sck`/`miso`/`mosi`.

**Indirizzamento selezionabile a runtime**: `enum AddressingMode { ADDRESSING_DUAL_CS,
ADDRESSING_CASCADE }` + `setAddressingMode()`, default `DUAL_CS` (comportamento storico). Va
chiamata **prima di `init()`**, che in base al modo decide quali pin configurare. In cascade: la
`ScreenPart` slave passa sul chip select del master e somma `CASCADE_CMD_OFFSET` (0x80) ai propri
opcode — cioè esattamente i comandi per-controller `0x44`, `0x45`, `0x4E`, `0x4F`, `0x24`, `0x26`,
perchè quelli sono gli unici che la ScreenPart emette; `_writeCommandAll`/`_writeDataAll` abbassano
un solo CS e restano **senza** offset (convenzione di `dualssd.cpp`); `_InitDisplay()` aggiunge
`0x21 = 08 10` per mettere il master in cascade; `init()` non pilota `_cs_s` e `_waitWhileAnyBusy`
ignora `_busy_s`. Effetto collaterale voluto: in cascade `S.isActive()` è vero anche con `cs2 = -1`,
perchè un secondo CS non serve. La costante **non** si chiama `SLAVE_CMD_OFFSET`: quel nome è già
una macro in `dual_panel_finder.ino` e la qualificazione di classe non compilerebbe. Lo sketch
sceglie il modo dalla voce `f` del menu e dalla candidata di init `per-CS` (voce 6 del tasto `i`),
non da un flag di compilazione.

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
driver custom, come `examples/097c/panel_diagnostic`), su una coda alla volta, ed è strutturata
come il gemello 9.7": un `.ino` corto più otto header.

```
dual_panel_finder.ino   tabella delle sonde, sottomenu, runner, scheda DRV, menu
Config.h                configurazione di sessione, limiti del silicio col riferimento al
                        datasheet, input seriale bloccante, waveformGuard()
Report.h                frame numerati, gate, registro delle misure, log a tag
Graphics.h              font 5x7, riquadro del numero, composizione righe. NON tocca SPI
Controller.h            bus a 4 e 3 fili, init e sei candidate, refresh, registri, LUT.
                        NON conosce le sonde
ProbesElettriche.h      le sonde che non spendono un refresh
ProbesFrame.h           le sonde che dipingono un frame e lo fanno guardare
ProbesWaveform.h        waveform, LUT, temperatura, sonno, secondo controller
ProbesDriver.h          la fase che misura il driver invece del silicio
```

**Nessuno switch a compile time**: una compilazione copre un'intera sessione. Dal menu si
scelgono candidata di init (tasto `i`, sei varianti), coda, secondo FFC, esaustivo, polarità e
dettagli; e dalla voce `p` temperature, i quattro MUX, gate per controller, i tre clock, timeout,
finestra del multimetro, più i **parametri di registro** alle voci 10..15 — livello del soft start
`0x0C`, entry mode `0x11`, TB, i tre campi di `0x21` con la sua posizione (mai / init / prima di
`0x22`), durata e tentativi del reset, border `0x3C` sotto LUT custom. Le combinazioni che una
singola sonda spazza da sè non stanno nel menu, e non è una mancanza: i quattro entry mode, i
valori di MUX, le due strade delle tensioni della LUT, gli stadi di `0x22` e i due modi del bus li
sceglie la sonda, perchè il confronto fra loro **è** la misura. `loop()` legge **un carattere** e chiama
`handleKey()`; i sottomenu e le domande del gate leggono una riga. Niente NVS: la configurazione
vive quanto la sessione, e la voce `k` ridichiara quello che si sa già.

**Log a tag in colonna fissa**, come il 9.7": `MEAS` una misura, `guarda:` cosa cercare sul vetro,
`SEEN` la risposta, `VERDICT` una conclusione, `DRV` una riga da cambiare nel driver. Il tasto `v`
accende le righe di dettaglio, spente di default. Il numero fra parentesi quadre è il **riquadro in
alto a destra** dell'area ridipinta e sta sul FRAME, non sulla sonda; le passate che scrivono una
cifra dentro l'immagine prenotano il numero con `prenotaNumero()` prima di dipingere, così la cifra
sulla fascia e il riquadro portano lo stesso numero. Prima del primo frame il prefisso non si
stampa. Unica eccezione documentata: la sonda della polarità, dove le cifre 1 e 2 sulle fasce sono
etichette di POSIZIONE e il log lo dichiara.

**23 sonde, 30 refresh minimi (~9 min), 41 in esaustivo (~13 min)**, e la selezione di default
esegue tutto. Le sei elettriche non spendono refresh: candidate, bit 7, clock, **impronta della
coda** (indicizza per coda BUSY a riposo con e senza pull, `0x47`/`0x46`, power on, HV Ready `0x14`,
VCI, CRC `0x34`, e stampa le due colonne affiancate — è la sonda che discrimina rail condivisi da
cascade), **SPI a 3 fili** (`transferBits` a 9 bit: se in 3 fili il chip risponde, BS1 flotta alto e
il rimedio è un pull-down, non un ponte), **reset a ritentativi** (come OEPL, contro l'impulso
singolo del driver). Quelle sul pannello intero: `b` opcode|0x80, `0` **identità del die** (righello
con MUX e finestra fino a 767, fuori specifica di proposito), `j` **TB = 1** (reverse scan gate in
hardware: se funziona, `_reverseBits` si cancella), `e` **entry mode** (quattro fasce in un frame,
con un pattern che ha struttura SOTTO il byte, perchè il datasheet non dice cosa accada ai bit
dentro un byte scritto). Più `q` **bitmask di `0x22`**, che spegne uno stadio per volta da `0xF7`.

Tre sonde spendono meno di prima perchè rimisuravano ciò che il 9.7", stesso silicio, ha già
chiuso: MUX cronometrato 3 -> 0 refresh, partial d'area 5-7 -> 2, differenziale 3-6 -> 1. Le
passate restano, in esaustivo. Il **ping-pong** invece è corretto e non ridotto: `0x37` A[7:0] è
riservato e va a zero, `F[3:0]` è il Display Mode di WS[35:32], e il datasheet aggiunge *"RAM
ping-pong function is not support for Display Mode 1"* — la versione precedente scriveva `0xFF` sul
byte riservato e lasciava `F[3:0]` a zero, cioè chiedeva una combinazione dichiarata non
supportata.

**Lo sketch è uno solo per il 12.2"**, e le due fasi stanno dentro la stessa tabella delle sonde.
La fase probe misura il **silicio** a SPI diretta, su una coda alla volta; la **fase driver** (tasto
`r`) costruisce `GxEPD2_SOLUM_DRIVER_CLASS` dall'ombrello e misura il **driver** — fondo bianco con
accent spento, cinque tile 64x64 in PROGMEM di cui uno a cavallo di `PART_HEIGHT`, che è il test
del dispatch per righe, e la barra accent sul bordo basso della banda del master. Se il probe
stampa e la fase driver no, il difetto è nel driver: è la distinzione per cui le due fasi stanno
nello stesso sketch.

Il driver si costruisce alla **prima** chiamata di `driver()` e non prima, perchè i suoi pin
dipendono dalla configurazione: senza secondo connettore `cs2` e `busy2` devono restare a -1,
altrimenti aspetterebbe un BUSY su un GPIO35 flottante e andrebbe in timeout. `driverBegin()`, in
`ProbesDriver.h`, installa il gancio di avanzamento una volta sola da `setup()`; sta là e non nel
`.ino` perchè il preprocessore di Arduino inserisce i prototipi generati prima del primo include, e
una funzione del `.ino` che ritorna quel tipo non compilerebbe. Flash occupata dallo sketch:
**~343 KB**, il 26% di `huge_app`.

**I fix del 097c ora sono portabili**: i due driver condividono command set oltre che
infrastruttura (`GxEPDImage` in un header a parte, bulk-SPI con `writeBytes`, page-hint di
`showImage`, dirty flag dell'accent, `_cleanAccentIfDirty`). Le differenze che restano sono il
piano giallo `0x28`, che qui non esiste, e la geometria per controller.

## Ipotesi 4 sulla coda muta: il secondo controller si indirizza con opcode|0x80

Dal firmware OEPL dei tag nRF52811, che è la famiglia di questo pannello ([[oepl_nrf52811_tag_fw]]):
il loro `dualssd.cpp` pilota un pannello a **due controller SSD con un solo CS**, sommando **0x80
all'opcode** per il secondo (`0x11→0x91`, `0x44→0xC4`, `0x45→0xC5`, `0x4E→0xCE`, `0x4F→0xCF`,
`0x24→0xA4`, `0x26→0xA6`); comandi comuni scritti una volta sola. Un solo BUSY, un solo RST.

Se i due COF del 12.2" sono strappati master/slave allo stesso modo, il secondo controller risponde
**solo** agli opcode con bit 7 alto: niente CS_S, niente GPIO32. **Attenzione a cosa questo non
implica**: indirizzare lo slave non basta a farlo stampare, perchè in cascade non ha nè oscillatore
nè booster (vedi la sezione sulla coda muta). Il test dell'offset da solo, su una coda sola, può
dire soltanto se il chip di quella coda è lo slave; per vedere un pixel servono anche il ponte dei
rail e `0x21` B[4] = 1 sul master. **Il finder la implementa** nella sonda `b`, `probeSlaveByOpcodeOffset()`, che scrive il pattern
di identificazione con gli opcode offset e lancia il refresh; `0x21` col secondo byte va **sempre
al master, senza offset**, perchè è il bit B[4] ckouten e chiederlo allo slave gli chiederebbe di
emettere un clock che deve invece ricevere. Il broadcast dei comandi comuni e il chip select unico
(`csBoth`) sono parametri della sonda, e il secondo CS ha senso solo su un pin che esiste: 32, non
33.

Su questo pannello però il bit 7 è il meccanismo della **cascade**, che il datasheet esclude: un
esito negativo della sonda `b` è quindi **atteso** e conferma il quadro invece di lasciare la
domanda aperta.
**Esito negativo = nessuna informazione** finchè non c'è il ponte dei rail: lo sketch lo dice sia nel
banner sia nella scheda di osservazione. La sonda configura **anche il master** (soft start, MUX,
entry mode, border, sensore) perchè è la sua master activation a chiudere la sequenza e la sua banda
viene scandita comunque; allo slave manda MUX, entry mode, finestra e dati — i due riferimenti gli
mandano solo entry mode e dati, ma là lo split è sulle sorgenti e le gate sono comuni, qui no.

**L'avvertenza è nell'header del driver e in `README_122c.md`** (banner in testa, §3 e §5), che
sono stati anche ripuliti da tutta la descrizione del vecchio driver UC8179 — sequenza `0x00`/`0x06`/`0x61`,
split per colonne, `writeImageBlack` su `0x10`: roba che il codice non fa da tempo: il modello a due chip select di
`GxEPD2_SOLUM_122c_960x768.h` potrebbe essere sbagliato, e cosa cambierebbe se il finder conferma la
cascade. Il modello non è più cablato nel file: `setAddressingMode()` rende entrambi i modelli
raggiungibili a runtime, così la misura sceglie senza riscrivere il driver. Le primitive del
finder passano da `cmdOffset` e da `csAssert()`/`csRelease()`, che fuori dalla sonda valgono 0 e
CS singolo, quindi il resto del test è invariato. Il BUSY lì non è testimone (è quello del
controller che risponde ai comandi normali): il timeout di `runRefresh` è esito previsto e la sonda
attende a tempo `SLAVE_OPCODE_WAIT_MS`.

**L'hardware di cascade sta nel pin table dell'SSD1677**, non solo in quello dell'SSD1683: `M/S#`
è dato *"reserved pin, should be connected to VDDIO"* (categoria "Reserved for Testing") e `CL` è
dichiarato **I/O**, *"should be left open in application"*, e compare nelle caratteristiche
elettriche sia fra i VIH d'ingresso sia fra i VOH d'uscita — un pin da lasciare aperto non ha
bisogno di poter pilotare. Sono i due pin che l'SSD1683 §6.12 usa per la cascade: presenti nel
nostro silicio, solo non documentati. Nella stessa direzione va un reference SSD1677 di terzi
(<https://github.com/bigbag/papyrix-reader>, `docs/ssd1677-driver.md`), che scrive `0x21` con **due**
byte, `0x40 0x00`, commentando il secondo con **"single chip"**: è il `ckouten` dell'SSD1683.
Non esiste una revisione pubblica dell'SSD1677 più recente: le copie reperibili altrove sono la
stessa Rev 1.0 del 2018 byte per byte, quindi inutile ricercarle. Vedi [[ssd1677_command_set]].

**SSD2677, il fratello a 4 colori da non confondere**: 960 × 680 come l'SSD1677, ma RAM a **2 bit
per pixel**, licenza E Ink Spectra 3100, command set in stile **UC** (`0x00` PSR, `0x06` BTST,
`0x50` CDI, `0x60` TCON, `0x61` TRES, `0xE0` CCSET, `0xE3` PWS), cascade fino a 1920 × 680 con il
bit `CSEIN` di `0xE0`, e preset di risoluzione `960 × 680 / 960 × 672 / 960 × 640 / 880 × 528` —
cioè su misura per le taglie SOLUM. Datasheet archiviati in `docs/` (Rev 1.0 2024-03 e Rev 1.1
2023-08); li nominano i driver GxEPD2 `epd4c/GxEPD2_1160c_GDEY116F51` (960 × 640 a 4 colori) e
`epd/GxEPD2_576_GDEH0576T81` (920 × 680 B/N). **Serve a due cose**: spiega da dove veniva la
"sequenza UC8179" che i README descrivevano (era in stile SSD2677, non UC8179), e chiude un
discriminante che sembrava esserci — "parla SSD16xx quindi è a 3 colori" **non regge**, perchè la
Table 6-4 dell'SSD1677 mostra quattro combinazioni delle due RAM con `LUT3` distinta.

**Clock SPI**: il driver 12.2" ha il default a 20 MHz, il 9.7" gira a 10. Su alcuni pannelli
SSD1677 il controller non tollera clock alti (stesso reference di terzi): se comparissero errori
intermittenti sulla banda che stampa, abbassare il clock è la prima prova, prima del cablaggio.

**Come si legge il codice modello**: `EL <taglia> <generazione> <colore scocca> <colore display>
<tipo tag>`, quindi `EL122H6W4A` = 12.2", gen H6, scocca bianca, campo colore `4`, tag con pulsante.
Il `4` significa "RED, YELLOW (BWRY)" nel §3.6 del datasheet PRO, ma **non è un dato per unità**:
nel modello table ogni taglia della linea PRO è `...W4A`, e le serigrafie sul vetro della stessa
generazione dicono BWRY sulla 9.7" e **BWR sull'11.6" e sulla 12.2"**. Il campo distingue la linea
(PRO nominalmente a 4 colori, Core `R` a 3); il film lo dice il vetro. Per il 12.2" il vetro dice
`Newton PRO 12.2" BWR normal`, quindi lì la questione è chiusa — non lo è sul 9.7"
([[gxepd2_097c_driver]]).

Altri sospetti nuovi, dallo stesso firmware e dal wiki della board:

- **BS (bus select) flottante**: gli SSD16xx scelgono 3 o 4 fili da un pin che il tag di fabbrica
  tiene basso. Su una coda cablata a mano un BS non pilotato può far ignorare tutto. Da verificare
  anche sulla coda che funziona.
- **Switch n.1 della board Waveshare** (A = 3R contro B = 0.47R): è la resistenza di sense del
  booster, non un selettore di pannello — vedi [[waveshare_esp32_driver_board]].
- **VPP**: sul cavo del pannello esiste una linea di lettura; individuarla rende leggibile `0x2E` e
  chiude il part number del controller. Sul pannello c'è anche una **EEPROM** con CS dedicato
  (`HLT`), sede tipica di waveform/LUT.

**Scheda tag Core, foto FCC ricomposte**: `docs/122c/fcc/122_core_tag_board_due_FFC.jpg` e il suo
zoom mostrano che i **due connettori FFC hanno larghezza e passo identici** (~24 contatti ciascuno,
misurati sul profilo di intensità): la scheda di fabbrica tratta le due code come pari.

OEPL **continua a non avere il 12.2"**: `dualssd` si attiva solo per 792×272 e `oepl-definitions.h`
riscaricato è identico alla copia archiviata.


## Come è cablato un pannello commerciale a due code, e cosa dice della cascade

Il 12.48" di Good Display (**GDEY1248Z51**) e il modulo Waveshare 12.48" (B) sono lo stesso
pannello: 1304 × 984, **due FPC da 30 pin**, UC8179. Lo schematico del breakout ufficiale
DESPI-C1248 e quello Waveshare coincidono e mostrano la topologia standard:

- **quattro controller con quattro chip select** — `CSB_M1`, `CSB_M2` sulla coda master, `CSB_S1`,
  `CSB_S2` sulla coda slave (aree 648×492, 656×492, 656×492, 648×492);
- **quattro BUSY**, **due DC**, **due RST**, **due BS** (`BS`, `BS_2`);
- **due sezioni di boost indipendenti**, con punti di misura separati `VGH_P1`/`VGL_P1` e
  `VGH_P2`/`VGL_P2` ("P1 MOS tube gate voltage", "P2 MOS tube gate voltage"), VCOM in comune;
- code **non intercambiabili**, serigrafate `WFT1248BZ23` (master) e `WFT1248BZ24` (slave):
  invertirle e il pannello non si aggiorna.

Il confronto è quello che serviva. L'asimmetria master/slave delle due code **non** è di per sè
un'anomalia, ce l'ha anche un pannello commerciale; ma un pannello a N controller indipendenti la
paga con **N chip select e N sezioni di boost**. Il tag di fabbrica della 12.2" ha **un solo CS**,
**una sola sezione analogica** e il secondo connettore nudo alimentato da un fascio di piste che
arriva dal primo. Il conteggio dei pin esclude la topologia a controller indipendenti e lascia in
piedi la cascade.

**Modello di driver già scritto**: `GxEPD2-master/src/epd3c/GxEPD2_1248c.{h,cpp}` scompone il
pannello in `ScreenPart(w, h, rev, cs, dc)` — dimensioni, **flag di mirror**, CS e DC per pezzo —
e fa broadcast con `_writeCommandMaster()` / `_writeCommandAll()`, che abbassano **più CS insieme**.
È la stessa scomposizione che serve qui (due parti 960 × 384, una specchiata). Corollario sul
`|0x80`: con un secondo CS il broadcast non servirebbe il trucco degli opcode: basterebbero due CS
bassi. Il trucco serve **perchè un secondo CS non c'è**.

Componenti del circuito di boost per un eventuale breakout (VGH +20 V, VGL −20 V, induttore 10 µH
1 A, MOSFET Si1304BDL/Si1308EDL, Schottky MBR0530) e resto del materiale in
[[pannelli_affini_commerciali]].

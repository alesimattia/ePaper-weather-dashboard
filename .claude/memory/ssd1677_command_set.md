---
name: SSD1677 command set e registri (estratto dal datasheet Rev 1.0)
description: Riferimento verbatim dei comandi SSD1677 che servono ai driver SOLUM 9.7" e 12.2" - polarità delle due RAM e la Table 6-4 che le mappa su LUT0..LUT3, struttura della LUT, tabella parametri di 0x22, 0x21, MUX e TB riservato, i pin M/S# e CL della cascade dati come riservati, data entry, finestre in pixel, HV Ready e VCI detection misurabili col BUSY, deep sleep, pattern hardware, registri in lettura, cosa il datasheet NON contiene, più la ricetta per riestrarre il PDF
type: reference
---

**La Rev 1.0 è più vecchia del silicio che abbiamo in mano.** Due indizi: definisce `0x21` con **un
solo parametro** A[7:0], mentre l'init di fabbrica SOLUM della 9.7" (`unissd.cpp` di OEPL) ne
scrive **due** (`0x08 0x00`); e dà il pin **`M/S#`** come *"reserved pin, should be connected to
VDDIO"*, senza nominare la cascade. Nel **SSD1683** (400x300, `docs/SSD1683_Rev1.0_2021-01_Solomon-Systech.pdf`)
quegli stessi elementi sono documentati: `0x21` ha un secondo byte con **B[4] *ckouten* = cascade
selection**, e `M/S#` sceglie master (VDDIO) o slave (VSS), con lo slave che ha oscillatore e
booster **disabilitati** e riceve CL e tutti i rail dal master (§6.12). Quindi per tutto ciò che
riguarda cascade e comandi estesi la fonte è il SSD1683, non questa Rev 1.0 — vedi
[[gxepd2_122c_driver]].

Altro dal SSD1677 che conta al cablaggio: **`BS1` L = 4 fili, H = 3 fili (9 bit)**. Un BS1 non
pilotato su una coda cablata a mano può mettere il controller in 3 fili e fargli ignorare tutto il
traffico a 4 fili.

Estratto da `A:\epd\GxEPD2_SOLUM_ESL\docs\SSD1677_Rev1.0_2018-11_Solomon-Systech.pdf` (47 pagine,
Solomon Systech, Nov 2018). Serve a non rileggere il PDF ogni volta e a non dedurre il comportamento
del controller dal driver custom, che su un punto era sbagliato. Driver e conseguenze:
[[gxepd2_097c_driver]].

## Estrazione del PDF

`pdftotext` **c'è**: `/mingw64/bin/pdftotext`, versione 4.00, arriva con Git for Windows ed è
raggiungibile sia dal tool Bash sia da PowerShell (`C:\Program Files\Git\mingw64\bin\pdftotext.exe`).
`pdftotext -layout file.pdf out.txt` funziona e basta: non serve nessuna decompressione zlib degli
stream nè ricostruzione per-glifo.

Unica cautela, la stessa dei PDF SOLUM: nella command table `-layout` **sfalsa le colonne di una
riga**, quindi la descrizione che appare accanto a un comando può appartenere a quello precedente o
successivo. I nomi dei comandi nella colonna di sinistra sono affidabili; le descrizioni vanno
riallineate guardando il contesto prima di citarle. Le righe escono nella forma
`0 0 24 0 0 1 0 0 1 0 0 Write RAM (Black White)`, cioè R/W# + D/C# + hex + bit + nome: cercare
`"0 0 24"` trova la riga di `0x24`.

Vale invece sempre: **non passare regex o stringhe con backslash attraverso l'heredoc del tool
Bash**, che i backslash li mangia (`(?<!\\)` arriva come `(?<!\)`, e `"\n"` dentro un letterale C
diventa un newline vero che spezza la stringa). Scrivere lo script su file con Write e poi lanciarlo.

## Le RAM immagine: sono DUE

Testo verbatim della command table:

| Cmd | Nome | Semantica |
|---|---|---|
| `0x24` | Write RAM (Black White) | "For White pixel: Content of WriteRAM(BW) = 1. For Black pixel: = 0" |
| `0x25` | Write RAM (Dithering) | "data entries will be written into the dithering engine" — **non è un piano** |
| `0x26` | Write RAM (RED) | "For Red pixel: Content of WriteRAM(RED) = 1. For non-Red pixel [Black or White]: = 0" |
| `0x27` | Read RAM | quale RAM si legge lo seleziona il registro `0x41`; il 1° byte letto è dummy |
| `0x28` | **VCOM Sense** | entra in condizione di misura VCOM e ci resta per la durata di `0x29`, il valore va in un registro. **Richiede CLKEN=1 e ANALOGEN=1** (vedi `0x22`), **alza BUSY**, **non accetta parametri** |

Dopo `0x24` / `0x26` i puntatori di indirizzo avanzano da soli fino al comando successivo.

**Table 6-4 "RAM bit and LUT mapping for 3-color display"** dà la semantica esatta delle quattro
combinazioni, e va citata per nome perchè è ciò che rende interpretabile la sonda:

| bit in RED `0x26` | bit in BW `0x24` | colore | LUT |
|---|---|---|---|
| 0 | 0 | nero | LUT0 |
| 0 | 1 | bianco | LUT1 |
| 1 | 0 | rosso | LUT2 |
| 1 | 1 | rosso | **LUT3 = LUT2** |

C'è anche la Table 6-5 per il B/N puro, dove `LUT2 = LUT0` e `LUT3 = LUT1`. In entrambe **le LUT
esistono tutte e cinque nel silicio** (`LUT0..LUT4`) e le tabelle si limitano ad aliasarne una: §6.7
descrive 112 byte di waveform LUT (byte 0..104 = `VS[nX-LUTm]`, `TP[n#]`, `RP[n]`, frame rate via
`0x32`; 105 gate level via `0x03`; 106..108 source level via `0x04`; 109 VCOM via `0x2C`), §6.6
dieci gruppi da quattro fasi e **quattro livelli di sorgente** (VSS, VSH1, VSH2, VSL) con `VS` a 2
bit. Conseguenza da non dimenticare: **"due RAM da 1 bit" non implica "3 colori"** — quattro stati
per pixel il chip li sa esprimere, e a decidere se `LUT3` guida un quarto colore o replica il rosso
è la waveform in OTP.

**Nel datasheet non compaiono**: la parola "yellow", una modalità a 4 colori, un terzo piano
immagine. Che colore renda ciascuna combinazione sul film montato, e se `0x28` su questo modulo
scriva comunque qualcosa, lo dice solo la misura — la fa
`GxEPD2_SOLUM_ESL\examples\097c\panel_diagnostic\panel_diagnostic.ino`, vedi
[[gxepd2_097c_driver]]. Il firmware scrive il rosso come `(RED=1, BW=1)` = **LUT3**; l'unica LUT mai
esercitata è **LUT2** = `(RED=1, BW=0)`.

## 0x22 Display Update Control 2 — tabella parametri completa

POR = `0xFF`. Ogni valore è una sequenza di stadi che parte con la Master Activation `0x20`:

| Param | Sequenza |
|---|---|
| `0x80` | Enable clock |
| `0x01` | Disable clock |
| `0xC0` | Enable clock + Enable Analog |
| `0x03` | Disable Analog + Disable clock |
| `0x91` | Enable clock + Load LUT with DISPLAY Mode 1 + Disable clock |
| `0x99` | Enable clock + Load LUT with DISPLAY **Mode 2** + Disable clock |
| `0xB1` | Enable clock + Load temperature (I2C single master) + Load LUT Mode 1 + Disable clock |
| `0xB9` | come `0xB1` ma Load LUT **Mode 2** |
| `0xC7` | Enable clock + Enable Analog + Display Mode 1 + Disable Analog + Disable OSC |
| `0xCF` | come `0xC7` ma Display **Mode 2** |
| `0xF7` | Enable clock + Enable Analog + Load temperature + Load LUT + DISPLAY Mode 1 + Disable Analog + Disable OSC |
| `0xFF` | come `0xF7` ma DISPLAY **Mode 2** |

**Decodifica per bit**, ricavata differenziando le righe della tabella (`0x91` vs `0x99` isola il
bit 3, `0xC7` vs `0xCF` lo conferma):

| bit | significato |
|---|---|
| 7 `0x80` | enable clock |
| 6 `0x40` | enable analog |
| 5 `0x20` | load temperature |
| 4 `0x10` | load LUT |
| 3 `0x08` | **seleziona DISPLAY Mode 2** invece di Mode 1 |
| 2 `0x04` | display, cioè la passata sul pannello |
| 1 `0x02` | disable analog |
| 0 `0x01` | disable OSC |

Serve a costruire i valori che la tabella non elenca. I due che contano:

| Param | Sequenza | Chi lo usa |
|---|---|---|
| `0xFC` | come `0xFF` **senza** disable analog e disable OSC: Mode 2 lasciando alimentazione e clock accesi | `_Update_Part()` di `GxEPD2_1330_GDEM133T91`, ed è così che una catena di partial resta veloce |
| `0xF4` | come `0xF7` senza il power down finale: Mode 1 con alimentazione accesa | il controllo che distingue "Mode 2 è veloce" da "non spegnere è ciò che fa risparmiare" |

`0xF7` è quello che usa il driver e anche l'init di fabbrica SOLUM: include power on e power off
impliciti, per questo non serve `_PowerOn()`/`_PowerOff()` attorno al refresh. `0xC0` / `0xC3` sono
i valori che il driver usa per power on / power off espliciti.

**Mode 2 è il banco waveform differenziale**, non un banco di colori: usa la seconda RAM come frame
precedente (`0x37` F[6] abilita il RAM ping-pong, e il datasheet nota "RAM ping-pong function is not
support for Display Mode 1"). Su questo pannello la seconda RAM è l'accent, quindi Mode 2 non è
utilizzabile per il fast partial update e da esso non è atteso un colore in più. La sonda lo esegue
comunque come seconda passata, così l'attesa è confrontata con quello che si vede.

## 0x21 Display Update Control 1 — opzione contenuto RAM

POR `0x00`. A[7:4] = opzione **Red** RAM, A[3:0] = opzione **BW** RAM. Per entrambi:
`0000` Normal, `0100` Bypass RAM content as 0, `1000` Inverse RAM content.

L'init di fabbrica SOLUM manda `0x21 {0x08,0x00}` = Red Normal + **BW Inverse** ("fix reversed image
with stock setup"); per i pannelli a 2 colori OEPL manda `0x48` = Red Bypass-as-0 + BW Inverse. Il
driver custom non scrive mai `0x21`, resta al POR (entrambi Normal) e inverte il piano rosso in
software: convenzione equivalente.

**Il secondo byte esiste anche fuori da SOLUM.** Un reference SSD1677 di terzi
(<https://github.com/bigbag/papyrix-reader>, `docs/ssd1677-driver.md`) scrive `0x21` a due byte,
`0x40 0x00`, e commenta il secondo con **"single chip"**: è `B[4] ckouten` dell'SSD1683, il bit che
mette il chip in cascade e gli fa emettere CL. Terza fonte indipendente dopo l'init di fabbrica e il
`dualssd.cpp` di OEPL.

## Altri comandi usati dal driver

| Cmd | Significato | Note |
|---|---|---|
| `0x01` | Driver Output Control (MUX + direzione scan) | A[9:0] POR `2A7h` = 680 MUX, gate line = A[9:0]+1. `{0x9F,0x02,0x00}` = 671 -> 672 gate line. B[2:0]: B[2] GD (primo gate output, POR 0 = G0), B[1] SM (ordine di scansione: POR 0 = G0,G1,G2... interlacciato sinistra/destra; 1 = pari poi dispari), B[0] **TB = 1 è dichiarato Reserved**: TB=0, scan da G0 a G679, ed è l'unica opzione. **Non esiste una reverse scan hardware sull'asse gate**. Da non confondere con il verso del CONTATORE DI INDIRIZZO, che invece è configurabile: `0x11` A[1:0] = 00 Y e X decrement, 01 Y decrement X increment, 10 Y increment X decrement, 11 entrambi increment (POR). Specchiare una banda **si può fare nei registri**, ed è così che GxEPD2 tratta le due metà del GDEY0579Z93; nel 122c il ribaltamento sta nel data path per scelta di implementazione, non per assenza di alternativa — vedi [[gxepd2_122c_driver]] |
| `0x0C` | Booster soft start | 5 byte; i livelli in OTP hanno E[7:0] = `0x40` (Level 1) / `0x80` (Level 2) |
| `0x10` | **Deep Sleep mode** | A[1:0]: `00` Normal [POR], `11` Enter Deep Sleep. In deep sleep **il BUSY resta alto**, e questo lo rende verificabile a occhio sul pin. `0x11` ha A[1:0]=01, che nella tabella non c'è; `0x03` ha A[1:0]=11 ed è quello che usa OEPL sulla stessa famiglia. Quale dei due il modulo accetti davvero lo misura la sonda |
| `0x11` | Data Entry mode | A[1:0] = ID: `00` Y-- X--, `01` Y-- X++, `10` Y++ X--, `11` Y++ X++ [POR]. A[2] = AM, direzione di avanzamento del contatore dopo ogni byte: `0` in X [POR], `1` in Y. Il driver usa `0x03`, l'init di fabbrica `0x02` (X decrescente) con finestra X 959->0. Attenzione: il decremento cambia **dove atterrano i byte**, non l'ordine dei bit dentro il byte, quindi da solo non specchia un'immagine |
| `0x14` | **HV Ready Detection** | A[6:4] = n, cool down `10ms x (n+1)`; A[2:0] = m, numero di cicli; durata massima `10ms x (n+1) x m`. `A[7:0] = 00h` fa una detection singola. Richiede **CLKEN=1 e ANALOGEN=1** (cioè power on via `0x22`=0xC0 prima). "BUSY pad will output high during detection", e "the detection will be completed when HV is ready": quindi **la durata del BUSY è l'esito**, leggibile anche senza SDO — molto più corta del massimo = alte tensioni salite, uguale al massimo = mai salite. L'esito esplicito sta in `0x2F` bit 5 |
| `0x15` | **VCI Detection** | A[2:0] livello di soglia: `011` 2.2 V, `100` 2.3 V [POR], `101` 2.4 V, `110` 2.5 V, `111` 2.6 V. Richiede CLKEN=1 e ANALOGEN=1. BUSY alto durante la misura, esito in `0x2F` bit 4. Qui il datasheet **non** promette una conclusione anticipata, quindi la durata dice meno che in `0x14`: quello che conta è che il BUSY reagisca, cioè che il blocco analogico sia vivo |
| `0x18` | Temperature Sensor Control | A[7:0] = `0x48` [POR] sensore esterno, `0x80` interno |
| `0x1A` | Write temperature register | 12 bit, POR `7FFh` |
| `0x1B` | **Read temperature register** | frame di lettura: byte0 = A[11:4], byte1 = A[3:0] nei bit alti. `(b0<<8\|b1)>>4` dà A[11:0] |
| `0x1C` | Temperature Sensor Control (I2C write) | |
| `0x20` | Master Activation | esegue la sequenza di `0x22`. Alza BUSY. "User should not interrupt this operation to avoid corruption of panel images" |
| `0x2B` | VCOM glitch control | |
| `0x2E` | **User ID Read** | 10 byte di User ID da OTP (registro `R38`, byte A..J) |
| `0x2F` | **Status Bit Read** | POR `0x01`. A[5] HV Ready flag (0 ready), A[4] VCI detect flag (0 normale), A[2] Busy flag, A[1:0] **Chip ID** [POR=01]. Qui finiscono anche gli esiti di VCI detect e HV Ready detect |
| `0x37` | registro opzioni display, 10 byte A..J | B[7:0]..F[3:0] = bit Display Mode per ogni stadio waveform WS[35:0] (`0`=Mode 1, `1`=Mode 2); F[6] = RAM ping-pong per Mode 2; **G[7:0]~J[7:0] = module ID / waveform version**. Tutto memorizzabile in OTP |
| `0x3C` | Border waveform control | il driver manda `0x01` (LUT1, bianco), OEPL `0x05` sulle altre taglie |
| `0x44` / `0x45` | finestra RAM X / Y | **coordinate in pixel, non in byte**: X ha A[9:0] XSA POR `000h` e B[9:0] XEA POR `3BFh` = **959**, cioè l'ultimo source. Y: A[9:0] YSA POR `000h`, B[9:0] YEA POR `2A7h` = 679 |
| `0x4E` / `0x4F` | cursore RAM X / Y | POR `000h` |
| `0x46` | Auto Write **RED** RAM for Regular Pattern | vedi sotto |
| `0x47` | Auto Write **B/W** RAM for Regular Pattern | vedi sotto |
| `0x7F` | **NOP** | "empty command; it does not have any effect on the display module. However, it can be used to terminate Frame Memory Write or Read Commands" |

### 0x46 / 0x47 Auto Write RAM for Regular Pattern

`A[7:0] = 00h` [POR]. `A[7]` = valore del primo step. `A[6:4]` = **step height** (alterna la RAM in
Y secondo il gate), `A[2:0]` = **step width** (in X secondo il source):

| A[6:4] | height | A[2:0] | width |
|---|---|---|---|
| 000 | 8 | 000 | 8 |
| 001 | 16 | 001 | 16 |
| 010 | 32 | 010 | 32 |
| 011 | 64 | 011 | 64 |
| 100 | 128 | 100 | 128 |
| 101 | 256 | 101 | 256 |
| 110 | 512 | 110 | 512 |
| **111** | **680** | **111** | **960** |

Quindi `0xF7` = primo step a 1, height 680, width 960 = un unico step su tutta la RAM nativa a 1;
`0x77` = lo stesso a 0. "BUSY pad will output high during operation": il pattern si attende sul
BUSY. Il generatore emette **un livello per step**, quindi sa esprimere solo `0x00` e `0xFF`, e solo
sui due piani immagine — qualunque altro valore passa dal bus.

## I pin della cascade ci sono, dichiarati riservati

Dal pin table della Rev 1.0, cioè dal datasheet del **nostro** controller:

- **`M/S#`** — categoria "Reserved for Testing", *"This pin is reserved pin and should be connected
  to VDDIO"*. Nell'SSD1683 lo stesso pin è la selezione master/slave (VDDIO = master, VSS = slave,
  con oscillatore e booster dello slave disabilitati).
- **`CL`** — tipo **I/O**, *"This is the clock signal pin. It should be left open in application"*,
  e nelle caratteristiche elettriche compare sia fra i VIH d'ingresso (`M/S#, EXTVDD, CL`) sia fra i
  VOH d'uscita (`SDA, BUSY, CL`). Un pin da lasciare aperto non ha bisogno di poter pilotare.

Nell'SSD1683 §6.12 sono esattamente i due pin della cascade. Quindi **l'hardware c'è nel silicio**,
la Rev 1.0 non lo documenta. È l'argomento che regge l'ipotesi cascade sul 12.2"
([[gxepd2_122c_driver]]).

**Non esiste una revisione pubblica più recente**: le copie reperibili su cursedhardware e su
e-paper-display.com sono lo stesso file Rev 1.0 Nov 2018, byte per byte. Inutile ricercarle.

## Caratteristiche dichiarate (pagina feature)

- sensore di temperatura interno **-25..50 °C, accuratezza ±2 °C, status a 9 bit**
- interfaccia I2C single master per sensore di temperatura esterno
- MCU interface SPI, **max 20 MHz in scrittura** (il progetto usa 10 MHz sul 9.7", 20 sul 12.2").
  Il massimo è di targa: un reference di terzi riporta pannelli SSD1677 che a 40 MHz funzionano e
  altri dello stesso controller che non tollerano più di 10, quindi il clock è la prima cosa da
  abbassare davanti a errori intermittenti
- auto write RAM per pattern regolare, dithering mono B/N, panel break diagnostic, partial update
- low voltage detect sull'alimentazione, high voltage ready detect sulla tensione di pilotaggio,
  entrambi con esito leggibile da `0x2F`

## Vincolo pratico che annulla tutte le letture

**Ma HV Ready e VCI si misurano comunque, senza SDO**: `0x14` e `0x15` alzano il BUSY per la
durata della detection, e per `0x14` il datasheet dichiara che la detection si conclude quando HV è
pronta. Cronometrare il BUSY dà l'esito senza leggere `0x2F`, ed è così che
`examples/12_2c/dual_panel_finder` stabilisce se una coda del 12.2" ha le alte tensioni: vedi
[[gxepd2_122c_driver]]. Resta vero che tutto il resto delle letture non è disponibile.

`0x1B`, `0x27`, `0x2E`, `0x2F` e il read-back di `0x37` richiedono la linea dati in uscita del
pannello. Sul FPC 24 pin **non c'è SDO** (pin 12 = solo SDI, schematico Waveshare V3 in
`docs/E-Paper_ESP32_Driver_Board_V3.pdf`): su questo hardware tornano `0x00`/`0xFF` e non valgono
niente. Prima di credere a una lettura, verificare `0x2F`: ha POR `0x01` e Chip ID `01`, quindi è
l'unico registro con un valore atteso noto e fa da test di validità del percorso. Vale la pena
ricablare solo per leggere `0x2E` (User ID) e `0x37` G..J (module ID / waveform version), che
identificherebbero il modulo senza ambiguità.


## Conferme da pannelli commerciali sullo stesso controller

Dai demo Arduino di Good Display per **GDEM102Z91** (960 × 640 BWR) e **GDEM133T91** (960 × 680
B/N), entrambi SSD1677 ([[pannelli_affini_commerciali]]):

- **`0x0C` soft start = `AE C7 C3 C0 80`** su tutti e due, ultimo byte compreso. Il `0x40` di
  GxEPD2 `GDEH116T91` è l'eccezione, non la regola: il valore di fabbrica SOLUM è quello comune.
- **`0x3C = 0x01`** è commentato nel sorgente **"LUT1, for white"** — conferma indipendente della
  numerazione della Table 6-4: il border waveform si sceglie nominando la LUT, e LUT1 è il bianco.
- **Il tetto dei gate è reale e viene usato tutto**: GDEM133T91 è 960 × 680 con **un solo**
  controller e programma `0x01` MUX = **679** (`A7 02 00`) e `0x4F` = 679. È il pannello a chip
  singolo più grande esistente su questo silicio, e cade esattamente sul limite dichiarato.
- `0x11` entry mode vale `0x01` nei demo Good Display contro `0x02` nell'init SOLUM: cambia solo il
  verso di scan, e con esso il senso delle finestre `0x45`/`0x4F`.

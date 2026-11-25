---
name: Waveshare E-Paper ESP32 Driver Board V3 (hardware di bring-up)
description: Caratteristiche verificate sullo schematico della board usata per pilotare i pannelli SOLUM - pinout dell'interfaccia e-paper e del connettore J1, EPD_3.3V distinto dal 3V3 dell'header, BS1 a massa via R11, HSPI con SCK e MOSI scambiati, MISO dummy, IO12 non collegato al pannello, header J3/J4 con GPIO33 su J3-8 (da confermare), GPIO per il secondo controller (CS 32, BUSY 22), switch n.1 = resistenza di sense con schematico e wiki in contrasto, caveat di estrazione del PDF
type: reference
---

Board su cui girano il bring-up e la diagnostica di tutti i pannelli SOLUM del progetto.
Documenti in `A:\epd\GxEPD2_SOLUM_ESL\docs\`: `E-Paper_ESP32_Driver_Board_V3.pdf` (schematico, la
fonte) e `waveshare-E-Paper_ESP32_Driver_Board_manual_en.pdf` (manuale utente, **inutile per i
pin**: parla dell'app e del caricamento immagini, nessuna tabella di pinout).

Modulo: **ESP32-WROOM-32E**, quindi **senza PSRAM** → GPIO16 e GPIO17 sono liberi, non impegnati
dalla RAM esterna come su un WROVER.

## Interfaccia e-paper (verificata)

| Segnale | GPIO |
|---|---|
| BUSY | 25 |
| RST | 26 |
| D/C | 27 |
| CS | 15 |
| CLK (SCK) | 13 |
| DIN (MOSI) | 14 |

La board usa i pin HSPI **con SCK e MOSI scambiati** rispetto al default del bus, quindi il remap
non è una scelta ma un obbligo:

```cpp
SPIClass hspi(HSPI);
hspi.begin(13, 12, 14, 15);   // sck, miso, mosi, ss
display.epd2.selectSPI(hspi, SPISettings(4000000, MSBFIRST, SPI_MODE0));
```

È esattamente ciò che fa l'example board-specific upstream
`A:\tmp\GxEPD2-master\examples\GxEPD2_WS_ESP32_Driver\GxEPD2_WS_ESP32_Driver.ino` (righe 22, 178-190),
che è la fonte autorevole per questa board e usa anche lui 4 MHz. Il firmware del progetto sta a
10 MHz, il driver 12.2" ha 20 MHz come default in `selectSPI()`.

## Alimentazione e strap del connettore J1 (netlist)

- J1-15/16 (VDDIO, VCI) = **EPD_3.3V**, uscita di U3 RT9193, sempre acceso (R35 verso IO4 non montata, quindi niente circuito di reset "clever"). Il 3V3 di J3-1 è un'altra net: U2 ME6217, che alimenta l'ESP32. Una seconda coda va alimentata da EPD_3.3V (piazzole di C5/C6/C20).
- J1-8 **BS1 a GND via R11 0R** (R10 verso EPD_3.3V non montata): bus a 4 fili fisso, non flottante.
- J1 pin 1, 4, 19 senza net; 6/7 solo su R5/R6 (10K/NC); 21 PREVGH e 23 PREVGL con D4/D2 dal booster (Q1 BSS138, L1 10 µH); condensatori 1 µF/50 V su 5, 18, 20, 21, 22, 23, 24.
- IO12 arriva solo a J3-13, non a J1: il MISO dell'HSPI non è collegato al pannello; resta pin di strapping.
- Switch n.1: lo schematico dice ON:A = 0,47 Ω e OFF:B = 3 Ω (R7 0,47 sempre in serie, R8 2,2 cortocircuitata dallo switch), il wiki il contrario: decide la misura della resistenza di sense.

## MISO = GPIO12 è un dummy, e il perchè conta

Sul FPC 24 pin del connettore interno **non esiste SDO**: il pin dati è solo SDI. Conseguenza
diretta: su quel connettore tutti i comandi di lettura del controller (`0x1B` temperatura, `0x27`
read RAM, `0x2E` User ID, `0x2F` status) non tornano niente, e un driver o una sonda che ci conti
sopra sta misurando rumore. Vedi [[ssd1677_command_set]] e [[gxepd2_097c_driver]].

Sulle code cablate a mano su breakout la storia cambia: se la coda del pannello porta fuori un SDO,
collegarlo a GPIO12 rende possibile la lettura, e `0x2E` (User ID da OTP) identificherebbe il
modulo senza ambiguità — l'unico modo pulito di chiudere la questione del part number del
controller del 12.2" ([[gxepd2_122c_driver]]).

**GPIO12 è un pin di strapping** (MTDI: al reset seleziona la tensione della flash). Flottante o
basso al boot non fa danni, ed è la condizione in cui lo lascia l'uso come dummy. Se invece ci si
collega un SDO vero, verificare che al reset non sia forzato alto: in quel caso l'ESP32 non parte.

## Header di espansione: GPIO33 c'è, su J3 pin 8 (da confermare sulla serigrafia)

Netlist con de-raddoppio **per token** (`pdftotext -raw`, un token è raddoppiato se ogni carattere
pari è uguale al successivo) e coppie pin header / pin modulo: è la piedinatura delle DevKitC.

```
J3: 1 3V3, 2 EN, 3 VP(36), 4 VN(39), 5 IO34, 6 IO35, 7 IO32, 8 IO33, 9 IO25, 10 IO26,
    11 IO27, 12 IO14, 13 IO12, 14 GND, 15 IO13, 16 SD2, 17 SD3, 18 CMD, 19 5V
J4: 1 GND, 2 IO23, 3 IO22, 4 U0TX, 5 U0RX, 6 IO21, 7 GND, 8 IO19, 9 IO18, 10 IO5,
    11 IO17, 12 IO16, 13 IO4, 14 IO0, 15 IO2, 16 IO15, 17 SD1, 18 SD0, 19 CLK
```

IO33 = J3 pin 8 = pin 9 del modulo WROOM-32E, che è proprio IO33. La conclusione "33 non portato
fuori" veniva dal de-raddoppio **per carattere**, che trasforma `IIOO3333` in `IO3` (la lista di
allora conteneva un "3" e nessun 33). Conferma fisica: serigrafia dell'header o continuità fra J3
pin 8 e il pin 9 del modulo. `PIN_CS_S` resta su **GPIO32** in `Layout_122c.h` e nei documenti del
submodule; funziona comunque, il 33 è un'alternativa. Il `CLAUDE.md` del progetto dice ancora che il
33 non è portato fuori.

## GPIO liberi per un secondo controller

| Uso | Pin | Perchè |
|---|---|---|
| **CS_S (in uso)** | **32** | output-capable, sull'header, **una sola occorrenza nello schematico** cioè non usato da nient'altro, e adiacente al 35 |
| CS_S alternative | 4 | libero e output-capable (16/17 pure, essendo WROOM senza PSRAM, ma tenerli di riserva) |
| cablaggio consigliato del sensore Indoor (BME680 o BME280) | 21, 16 | liberi sulla board; il firmware ci mette SDA e SCL del sensore (`INDOOR_SDA_PIN` 21 / `INDOOR_SCL_PIN` 16 in `Indoor.h`; il 16 è utilizzabile perchè il modulo non ha PSRAM), ma è solo il cablaggio consigliato: l'I2C dell'ESP32 si instrada su qualunque pin I/O, quindi il sensore si sposta se servono questi pin |
| **BUSY_S (in uso)** | **22** | ha il pull-down interno, che il driver attiva: un BUSY scollegato o lasciato aperto (cascade) si legge libero; 34..39 sono input-only senza pull e lo leggerebbero a caso |
| da evitare | 0, 2, 12 | pin di strapping (boot mode, flash voltage) |
| liberi ma da tenere di riserva | 5, 18, 19, 23 | sono i pin VSPI: liberi su questa board, ma è il bus che si usa per aggiungere periferiche SPI (una SD, per esempio), quindi conviene non occuparli |

## Caveat di estrazione dello schematico

`pdftotext -layout E-Paper_ESP32_Driver_Board_V3.pdf` produce testo con **ogni carattere
raddoppiato** (`IO2255` = IO25) e con le colonne delle etichette **sfalsate di una riga**, quindi
l'accoppiamento fra una net e il nome del pin che le sta accanto non è affidabile. Ricetta: prima
si de-raddoppia il testo, poi si estraggono le net con una regex `IO\d{1,2}`, e per il pinout
dell'interfaccia e-paper si incrocia con l'example upstream invece di fidarsi dell'accoppiamento
visivo. È lo stesso genere di trappola dei PDF SOLUM descritta in [[ssd1677_command_set]].

## Altro sulla board

- **Nessun socket microSD**: verificato sullo schematico, zero occorrenze di MicroSD / TF / CARD /
  SD_CMD / SDIO. Le net che contengono "SD" sono altro: `TSDA`/`TSCL` sono l'I2C del sensore di
  temperatura sul FPC, `SD0..SD3` + `CMD` + `CLK` sono i pin della flash interna del modulo
  (GPIO6-11), `SDI` è il dato del FPC. Il commento dell'example upstream "use HSPI for EPD (and
  VSPI for SD)" è un pattern per quando una SD la si aggiunge, non una descrizione della board:
  è la trappola in cui si cade leggendolo di fretta.
- Presenti invece: pulsanti KEY_RST / KEY_FLASH, USB-seriale CH343P e un doppio switch. Il n.1 il
  manuale lo descrive come "set types switch according to the e-paper you use", ma il wiki dice cosa
  commuta davvero: la **resistenza di sense del booster**, A = 3R e B = 0.47R — non è un selettore
  di modello (vedi la sezione sul wiki, più sotto). Il n.2 alimenta il modulo USB-UART.
- `hspi.begin()` e `selectSPI()` sono roba board-level: nel firmware stanno nel `.ino` e non nei
  `Layout_*.h`, con l'eccezione documentata in [[layout_separation]] (il driver 12.2" apre il bus
  da sè in `init()`, quindi `Layout_122c.h` gli passa anche sck/miso/mosi).

## Wiki ufficiale: cosa aggiunge allo schematico

<https://www.waveshare.com/wiki/E-Paper_ESP32_Driver_Board>, testo estratto in
`A:\epd\GxEPD2_SOLUM_ESL\docs\waveshare-E-Paper_ESP32_Driver_Board_wiki.txt`; analisi in
`docs\fonti_esterne.md`. Il wiki conferma il pinout dell'interfaccia e-paper (DIN P14, SCLK P13,
CS P15, DC P27, RST P26, BUSY P25) e **non elenca alcun MISO**: riscontro indipendente che il
connettore non porta una linea di lettura. Non pubblica invece il pinout dell'header, quindi la
questione GPIO33 resta decisa dal solo schematico.

**Lo switch n.1 non è un selettore di "tipo pannello": sceglie la resistenza di sense del
booster.** Posizione **A = 3R**, posizione **B = 0.47R**, con tabella per pannello:

- **A (3R)**: 1.54", 1.54"(B), 2.13", 2.13"(B), 2.66", 2.66"(B), 2.9", 2.9"(B), 3.7", 4.2",
  4.2"(B), **13.3"**, **13.3"(B)**
- **B (0.47R)**: 2.13"(D), 2.7", 2.9"(D), 4.01"(F), 4.2"(C), 5.65"(F), 5.83", 5.83"(B), 7.5",
  7.5"(B)

I SOLUM non sono in tabella; il riferimento più vicino per silicio e geometria sono i 13.3"
(SSD1677, 960 source), che stanno in **A**. Il wiki dice di provare l'altra posizione se il display
è anomalo o non si pilota. Agisce sul circuito di boost, quindi va escluso prima di attribuire al
driver un refresh debole, un ghosting o una coda che non risponde ([[gxepd2_122c_driver]]).

Altro dal wiki: switch n.2 = alimentazione del modulo USB-UART, con OFF **non si programma**;
revisioni **2022-07-28** CP2102 → **CH343** e **2024-12-30** micro-USB → **USB-C** (hardware per il
resto compatibile), quindi la board del progetto sta fra le due; 4 MB flash, 520 KB SRAM,
29.46 × 48.25 mm, 5 V con range 3.6–5.5 V; in confezione anche **adapter board e prolunga FFC**.

Sul cavo del pannello, il firmware di fabbrica dei tag usa segnali che questa board non porta —
**BS** (bus select 3/4 fili), **VPP** (linea di lettura), **HLT** (CS di una EEPROM sul pannello),
**POWER** (alimentazione commutata): vedi [[oepl_nrf52811_tag_fw]].

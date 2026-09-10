---
name: Pannelli commerciali affini ai SOLUM (Good Display / Waveshare)
description: Cosa esiste in commercio vicino ai due pannelli SOLUM - il reverse engineering di terzi su una ESL 10.2" BWR con SSD1677 che scrive le tensioni in init e conferma la LUT (precedente, nuovo), GDEM102Z91 gemello funzionale della 9.7" (SSD1677 BWR, init identico byte per byte), la regola SSD1677 = 3 colori / SSD2677 = 4 colori su tutta la linea grande Good Display, il formato a 2 bit su stream 0x10 dei BWRY, il tetto dei 680 gate confermato da GDEM133T91, la topologia reale di un pannello a due code (12.48": 4 CS, 4 BUSY, 2 boost) e cosa dice della cascade, componenti del circuito di boost, GxEPD2_1248c come modello di driver multi-controller, il filone Xteink X4 / GDEQ0426T82 su SSD1677 con LUT custom completa di VCOM e grayscale a due piani (e perche' la loro LUT grigia e' pericolosa su un film BWR), dove sta il materiale scaricato
metadata:
  type: reference
---

Ricognizione su good-display.com e waveshare.com/epaper. I SOLUM non stanno su quei siti: quello
che si cerca sono pannelli **affini** — stessa taglia, stesso controller, stessa architettura a due
code — da cui trarre conferme e spunti.

## Dove sta il materiale

`A:\tmp\panelli_affini\` (21 MB, fuori dal repo): `datasheet/` GDEM102Z91, GDEM102F91, GDEM133T91;
`sample_code/` i .rar Arduino più gli estratti; `breakout/` spec e schematico DESPI-C1248,
schematico e datasheet Waveshare 12.48"; `wiki/` le due pagine wiki salvate.

In `docs/` del submodule è entrato **solo** il demo Arduino GDEM102Z91
(`docs/097c/gooddisplay_GDEM102Z91_arduino/`): è l'unico artefatto direttamente riapplicabile —
driver SSD1677 BWR a due piani, gira sulla 9.7" cambiando MUX e finestra. Tutto il resto documenta
pannelli che non sono i nostri e resta fuori dal repo.

Endpoint di download Good Display (i pulsanti "Download" sono JS): la pagina
`/companyfile/<id>.html` porta `data-url="/comp/xcompanyFile/downloadNew.do?appId=24&fid=<fid>&id=<id>"`,
che risponde con uno `<script>window.location='https://v4.cecdn.yun300.cn/100001_1909185148/<file>'</script>`.
Serve `-e https://www.good-display.com/` come referer.

## Reverse engineering di terzi su una ESL 10.2" BWR con SSD1677

<https://andelf.github.io/blog/2023/12/17/10-2-inch-3-color-esl/> — vetro `HINK-E102A01-A1`,
controller SSD1677, full refresh BWR ~20 s contro i nostri 24. È la fonte esterna più vicina a
questo pannello, e conferma il modello del partial da un'implementazione indipendente:

- la sua LUT per il fast refresh in bianco e nero usa `LUT0 = B->B`, `LUT1 = B->W` a **VSL**,
  `LUT2 = W->B` a **VSH1**, `LUT3 = W->W`, cioè l'indice `(precedente, nuovo)` con il precedente
  nella RED RAM e le stesse polarità del nostro driver;
- **scrive le tensioni esplicitamente in init**: `0x03 = 0x00` (VGH 20 V), `0x04 = 0x41, 0xA8, 0x32`
  (i POR: VSH1 15 V, VSH2 5 V, VSL −15 V), `0x2C = 0x44` (VCOM −1,7 V). È la pratica che mancava al
  nostro driver e la causa del nero pallido, vedi [[gxepd2_097c_driver]];
- ottiene un fast BW usabile con **15 frame in una sola fase** (`TP[0A] = 0x0F`, frame rate `0x22`),
  cioè meno dei nostri 18: il numero di frame non era il problema;
- scrive anche `0x37 = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x4F, ...}`, cioè tutti i waveform set
  dichiarati Mode 2 e `F[6] = 1` = **RAM ping-pong abilitato**, perchè lei carica dall'OTP. Sul
  nostro non serve: con una LUT custom il bit 4 di `0x22` è spento e nessun set viene caricato.

Nota di metodo su `0x37`: dichiarare tutti i set Mode 2 sarebbe rischioso qui, perchè `_Update_Full`
usa `0xF7` che è Mode 1 e potrebbe restare senza set corrispondente.

Seconda fonte, meno ricca ma utile perchè ricostruisce la stessa sequenza canonica
(`0x32`, poi `0x03`/`0x04`/`0x2C`, poi `0x22` + `0x20`):
<https://github.com/bigbag/papyrix-reader/blob/main/docs/ssd1677-driver.md>. E per il principio
generale del fast BW su film a tre pigmenti, cioè togliere le fasi del rosso e tenere quelle del
nero: <https://github.com/olikraus/u8g2/issues/1393> (UC8151D, LUT diverse, idea identica).

## Gemello funzionale della 9.7": GDEM102Z91

10.2", **960 × 640**, **BWR**, **SSD1677**, FPC **24 pin 0,5 mm**, un solo COF, pitch 0,2245 mm,
full refresh 20 s **a 25 °C**, 0~40 °C. Non è il nostro pannello (noi 960 × 672, pitch 0,210) e
**non è un rimarchio**: è l'analogo commerciale più vicino che esista. Il datasheet scrive
`DRIVER IC: SSD1677`, `RESOLUTION 640gate X 960source` — stessa notazione gate/source del conto sui
gate.

Il suo demo Arduino conferma l'init di fabbrica SOLUM ([[gxepd2_097c_driver]]):

| Comando | GDEM102Z91 | SOLUM 9.7" | |
|---|---|---|---|
| `0x0C` soft start | `AE C7 C3 C0 80` | `AE C7 C3 C0 80` | **identico**, ultimo byte `0x80` |
| `0x01` MUX | `7F 02 00` (639) | `9F 02 00` (671) | stessa codifica |
| `0x11` entry mode | `01` | `02` | scan opposto |
| `0x3C` border | `01` | `01` | commento sorgente: **"LUT1, for white"** |
| `0x18` | `80` | `80` | sensore interno |
| `0x22` + `0x20` | `F7` | `F7` | identico |
| piani | `0x24` diretto, `0x26` con `~data` | uguale | accent invertito |

Due ricadute. Il byte `0x80` del soft start è quello **giusto** — GxEPD2 `GDEH116T91` mette `0x40`,
ma un pannello BWR di Good Display usa `0x80` come il tag SOLUM. E il commento `0x3C = 0x01 //
LUT1, for white` è **conferma indipendente della numerazione della Table 6-4**
([[ssd1677_command_set]]): la mappa LUT0..LUT3 su cui è costruita la sonda `panel_diagnostic` è
quella vera, non una ricostruzione.

## Secondo filone di terzi: gli e-reader Xteink su SSD1677

Il **GDEQ0426T82** (4.26", 800 × 480, B/N, SSD1677) monta gli e-reader **Xteink X4**, e ha attorno
due basi di codice indipendenti da GxEPD2 che quel controller lo pilotano per intero:
`open-x4-epaper/community-sdk` (`libs/display/EInkDisplay/`, più `doc/SSD1677_GUIDE.md`) e
`bigbag/papyrix-reader` (`docs/ssd1677-driver.md`). GxEPD2 lo supporta a sua volta come
`gdeq/GxEPD2_426_GDEQ0426T82`, quindi lo stesso pannello si può confrontare in tre implementazioni.

Cosa fanno che GxEPD2 non fa, e che vale come precedente per il driver SOLUM:

- **la LUT custom la mandano completa**: `0x32` per i 105 byte, poi `0x03` VGH, `0x04` VSH1/VSH2/VSL
  e **anche `0x2C` VCOM**. È una conferma indipendente del taglio 110/105 del waveform setting, e
  l'unico esempio in circolazione che scriva pure il VCOM insieme alla waveform;
- **`0x21` prima di ogni refresh**: `0x00` normale per il fast, `0x40` bypass della RED RAM per il
  pieno. Stesso idioma dei due driver GxEPD2 recenti;
- **power off con `0x22 = 0x03`** invece di `0xC3`, e **deep sleep `0x10 = 0x01`**, cioè il modo che
  sul SSD1683 ritiene la RAM;
- **grayscale a quattro livelli** su un pannello a due piani: LSB in `0x24`, MSB in `0x26`, una LUT
  custom e un refresh fast, ~500 ms. Tensioni della loro LUT grigia: VGH `0x17`, VSH1 `0x41`,
  VSH2 `0xA8`, VSL `0x32`, VCOM `0x30`;
- timeout del BUSY a 30 s, e il clock SPI spinto oltre i 20 MHz di targa su alcune unità e non su
  altre — la stessa avvertenza di papyrix-reader.

**Attenzione a riusare le loro LUT grigie su questo film.** Sono scritte per un pannello
monocromatico, dove LUT2 e LUT3 sono due transizioni come le altre; qui LUT3 è l'accent. La seconda
delle due, `lut_grayscale_revert`, ha LUT3 = `FC FC FC FC`, e `0xFC` decodificato a due bit per fase
è `11 11 11 00`, cioè **tre fasi a VSH2**: su questo vetro VSH2 è il pigmento ROSSO. È il caso
concreto per cui la sonda rifiuta ogni waveform che contenga quel code point fuori dal probe dei
livelli di sorgente.

## La regola colori/controller è sistematica

Linea grande Good Display, stessa risoluzione e stesso connettore:

| Modello | Risoluzione | Colori | Driver |
|---|---|---|---|
| GDEM102T91 | 960 × 640 | B/N | SSD1677 |
| GDEM102Z91 | 960 × 640 | **BWR** | **SSD1677** |
| GDEM102F91 | 960 × 640 | **BWRY** | **SSD2677** |
| GDEH116T91 | 960 × 640 | B/N | SSD1677 |
| GDEY116F91 | 960 × 640 | **BWRY** | **SSD2677** |
| GDEM133T91 | 960 × 680 | B/N | SSD1677 |

**Nessun pannello a 4 colori su SSD1677, in nessuna taglia.** Il demo del 4 colori (`GDEM0102F51`)
usa un command set del tutto diverso — `0x00` PSR, `0x03`, `0x06` BTST, `0x30` PLL, `0x41` TSE,
`0x50` CDI, `0x61` TRES, `0x62`, `0x04` power on, `0x02` power off, `0x07` deep sleep `0xA5`,
`0x12` refresh, BUSY attivo basso — e **un solo stream `0x10` a 2 bit per pixel**:

```
00 = bianco (schermo pieno 0x55)     10 = rosso (0xFF)
01 = giallo (0xAA)                   11 = nero  (0x00)
```

È **lo stesso formato** che OEPL usa nel path `epdvarbwry` per le SOLUM BWRY vere (1.6"/2.4"/3.0",
controller 0x17) — [[oepl_nrf52811_tag_fw]]. Due fornitori indipendenti, stessa architettura per i
quattro colori.

Effetto sulla questione aperta del 9.7": **non la chiude in logica** (LUT3 esiste nel silicio e i
livelli di sorgente sono quattro, decide l'OTP), ma sposta molto la prior. Le due RAM `0x24`/`0x26`
sono il formato dei 3 colori presso tutti, il quarto colore vive sempre altrove, e la 9.7" parla il
primo. La wiki Waveshare lo dice esplicito: la waveform è un file `.wbf` fornito dal produttore del
film e **bruciato in OTP nel driver IC in fabbrica**. Resta la banda 4 di `panel_diagnostic` come
misura, ma un esito positivo sarebbe una sorpresa vera.

## Il tetto dei 680 gate, confermato in commercio

**GDEM133T91**: 13.3", **960 × 680**, **un solo SSD1677, una sola coda**. Il suo init programma
`0x01` MUX = **679** (`A7 02 00`) e cursore `0x4F` = 679; il commento nel sorgente ("Set MUX as
527") è copia-incolla sbagliato, contano i byte. È il pannello a chip singolo più grande che esista
su questo controller e cade esattamente sul limite derivato dal datasheet.

Quindi il conto sul 12.2" (768 gate > 680 → due controller obbligati, split sull'asse gate) non è
più solo aritmetica sul datasheet: nessuno spinge un SSD1677 oltre 680 righe
([[gxepd2_122c_driver]]).

## Come è fatto DAVVERO un pannello a due code: il 12.48"

Good Display **GDEY1248Z51** e Waveshare **12.48" Module (B)** sono lo stesso pannello: 1304 × 984,
**due FPC da 30 pin**, UC8179. Dallo schematico del breakout ufficiale DESPI-C1248 e da quello
Waveshare, che coincidono:

- **quattro controller, quattro chip select**: `CSB_M1`, `CSB_M2` sulla coda master, `CSB_S1`,
  `CSB_S2` sulla coda slave (aree 648×492, 656×492, 656×492, 648×492);
- **quattro BUSY** (`BUSY_M1/M2/S1/S2`), attivi bassi;
- **due DC** e **due RST**, uno per coda; **due BS** (`BS`, `BS_2`);
- **due sezioni di boost indipendenti**: punti di misura `VGH_P1`/`VGL_P1` e `VGH_P2`/`VGL_P2`,
  descritti come "P1 MOS tube gate voltage" e "P2 MOS tube gate voltage"; VCOM in comune;
- le code **non sono intercambiabili**: serigrafate `WFT1248BZ23` (master) e `WFT1248BZ24` (slave),
  *"The master FPC and the slave FPC should not be reversed, otherwise the e-paper will not be
  refreshed."*

**Cosa ne segue per il 12.2".** Un pannello convenzionale a due code porta **un CS e un boost per
ogni controller**. Il tag di fabbrica della 12.2" ha **un solo CS** (`HAL_Newton_M3.h`), **una sola
sezione analogica** su un connettore e l'altro connettore nudo, alimentato da un fascio di piste che
arriva dal primo. L'asimmetria master/slave delle code non è di per sè un'anomalia — ce l'ha anche
il 12.48" — ma il 12.48" la paga con **quattro** CS, noi con **uno**. La topologia "N controller
indipendenti" è esclusa dal conteggio dei pin; resta la cascade.

## Circuito di boost, se serve un breakout per i SOLUM

Dalla specifica DESPI-C1248 §4 ("Problems of designing drive circuit"), valida per qualunque
e-paper SPI:

- VGH tipico **+20 V**, VGL tipico **−20 V**;
- induttore **10 µH 1 A avvolto**;
- MOSFET **Si1304BDL** o **Si1308EDL**, in alternativa AO3400;
- diodo Schottky equivalente a **MBR0530**, frequenza di commutazione adeguata;
- socket FPC 0,5 mm con contatto sopra o su entrambi i lati;
- corrente alta in deep sleep = capacità eccessiva nella sezione di boost;
- se non si accende: misurare VGH/VGL **separatamente per ogni coda** prima di sospettare l'SPI.

## Modello upstream di driver multi-controller

`GxEPD2-master/src/epd3c/GxEPD2_1248c.{h,cpp}` (GDEY1248Z51): classe interna
**`ScreenPart(w, h, rev, cs, dc)`** — larghezza, altezza, **flag di mirror**, CS e DC per ogni
pezzo — istanziata quattro volte, più `_writeCommandMaster()` / `_writeCommandAll()` che scrivono lo
stesso comando a più controller **abbassando più CS insieme**. È la scomposizione che serve al 12.2"
(due parti 960 × 384, una specchiata), già scritta e collaudata. Corollario: se un secondo CS
esistesse, il broadcast non richiederebbe `opcode|0x80` — basterebbero due CS bassi. Il trucco
`|0x80` serve **perchè un secondo CS non c'è**.

## Cosa NON è affine

La 9.7" di Waveshare è **IT8951, 1200 × 825, scala di grigi, interfaccia parallela/I80**: nessuna
parentela. Su Waveshare non esiste nulla di SPI in quella taglia — per la 9.7" l'unico riferimento
utile è Good Display 10.2".

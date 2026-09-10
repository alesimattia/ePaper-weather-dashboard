---
name: Driver custom GxEPD2_SOLUM_097c_960x672
description: Driver SOLUM 9.7" SSD1677 (submodule GxEPD2_SOLUM_ESL): tre colori chiusi dalla misura con il probe dei livelli, il PARTIAL da 639 ms e il fatto che un waveform setting è di 110 byte mentre 0x32 ne scrive 105 (le tensioni hanno comandi propri 0x03/0x04/0x2C e l'OTP le sovrascrive, da cui il nero pallido), modello durata = 20 ms x frame + 83 ms, semantica (0x26, 0x24) = (precedente, nuovo) con TP/RP condivisi fra le LUT, le due trappole di 0x26 chiuse dal flag, identificazione del pannello, init di fabbrica, vincoli e costi noti
type: reference
---

Il driver del pannello SOLUM 9.7" (672w × 960h native portrait, controller SSD1677; bianco, nero e
rosso verificati sul pannello) sta in una **libreria Arduino a sè stante**, non in `A:\epd`. La
libreria ospita due driver: struttura, ombrello di selezione, pinout uniforme e contratto in
[[gxepd2_solum_esl_library]]; il fratello 12.2" in [[gxepd2_122c_driver]].

- submodule al path `A:\epd\GxEPD2_SOLUM_ESL`, branch `main`
- repo <https://github.com/alesimattia/GxEPD2_SOLUM_ESL>, fork di ZinggJM/GxEPD2 in cui l'albero
  upstream non è duplicato: l'unico branch remoto è `main`
- header in `src/GxEPD2_SOLUM_097c_960x672.h`; `library.properties` dichiara
  `depends=GxEPD2 (>=1.6.9),Adafruit GFX Library`, `architectures=esp32` e
  `includes=GxEPD2_SOLUM.h` (l'ombrello, non i driver)
- il namespace `GxEPDImage` **non** sta in questo header: è in `src/GxEPDImage.h`, condiviso da
  tutti i driver della libreria
- due costruttori: `(cs, dc, rst, busy)` nativo e `explicit (const GxEPD2_SOLUM_Pins&)`, che è la
  firma uniforme con cui gli sketch costruiscono qualunque driver della libreria
- licenza **GPL-3.0**, obbligata: il driver è copia modificata di `GxEPD2_1330c_GDEM133Z91`
- lo sketch non include questo header direttamente: `Layout_097c.h` definisce `SOLUM_PANEL_097C` e
  include `GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM.h`
- le modifiche al driver si committano e pushano nel suo repo; nel padre si aggiorna il puntatore
  del submodule. `A:\epd` va clonato con `--recursive` (submodule anche `webapp`)
- upstream GxEPD2 sta in `A:\tmp\GxEPD2-master`, fuori dal progetto perchè arduino-cli copia
  l'albero dello sketch a ogni build: clone gitignorato al tag 1.6.9 (`de82887`), copia di sola
  lettura per consultare i sorgenti. Si sfoglia da `epd.code-workspace`, che lo monta come seconda
  radice

Doc dedicata: [A:\epd\GxEPD2_SOLUM_ESL\README.md](../../GxEPD2_SOLUM_ESL/README.md) — motivazione,
API (`showImage`, `writeImageBlack/Red`, le API del partial), sistema di descrittori, tabella di
confronto con la base GDEM133Z91, dettaglio delle ottimizzazioni.

**QUESTIONE 4° COLORE: CHIUSA, tre colori.** Il driver pilota due piani, `0x24` (BW) e `0x26`
(accent), e un terzo non esiste: `panel_diagnostic` ha esercitato tutte e quattro le combinazioni
dei due piani sotto la waveform di produzione e `(1,0)` e `(1,1)` escono **entrambe rosse**, cioè
`LUT3` è aliasata su `LUT2` come la Table 6-4 dichiara. Con due bit per pixel le combinazioni sono
esaurite. Le prove dirette che chiudono la questione:

- **probe dei livelli di sorgente**, l'evidenza diretta: una waveform custom via `0x32` porta `LUT2`
  a VSH1 e `LUT3` a VSH2, tempi identici, e sul vetro le due bande escono di colore **diverso**,
  VSH1 nero e VSH2 rosso. Il film separa i due pigmenti per soglia di tensione, che è come lavora un
  BWR, e nessuna delle due tensioni tira fuori un giallo;
- **`0x28` è VCOM Sense e non un piano**: alla scrittura alza il BUSY per 9953-9968 ms misurati e non
  dipinge niente;
- il codice modello dell'unità letto sul case è `EL097R2CRN`, campo colore `R` = BWR (pratica FCC
  `2AFWN-EL097R2CRN`, KC `R-R-SLU-EL097R2CRN`): non è il donor `EL097F5C4C` della linea PRO, che
  porta la cifra `4`. Il datasheet SOLUM che dà `PIXEL COLORS = BWRY` per la taglia 9.7" riguarda
  quella linea, non questa unità.

Conseguenza pratica per chi compone immagini: **sotto un pixel di accent il valore del piano BW è
indifferente**, perchè LUT2 = LUT3, quindi scrivere l'accent non richiede di mascherare `0x24` e
`writeImageRed()` non lo fa. Il driver 9.7" **non ha** le API del terzo piano (`writeImageYellow`,
`preserveYellow`, `isYellowPreserved`): esistono solo nel 12.2", dove la domanda è ancora aperta.
`GxEPD_YELLOW` finisce sul rosso, che è l'unico accent che il film ha, e il template upstream
`GxEPD2_3C` lo tratta già così (`GxEPD2_3C.h:196`).

Riscontri documentali concordi, tutti archiviati in `GxEPD2_SOLUM_ESL/docs/`:

- **Datasheet SSD1677 Rev 1.0** (`docs/SSD1677_Rev1.0_2018-11_Solomon-Systech.pdf`): le RAM
  immagine sono soltanto `0x24` (BW, 1 = white) e `0x26` (RED, 1 = red). `0x25` è Write RAM
  (Dithering) e entra nel dithering engine, non è un piano; `0x27` è Read RAM; **`0x28` è VCOM
  Sense**, comando analogico che richiede `CLKEN=1` e `ANALOGEN=1` e **alza BUSY** per la durata
  impostata da `0x29`. I byte mandati dopo `0x28` non sono solo scartati: viaggiano mentre il chip
  può essere in sensing. Command table completa, tabelle parametri e ricetta di estrazione del PDF
  in [[ssd1677_command_set]].
- **Enum hardware di OpenEPaperLink** (`docs/openepaperlink/oepl-definitions.h`): ogni diagonale
  con variante 4 colori ha la coppia BWR/BWY (`_16` 0x30/0x38, poi `_22`, `_26`, `_29`, `_42`,
  `_60`, `_75`, `_116` 0x37/0x3F). Per la 9.7" esiste solo `SOLUM_M3_BWR_97` (0x2E): **nessun
  `BWY_97`**.
- **Il driver SSD di OEPL rifiuta 4 colori sulla 9.7"**: in
  `docs/openepaperlink/oepl_display_driver_unissd.c` il ramo `960x672` chiama
  `oepl_hw_crash("Invalid colors for 9.7\" SSD")` per `num_colors != 3`.
- **I pannelli SOLUM BWRY veri usano un altro controller e un altro protocollo**:
  `docs/openepaperlink/oepl_display_driver_ucbwry.c` è famiglia UC/JD (`0x00` PSR, `0x04` power on,
  `0x10` DTM1, `0x12` refresh, `0x61` TRES), nessun `0x24`/`0x26`/`0x44`/`0x4E`. **Un frame solo**,
  2 pixel per byte, codice a 2 bit nel nibble: `00` nero, `01` bianco, `10` giallo, `11` rosso.

Lettura corretta dello schema a 2 bit, quindi: sui pannelli BWRY riconosciuti da OEPL i due bit
stanno in un nibble di un unico stream `0x10`, **non** in due piani separati — e questo silicio non
è quel controller, risponde al command set SSD1677.

**Table 6-4 del datasheet dà i nomi alle quattro combinazioni**, e cambia il modo di leggere la
sonda. "RAM bit and LUT mapping for 3-color display": `(RED 0x26, BW 0x24)` = `(0,0)` nero LUT0,
`(0,1)` bianco LUT1, `(1,0)` rosso LUT2, `(1,1)` rosso **LUT3 = LUT2**. Tre conseguenze:

1. `LUT3` è una **LUT distinta nel silicio**, che la waveform a 3 colori si limita ad aliasare su
   `LUT2`. L'SSD1677 ha `LUT0..LUT4` (§6.7: 112 byte, 10 gruppi × 4 fasi, quattro livelli di
   sorgente VSS/VSH1/VSH2/VSL). Cade quindi l'argomento "due piani a 1 bit, quindi 3 colori":
   **architetturalmente il chip può fare quattro stati**, e decide l'OTP.
2. Quella che il firmware scrive per il rosso è `LUT3`, non `LUT2`: un pixel rosso esce dalla catena
   come `0x24 = 1`, `0x26 = 1` (`0x24` senza invert, `0x26` con `!invert`).
3. **L'indice di LUT è la coppia `(bit di 0x26, bit di 0x24)`, sempre**: Table 6-4 (3 colori) e
   Table 6-5 (BW) indicizzano entrambe così, e fra le due cambia solo l'aliasing che la waveform di
   fabbrica mette dentro le LUT. Non esiste nessun modo di `0x22` che cambi quella mappa, ed è il
   presupposto su cui poggia tutto il partial: con una LUT custom le quattro combinazioni diventano
   quattro transizioni distinte.

**Un discriminante che sembrava esserci e non c'è.** Il datasheet dell'SSD2677 è ora archiviato in
`docs/` (Rev 1.0 2024-03 e Rev 1.1 2023-08): 960 × 680 come l'SSD1677, RAM a **2 bit/pixel**,
licenza E Ink Spectra 3100, e preset di risoluzione `RES[1:0]` = **960 × 680 / 960 × 672 /
960 × 640 / 880 × 528**, cioè su misura per le taglie SOLUM, la nostra 960 × 672 compresa. Verrebbe
da concludere "il nostro parla SSD16xx, quindi è a 3 colori": **non regge**, per il punto 1 qui
sopra — il tipo di chip non decide il numero di colori. Sul perchè i due protocolli siano comunque
incompatibili fra loro, vedi il vincolo "SSD2677 non è un'alternativa" più sotto.

- Il driver 9.7" scrive **solo** `0x24` e `0x26`: `0x28` non è nel path di nessuna API, e le
  primitive del terzo piano non esistono qui. Un descrittore `FORMAT_BWRY_1BPP` passato a
  `GxEPDImage::showImage()` rende `data0` e `data1` e ignora `data2`, che è il contratto della
  libreria e vale per entrambi i driver.
- **Niente read-back**: sul FPC 24 pin non esiste una linea SDO (pin 12 = SDI e basta, confermato
  sullo schematico Waveshare V3 in `docs/E-Paper_ESP32_Driver_Board_V3.pdf`). Inutilizzabili tutti i
  comandi di lettura del controller — `0x1B` temperatura, `0x27` read RAM, `0x2E` User ID da OTP,
  `0x2F` status — elencati in [[ssd1677_command_set]]. La diagnostica è solo visiva. La sonda li
  tenta comunque e usa `0x2F`, che ha POR noto `0x01` con chip ID `01`, come prova di validità del
  percorso: se quello non torna, dichiara inattendibili anche gli altri due invece di stampare
  numeri senza senso. Ricablare la linea avrebbe senso solo per `0x2E` e per i byte G..J di `0x37`
  (module ID / waveform version), che identificherebbero il modulo senza ambiguità.
  **Il limite è del connettore della board, non del pannello**: il tag di fabbrica usa un pin del
  cavo (`EPD_VPP`) come MISO e legge davvero i registri, e sullo stesso cavo seleziona una EEPROM
  montata sul pannello (`EPD_HLT`) — vedi [[oepl_nrf52811_tag_fw]]. Quindi la linea di lettura
  esiste sul lato pannello e su una coda cablata a mano si può ricavare; è così che `0x2E` diventa
  raggiungibile invece di essere impossibile per costruzione.
- La sonda non cambia le misure ma porta in testa e nella scheda di osservazione le evidenze
  documentali: la UICR di fabbrica che dà il pannello per BWR (serve a sapere quale esito
  sorprende, non a decidere al posto dell'occhio), il distinguo connettore/pannello sulla lettura,
  le due vie di identificazione che costano un minuto — serigrafia sul vetro ed etichetta col part
  number del pannello — e l'avvertenza sullo switch del booster della board, che va escluso prima
  di attribuire alla waveform colori deboli o ghosting ([[waveshare_esp32_driver_board]]), più due
  pin del controller che su una coda cablata a mano vanno guardati: **BS1** (L = 4 fili, H = 3 fili
  a 9 bit: flottante può far ignorare tutto il traffico) e **M/S#**, che su un pannello a chip
  singolo va a VDDIO. Sia lo sketch sia l'header del driver avvertono ora che il datasheet in
  `docs/` è la Rev 1.0 e il silicio sembra più recente, quindi "non è nel datasheet" non equivale a
  "non esiste nel chip".
- **Sonda**: `A:\epd\GxEPD2_SOLUM_ESL\examples\097c\panel_diagnostic\panel_diagnostic.ino`, cioè
  dentro il **submodule**, non nel progetto consumer: appartiene al processo di costruzione del driver, e
  `examples/` è la posizione che la convenzione delle librerie Arduino prevede per gli sketch.
  Sketch autonomo (solo `SPI.h`, nè GxEPD2 nè il driver custom). Non viene compilato dal build del
  firmware: Arduino concatena i `.ino` solo dalla root dello sketch e ricorre soltanto in
  `<sketch>/src/`. È uno **strumento di misura**:
  non conclude, mette il pannello in condizione di rispondere e chiude con una scheda di
  osservazione che mappa ogni esito visibile sulla conseguenza per il driver. Cosa esercita:
  - 4 bande orizzontali alte 168 px con **tutte** le combinazioni dei due piani, numerate 1..4 con
    un font 5x7 scalato ×8 disegnato dentro la banda stessa (in basso a sinistra, per non invadere
    la fascia `0x28`), così non serve contare le fasce dall'alto. La cifra si compone riga per riga
    a larghezza piena: nessuna finestra parziale lungo X. Bande orizzontali di proposito, la
    finestra RAM varia solo lungo Y come fa il firmware a ogni page;
  - `0x28 = 0xFF` sui primi 84 px della banda bianca, con lettura del BUSY subito dopo il comando
    per distinguere una scrittura RAM da un comando analogico;
  - due passate di refresh, `0x22 = 0xF7` (DISPLAY Mode 1) e poi `0xFF` (Mode 2), con pausa di 45 s
    fra le due per guardare il pannello prima che la seconda sovrascriva;
  - i comandi Auto Write Pattern (`0x47` su `0x24`, `0x46` su `0x26`), poi ricoperti dalle bande;
  - il **refresh differenziale**: porta entrambi i piani a nero col pattern e fa un refresh pieno,
    così `0x26` è un frame precedente coerente con quello che si vede, poi riscrive solo `0x24` e
    lancia `0x22 = 0xFC` due volte di seguito su due inversioni piene. Se la passata 2 esce
    **rossa**, `0xFC` non sta selezionando Mode 2 e `0x26` è stato riletto come accent: è l'unico
    esito che lo dimostra, perchè nella passata 1 `0x26` vale 0 e un fallback sarebbe invisibile;
  - quale parametro di `0x10` fa davvero dormire il controller, col BUSY come testimone.
  Non esercita di proposito il dithering `0x25`: scrive lui stesso nella BW RAM dal cursore e
  sporcherebbe le bande.
  **Trappola da non reintrodurre**: qualunque misura che usi la salita del BUSY come prova che un
  comando è stato accettato viene ingannata da un BUSY lasciato alto da un comando precedente, e
  `0x28` è precisamente un comando che lo alza. Per questo il benchmark del bus si appoggia a `0x26`
  e non a `0x28`, e `ensureBusyLow()` fa da guardia prima della validazione del pattern, di ogni
  passata di refresh e della scrittura delle bande. Sul seriale, oltre ai colori: ambiente ESP32 e clock SPI effettivo, init
  spacchettata con la durata reale del BUSY dopo SWRESET, benchmark del bus a blocchi crescenti
  (compresi i due che il driver usa davvero, 120 e 256 byte) e costo delle transazioni a singolo
  byte, µs/byte e MB/s per ogni fascia, durata e guadagno del pattern hardware, durata di ogni
  passata di refresh con rilevamento multi-fase, riepilogo tempi e stima del ciclo completo.

**Sequenza di init di fabbrica della 9.7"**, estratta dal firmware stock SOLUM dal progetto
OpenEPaperLink e commentata `// stock init 9.7"` in `docs/openepaperlink/oepl_display_driver_unissd.c`
(ramo `x_res == 960 && y_res == 672`):

```
0x46 {0xF7}  + 15 ms        Auto Write RED RAM pattern, tutta la RAM a 1
0x47 {0xF7}  + 15 ms        Auto Write B/W RAM pattern, tutta la RAM a 1
0x0C {0xAE,0xC7,0xC3,0xC0,0x80}   soft start
0x01 {0x9F,0x02,0x00}       MUX 671 -> 672 gate line
0x11 {0x02}                 entry mode: Y increment, X DEcrement
0x44 {0xBF,0x03,0x00,0x00}  finestra X 959 -> 0
0x45 {0x00,0x00,0x9F,0x02}  finestra Y 0 -> 671
0x3C {0x01}                 border waveform
0x18 {0x80}                 sensore di temperatura interno
0x22 {0xF7}
0x21 {0x08,0x00}            BW RAM Inverse ("fix reversed image")
poi 0x4E {0xBF,0x03} e 0x4F {0x00,0x00}, 0x24 piano nero, 0x26 piano rosso, 0x22 {0xF7}, 0x20
```

Coincidono col nostro driver: `0x0C`, `0x01`, `0x3C`, `0x18` e `0x22 {0xF7}` -> `0x20`. Divergenza
innocua sull'indirizzamento: lo stock usa `0x11 = 0x02` con finestra X discendente, X start 959 e
`0x21 = 0x08` (= *BW RAM Inverse*, A[3:0]=1000) per raddrizzare l'immagine; noi usiamo
`0x11 = 0x03`, X start 0 e invertiamo il piano rosso in software. `0x21` non viene mai scritto dal
driver, quindi resta al POR `0x00` = entrambi i piani Normal. Due convenzioni equivalenti.

Il valore `0xF7` di `0x22` è quello di fabbrica: enable clock, load temperature, load LUT DISPLAY
Mode 1, disable clock. Il Mode 2 (`0xB9` / `0xFF`) è il secondo banco di waveform, quello
differenziale: inutilizzabile qui perchè userebbe `0x26` come buffer "precedente" e su questo
pannello `0x26` è il rosso — da cui `hasFastPartialUpdate = false`.

**Identificazione del pannello.** Il donor è un **Newton Pro gen. F5**, `EL097F5C4C/WWW` (cornice
grigia): senza pulsanti e senza IP68, che sono i due tratti esclusivi del F6. La pagina 9.7" del F5
dichiara `PIXEL COLORS = BWRY`.

**Decodifica del codice modello**, dal datasheet ufficiale
`docs/Newton-PRO_Data-sheet_C-Lab_G_240320.pdf` (REV 1.0, generazione F6, §3.6 Label Marking):
`EL` + `097` diagonale + `F6` generazione + `W` colore housing (`W` white, `B` black) + `4` colore
display (**`4` = RED, YELLOW, cioè BWRY**) + `A` tag type (`A` = Button, NFC, LED).

Quindi nel nostro `EL097F5C4C` la cifra `4` decodifica come BWRY e la `C` finale è un tag type
diverso dall'`A` — coerente con un'unità senza pulsante. Ma la stessa pagina generale del datasheet
mette la riga `Display Colors: BWRY` per tutta la linea Pro 1.6"-12.2" con la nota
**"color options are not available for all sizes"**, e nel datasheet F6 *ogni* modello elencato ha
la cifra `4`: potrebbe essere una SKU di famiglia e non una garanzia per taglia. L'ipotesi è che la
9.7" sia una delle taglie in cui l'opzione 4 colori non viene prodotta, e la sostengono l'enum OEPL
e il crash del driver unissd; contro c'è la pagina 9.7" del F5, che dichiara BWRY per quella taglia
specifica. Non è dirimente in nessuna delle due direzioni: serve la sonda.

**C'è una via di verifica più diretta della sonda: l'etichetta sul vetro.** Le foto interne delle
pratiche FCC mostrano che SOLUM serigrafa i colori **sul pannello stesso**, taglia per taglia e
unità per unità: il 9.7" `EL097F6W4A` porta `NEWTON PRO 9.7" BWRY Normal`, mentre 11.6" e 12.2"
della stessa generazione portano `BWR normal`. Quindi **leggere l'etichetta del donor del progetto
dice se il film è BWR o BWRY** senza misurare niente.

Questo però non chiude la questione, la sposta: dimostra che un 9.7" BWRY si produce e che la
cifra `4` del codice modello non è solo catalogo, ma non dice nulla sul donor del progetto (che è
un F5 `EL097F5C4C`, l'etichetta letta è di un F6). Ed è **compatibile** con i riscontri qui sopra:
se i pannelli SOLUM BWRY veri usano il controller UC/JD a nibble da 2 bit, il 9.7" BWRY può essere
proprio una variante con silicio diverso da quello del donor, che risponde a SSD1677. Le due letture
restano entrambe in piedi; a deciderle basta l'etichetta del proprio pannello, e in subordine la
sonda.

Il PDF sta in `GxEPD2_SOLUM_ESL/docs/097c/fcc/fcc_2AFWN-EL097F6W4A_internal_photos.pdf`, i JPEG del
pannello e della scheda tag nella stessa cartella; il confronto fra le tre taglie è in
`GxEPD2_SOLUM_ESL/docs/122c/identificazione_pannello.md` ([[gxepd2_122c_driver]]). In `docs/` il
materiale è separato per pannello: `097c/` il 9.7", `122c/` il 12.2" con dentro il confronto 11.6",
alla radice quello che vale per entrambi (SSD1677, datasheet Newton, board Waveshare, sorgenti OEPL).

- Nelle pagine F5 del PDF `pdftotext -layout` sfalsa le etichette di una riga rispetto ai valori
  (si vede da `ACTIVE DISPLAY AREA -> 168.05 x 224.07 x 14.81 mm`, che è la DIMENSION): va
  riallineato prima di citare quei campi.
- La cornice grigia non discrimina F5 da F6 (il F6 dichiara `Grey / White / Black / Customizable`)
  ma esclude il Core, che in catalogo ha solo `White or Black`. Nemmeno il LED a 7 colori
  discrimina: c'è su entrambi i Pro.
La sua §0 tiene le caratteristiche del pannello: codici modello SOLUM (`EL097F5*4C` / `EL097F6*4A`
Pro, Newton Core), pitch 0,210 mm e 120,95 dpi, range 0~40 °C, pinout FPC 24 pin, volumi di
transfer SPI.

Dal datasheet ufficiale F6, dati non presenti nelle pagine di catalogo:

- **storage temperature dichiarata**: 0~40 °C @45~70% RH, identica alla operating (BWRY)
- 9.7" F6: 170,2 × 223,6 × 14,85 mm, 386,60 g; display 672 × 960 @121 dpi = 141,1 × 201,6 mm
- batterie 5× CR2450 per 9.7" e 11.6" (6 per la 12.2"), viewing angle "nearly 180°"
- radio: 2.4 GHz ISM, **PHY BLE** con protocollo proprietario SOLUM, AES-128, 30 m LoS, Tx 4 dBm /
  10 mA, sensibilità Rx -85 dBm
- nessun tempo di refresh dichiarato, come in tutte le pagine SOLUM
- la 11.6" è 640 × 960 @100 dpi: è la stessa geometria del `GDEY116F51` di GxEPD2 (11.6" 4 colori,
  controller **SSD2677**, 2bpp packed su `0x10`), cioè il vetro di `SOLUM_M3_BWY_116`. Conferma il
  pattern: SOLUM 4 colori = famiglia UC/SSD2677, non SSD1677

**Vincoli architetturali** quando si lavora sul driver:

- **Origine**: derivato da
  [`GxEPD2_1330c_GDEM133Z91`](https://github.com/ZinggJM/GxEPD2/blob/1.6.9/src/gdem3c/GxEPD2_1330c_GDEM133Z91.cpp)
  (13.3" 3-colori, 960×680). In GxEPD2 1.6.9 i driver SSD1677 sono **nove**, ma i 3-colori sono
  tre, e questo è il più vicino al SOLUM. Confrontati con la sequenza di init di fabbrica:

  **Tutti e nove, letti in 1.6.9**, con quello che li distingue. `0x0C[4]` è il quinto byte del
  soft start, `off` il parametro di `0x22` del power off, `sleep` quello di `0x10`:

  | driver | geometria | col. | partial | `0x0C[4]` | init | refresh | off | sleep |
  |---|---|---|---|---|---|---|---|---|
  | `gdem3c/GDEM133Z91` | 960×680 | BWR | **no** | `80` | `0x0C`,`0x01`,`0x3C{01}`,`0x18{80}` | `F7` | `C3` | `11` |
  | `epd3c/750c_Z90` | 880×528 | BWR | **no** | `40` | + `0x22{B1}`/`0x20` | `C7` | `C3` | `11` |
  | `gdey3c/GDEY116Z91` | 960×640 | BWR | **no** | — | solo SWRESET + `0x3C{01}` | `F7` | `C3` | `11` |
  | `epd/1160_T91` | 960×640 | B/N | `0x32`+`CC` | `40` | + `0x22{B1}`/`0x20` | `F4` con `_PowerOn` | `83` | `03` |
  | `gdem/GDEM133T91` | 960×680 | B/N | `FC` | `80` | come Z91, `delay(15)` | `F7` | `83` | `03` |
  | `gdem/GDEM102T91` | 960×640 | B/N | `FC` | **`FF`** | come Z91 | `F7` | `83` | `03` |
  | `gdem/GDEM0397T81` | 800×480 | B/N | `0x21{00 00}`+`FC` | `80` | `0x18` **prima** di `0x0C`, MUX B=`02` | `F7`, fast `0x1A{6A}`+`D7` | `83` | `01` |
  | `gdeq/GDEQ0426T82` | 800×480 | B/N | idem | `80` | idem | `F7`, fast `0x1A{5A}`+`D7` | `83` | `01` |
  | `epd/370_TC1` | 280×480 | B/N | `CF` | `C0` | scrive `0x03`,`0x04`,`0x2C{44}`,`0x37`, LUT via `0x32` | `CF` | `83` | `03` |

  **Il fatto che orienta la scelta della base: nessuno dei tre driver a 3 colori ha il partial, e
  tutti e sei i monocromatici ce l'hanno** con `0x26` come frame precedente. È la ragione per cui il
  partial del driver custom non viene dalla sua base ma dal `1160_T91`.

  GDEM133Z91 ha **quattro comandi su cinque byte-identici** al firmware SOLUM (`0x0C` col quinto
  byte `0x80`, `0x3C{01}`, `0x18{80}`, `0x22{F7}`); differiva solo il conteggio gate di `0x01`.
  GDEY116Z91 lascia tutto il front-end analogico al POR e andrebbe riscritto; 750c_Z90 ha il soft
  start col quinto byte `0x40`, MUX 527 e refresh `0xC7`, che **non carica la temperatura** mentre
  la waveform di questo pannello è compensata. La geometria non discrimina: `HEIGHT` la definisce il
  driver custom a 672, conta solo `WIDTH = 960`, cioè righe da 120 byte.

  **`GDEQ0426T82` e `GDEM0397T81` sono il riferimento SSD1677 moderno della libreria**, e portano
  quattro idiomi che il driver custom non ha: `0x21` a due byte **prima di ogni refresh** (`{40 00}`
  bypassa la RED RAM sul pieno, `{00 00}` la lascia normale sul partial), `0x18` scritto **prima**
  di `0x0C`, il terzo byte di `0x01` a `0x02` (bit SM, scansione interlacciata), e il banco caldo
  chiesto con `0x1A` + `0x22 = 0xD7` invece che con `0xF7`.

  **`0x0C[4] = 0xFF` di GDEM102T91 è fuori dalla tabella del datasheet**, che per il soft start
  documenta solo `0x40` (Level 1) e `0x80` (Level 2): resta fuori dalle sequenze riprodotte.

  **Convenzione del MUX, ed è una trappola quando si legge un sorgente upstream**: il valore di
  `0x01` è il **registro**, e le gate line sono una in più. `0x9F 0x02` = 671 -> 672 linee,
  `0xA7 0x02` = 679 -> 680. Da qui viene anche il commento sbagliato `// Set MUX as 527`, copiato da
  750c_Z90 dove 527 è giusto (`0x020F` = 527 -> 528 gate = la sua `HEIGHT`).

  **I parametri di `0x10` si dividono in due gruppi, e non è una preferenza di stile**: `0x01` e
  `0x11` hanno entrambi A[1:0] = 01, quindi chiedono la **stessa** cosa, il deep sleep **modo 1**;
  `0x03` ha A[1:0] = 11, cioè il **modo 2**. Sul SSD1683 il modo 1 *"Retain RAM data but cannot
  access the RAM"* a 3 µA, il modo 2 *"Cannot retain RAM data"* a 1 µA (tabella elettrica, verbatim).
  Il driver custom manda `0x03` e per questo `_InitDisplay()` riarma `_initial_write` al risveglio:
  se questo silicio si comportasse come l'SSD1683, `0x01` conserverebbe la RAM al prezzo di ~2 µA, e
  quel riarmo diventerebbe inutile. La ritenzione è una misura e non una deduzione: la sonda del
  deep sleep di `examples/097c/panel_diagnostic` la esercita facendo, dopo il risveglio, un refresh
  **senza riscrivere la RAM**.

- **SSD2677 non è un'alternativa**: è un altro protocollo, e la scelta la determina il silicio, non
  la preferenza. Comandi scritti dai due driver: SSD1677 (`GDEM133Z91`) usa
  `01 0C 10 11 12 18 20 22 3C 44 45 4E 4F`, SSD2677 (`epd4c/GDEY116F51`) usa
  `00 01 02 03 04 06 07 10 12 30 41 50 60 61 62 65 83 E0 E3 E7 E9`. L'intersezione è di tre opcode
  e tutti e tre significano cose opposte: `0x01` MUX contro PWRR, **`0x10` deep sleep contro DTM1**
  (il trasferimento immagine), **`0x12` SWRESET contro display refresh**. Un driver derivato da
  SSD2677 su questo pannello manderebbe `0x10` per iniziare l'immagine, cioè lo addormenterebbe, e
  `0x12` per il refresh, cioè lo resetterebbe. SSD2677 (con template `GxEPD2_4C`, base
  `GDEY116F51`, 2bpp packed su `0x10`) è la strada giusta solo per un pannello davvero a 4 colori:
  i SOLUM BWY fino alla 11.6", non questa 9.7".

- **Il partial esiste, 639 ms misurati sul vetro, e vive fuori dal template.** Dettaglio completo
  nella sezione "Partial in bianco e nero" più sotto. `hasFastPartialUpdate = false` non è prudenza
  ma struttura: in modalità partial `GxEPD2_3C` scriverebbe il piano accent dentro `0x26`
  (`GxEPD2_3C.h:340`), che sotto la LUT custom è il frame precedente, e con il flag alzato
  ripeterebbe anche l'intero loop paged (`GxEPD2_3C.h:354-358`).
  Non serve invece niente dell'OTP: `0xFC`, `0xFF`, `0xCF` e `0xC7` misurano tutti 24,6-24,8 s,
  quindi **in OTP c'è una sola waveform** e il differenziale del silicio non è una scorciatoia.

- **`GxEPD_YELLOW` finisce sul rosso, ed è corretto**: il template upstream `GxEPD2_3C` mappa
  `GxEPD_YELLOW` sul piano red (`GxEPD2_3C.h:196`), e su questo film è l'unico esito possibile
  perchè un terzo colore non c'è. Non è una trappola da aggirare.

- **Page-tracking `_show_image_page_hint`**: contatore parallelo a `_current_page` (privato in
  `GxEPD2_3C`), avanzato in `writeImage(black, color, ...)` (che il template chiama da `nextPage()`
  in full-window), azzerato in `setPaged()` e `_Update_Full()`. Regge il row-skip di `showImage`
  senza modificare GxEPD2.

- **API pubblica usata dallo sketch**: solo `GxEPDImage::showImage(display, *desc)` dentro un loop
  `firstPage/nextPage`. Il resto (API single-channel, partial) è compositing avanzato.

- **`showImage` hardcoda `pgm=true`** (`src/GxEPD2_SOLUM_097c_960x672.h:185` e `:221-223`): vale per
  immagini pre-compilate nello sketch, non per buffer scaricati via HTTP.

- **Variante 122c**: il pannello SOLUM 12.2" (960w × 768h) ha il proprio driver
  `GxEPD2_SOLUM_122c_960x768` nello stesso `src/` della libreria: due controller SSD16xx da
  960 × 384 con lo split sull'asse gate, BUSY attivo alto, e la seconda coda muta perchè è quella
  dello slave di una coppia in cascade — vedi [[gxepd2_122c_driver]]. Lo sketch
  sceglie il driver via `Layout::Panel` / `Layout::makePanel()` (in `Layout_097c.h` /
  `Layout_122c.h`) — vedi [[layout_separation]].

- **Il test a MUX ridotto a banda non va eseguito**: l'utente ha deciso di non percorrere quella
  strada. Il codice del test sta nel probe `A:\rd\probe_refresh_097c` e resta fermo.

**Implementazione e costi.** Le cifre in ms sono *derivate* dal clock SPI (0,8 µs/byte a 10 MHz,
limite fisico del bus), non misurate a oscilloscopio: per numeri veri serve una misura sul campo.

- MUX (`0x01`) programmato per **672 gate lines** (`0x9F 0x02 0x00`), quante il pannello ne ha
  davvero, **identico al firmware SOLUM di fabbrica** (vedi la sequenza di init sotto). Il `0x2A7`
  = 679 ereditato dal GDEM133Z91 è il default di power-on del chip (datasheet: `A[9:0] = 2A7h
  [POR], 680 MUX`), tenuto perchè quel pannello ha davvero 680 linee. Atteso ~260 ms in meno sui
  22 s di refresh.
- **Riempimento piani via pattern hardware**: `_fillPlaneByPattern()` usa Auto Write RAM for
  Regular Pattern — `0x47` per `0x24`, `0x46` per `0x26` — con parametro `A[7]` = valore del primo
  step, `A[6:4] = 111` step height 680, `A[2:0] = 111` step width 960, cioè un unico step su tutta
  la RAM nativa: `0xF7` riempie di 1, `0x77` di 0. Un byte sul bus invece di 80.640, ~15 ms invece
  di ~65. Il generatore emette un livello per step, quindi copre solo i valori a bit uniformi
  (`0x00` / `0xFF`) e solo i due piani immagine: per `0x28` e per qualsiasi altro valore
  `_writeScreenBuffer` ripiega sul transfer SPI in bulk. Il pattern ignora la finestra di
  `0x44`/`0x45` e riempie tutti i 960x680; le 8 gate line oltre la 672 non vengono mai scandite.
- dirty flag `_color_dirty`: il cleanup accent viene saltato quando non serve (tipico sulle catene
  di frame B/N). Costa 8-9 ms, perchè `0x26` si pulisce col pattern hardware.
- `hibernate()` idempotente, azzera i due flag di `0x26` (`_color_dirty` e
  `_previous_in_color_ram`): il deep sleep **perde** la RAM del controller, e la ragione è quella,
  non il SWRESET, che dichiara *"RAM are unaffected by this command"*. Per lo stesso motivo
  `_InitDisplay()` riarma `_initial_write` quando arriva da un hibernate: al risveglio i piani sono
  indefiniti, non azzerati, e la prima scrittura deve ripulirli (18 ms col pattern hardware).
- cleanup accent simmetrico tra `writeImage` e `writeImagePart` BW.
- row-skip di `showImage` via page-hint: il loop pixel gira una volta per refresh invece di otto
  (~24 ms invece di ~192 ms).
- SPI in bulk con `_pSPIx->writeBytes(buf, n)` in `_writeImage` / `_writeImagePart` /
  `_writeScreenBuffer`: ~129 ms per il loop paged (161.280 byte), ~258 ms per il refresh BWRY
  completo (322.560 byte). La `_writeData(buf, n)` di `GxEPD2_EPD` è un loop per-byte (~1,5 µs/byte)
  e non va usata sui hot path.
- il refresh elettroforetico dura ~22 s: limite fisico del pannello, non riducibile via software.
  L'overhead software pre-refresh è ~30 ms, non c'è più margine da quel lato.

**Fonti archiviate in `A:\epd\GxEPD2_SOLUM_ESL\docs\`** (versionate nel submodule, così le
conclusioni sono riverificabili senza rifare le ricerche):

- `SSD1677_Rev1.0_2018-11_Solomon-Systech.pdf` — 47 pagine, command table completa. Estrarre con
  `pdftotext -layout`; la command table esce con le colonne sfalsate, va letta con attenzione.
  `pdftotext` non c'è su questa macchina: metodo alternativo e contenuto già estratto in
  [[ssd1677_command_set]], da consultare prima di riaprire il PDF
- `Newton-PRO_Data-sheet_C-Lab_G_240320.pdf` — 53 pagine, datasheet ufficiale generazione F6:
  label marking e decodifica del codice modello, dimensioni per taglia, batterie, radio
- `Newton-Pro_Specifications.pdf`, `Newton-Core_Specifications.pdf` — pagine di catalogo
- `E-Paper_ESP32_Driver_Board_V3.pdf`, `waveshare-E-Paper_ESP32_Driver_Board_manual_en.pdf` —
  schematico e manuale della board, da cui il pinout FPC 24 pin e l'assenza di SDO
- `openepaperlink/oepl-definitions.h` — enum dei tag type (`SOLUM_M3_BWR_97` = 0x2E)
- `openepaperlink/oepl_display_driver_unissd.c` + `.h` — driver SSD16xx di OEPL, contiene la
  sequenza di init di fabbrica della 9.7"
- `openepaperlink/oepl_display_driver_ucbwry.c` + `.h` — driver dei pannelli SOLUM BWRY veri,
  con la codifica a 2 bit per pixel

Toccando il driver, aggiornare anche la tabella di confronto e i bullet di dettaglio nel suo README,
così la doc resta allineata al codice.

## Partial in bianco e nero: 639 ms, e la waveform va scritta COMPLETA

**La cosa da non dimenticare mai su questo controller: un waveform setting è di 110 byte e `0x32`
ne scrive 105.** I cinque che restano sono le tensioni, e hanno comandi propri: byte 105 VGH via
`0x03`, 106..108 VSH1/VSH2/VSL via `0x04`, 109 VCOM via `0x2C`. Chi scrive solo `0x32` eredita le
tensioni dall'ultima scrittura, e su questo pannello quella è quasi sempre la waveform **BWR di
produzione tarata su 24 s**, perchè il bit 4 di `0x22` (`0xF7` di `_Update_Full`) ricarica dall'OTP
tutti i byte `0..109`, tensioni comprese. Il SWRESET invece li riporta ai POR.

È stato un bug reale del driver, e si manifestava come **nero pallido invece di nero pieno**, a
durata identica perchè il tempo lo fissano `TP` e `RP` e non dipende dalla tensione. La diagnosi è
venuta dal confronto fra due misure con la stessa LUT e la stessa sequenza di comandi:

| chi | sequenza | tensioni durante il partial | nero |
|---|---|---|---|
| `panel_diagnostic` | reset HW, SWRESET, config, LUT. **Nessun load dall'OTP** | POR, VSH1 15 V | **pieno** |
| catena di partial | dopo un refresh pieno (`0xF7`) | quelle dell'OTP | **pallido** |

Il probe dei livelli, anch'esso a POR, aveva già misurato che su questo film **VSH1 dà il nero e
VSH2 il rosso**: il nero di questo pigmento si ottiene a 15 V, il rosso a 5 V. Da qui la scelta del
driver, che in `_Init_Part()` manda `0x32` **più `0x04` con i POR** (`0x41, 0xA8, 0x32` = VSH1 15 V,
VSH2 5 V, VSL −15 V). VGH e VCOM restano dell'OTP di proposito: VGH pilota i transistor e non il
pigmento, il VCOM governa il bilanciamento DC ed è tarato di fabbrica, e il suo POR `0x00` non
compare nemmeno nella tabella del datasheet. Il ritorno alla waveform di produzione è automatico,
perchè `0xF7` ricarica anche le tensioni.

**Modello temporale, validato su due punti a 7× di distanza**: 28 frame in 641 ms e 200 frame in
4067 ms danno pendenza **19,92 ms per frame**, cioè 50 Hz allo 0,4%, e intercetta **83 ms**, che è
la rampa enable clock + enable analog dentro la sequenza di `0x22` (il `_PowerOn` isolato misura
82 ms).

> **durata del partial = 20,0 ms × frame + 83 ms**, più ~12 ms di push SPI per una fascia.

I cinque byte di frame rate della LUT valgono `0x22` = codice `0010` = 50 Hz. Il refresh pieno
dell'OTP a 24 s sono quindi ~1200 frame contro i 28 della LUT del partial.

**Semantica della LUT del partial**, verificata pixel per pixel contro le osservazioni sul vetro:
l'indice è `(bit di 0x26, bit di 0x24)` = `(frame precedente, frame nuovo)`, `LUT0 (0,0)` e
`LUT3 (1,1)` a zero (pixel fermo, ed è **questa** la confinatura del partial, non la finestra RAM),
`LUT1 (0,1)` nero→bianco a VSL, `LUT2 (1,0)` bianco→nero a VSH1. Nella catena di partial
la fascia aveva `(1,0)` → LUT2 → 18 frame VSH1 → nera; il resto `(1,1)` → LUT3 → zeri → intatto; il
testimone nero `(0,0)` → LUT0 → zeri → intatto. Ogni pixel osservato torna.

**`TP` e `RP` sono per GRUPPO, non per LUT**: un solo set di lunghezze di fase condiviso da
`LUT0..LUT4`, quindi non si può allungare il drive del nero senza allungare quello del bianco, e le
due transizioni stanno negli stessi 28 frame. L'unico modo di rompere la simmetria è la fase di
reset, che ha polarità opposta nelle due LUT.

**Quello che NON serve, ed è misurato**: una catena di partial non ha bisogno di refresh pieni
periodici. Undici passate consecutive attraverso il driver non hanno degradato nè i testimoni nè il
fondo, e un solo refresh pieno finale riporta il vetro netto con l'accent rosso saturo. I pixel non
pilotati non sbiadiscono: quello che sembrava sbiadimento nella sonda a SPI diretta era pilotaggio
non voluto, perchè lì `0x26` non era allineata e quei pixel cadevano su LUT1 o LUT2.

**Le due trappole dell'API, chiuse nel driver dal flag `_previous_in_color_ram`.** Sotto la LUT
custom `0x26` è il frame precedente in polarità BW, e un refresh pieno la rilegge come **accent**:
`0xFF` vuol dire rosso, quindi una `refresh(false)` nuda dopo una catena dipingerebbe lo schermo di
rosso. La seconda è più insidiosa: `writeImage(black, color, ...)` scrive `0x26` **una page alla
volta**, quindi un frame a colori dal loop paged dopo una catena uscirebbe con sette ottavi di rosso
spurio. Il flag lo alzano soltanto `writeScreenBufferPrevious()` e `writeImagePrevious()`, quindi
quando è alto l'ultima scrittura in quella RAM *è* un frame precedente e un refresh pieno non ha
nessun uso possibile per quel contenuto: cancellarlo non può mai distruggere un accent legittimo.
`_cleanColorIfPrevious()` è agganciata a `_Update_Full()` e a **`setPaged()`**, che è l'unico punto
in cui la pulizia è completa perchè `GxEPD2_3C::firstPage()` riempie solo il buffer locale e non ha
ancora scritto sul controller.

**Altre due cose che il driver deve fare e che si dimenticano.** `refreshPartial()` chiama
`_InitDisplay()` se serve, altrimenti una passata come prima operazione dopo `hibernate()` girerebbe
senza SWRESET, MUX e entry mode. `drawImagePartial()` porta la RAM a uno stato definito quando
`_initial_write` è alto: senza, `_writeImage` chiamerebbe `writeScreenBuffer()` che azzera `0x26` a
`0x00`, cioè **frame precedente tutto nero**, e ogni pixel bianco cadrebbe su LUT1 facendo pilotare
tutto lo schermo.

**`setPartialLut(const uint8_t* lut110)`** sostituisce la waveform, e fa scendere il booster:
altrimenti le nuove tensioni verrebbero scritte ad analog già acceso e `_PowerOn()` salterebbe,
lasciando girare la passata con quelle di prima. La taratura di
`examples/097c/panel_diagnostic` rispecchia la stessa sequenza a SPI diretta: sette bande con
**nero di riferimento adiacente** all'area di lavoro (il confronto contro un nero dipinto dall'OTP
nella stessa schermata è la misura che regge tutta la lettura),
sweep di VSH1 da 9 a 15 V, poi frame rate, frame e VGH/VCOM. Tutte le tensioni provate sono al
massimo i POR, mai sopra: salire oltre è la sola cosa che può danneggiare il film in modo
permanente.

Una precisazione che vale per non riaprire un vicolo: il **Display Mode** non è dimostrato
necessario. Le due varianti della sonda cambiavano LUT e modo insieme, quindi Mode 1 con questa LUT
non è mai stato provato, e `0x37` spiega perchè probabilmente non conta (vedi
[[ssd1677_command_set]]). Il driver resta su `0xCC` perchè è la configurazione misurata.

## Quinta evidenza sul quarto colore: la UICR di fabbrica dice BWR

Da `tagtype_db.cpp` del firmware OEPL per tag nRF52811 ([[oepl_nrf52811_tag_fw]]): le due righe
della 9.7" (`9.7 SSD` e `9.7 type 2`) dichiarano controller **0x19**, Xres 0x02A0 = **672**,
Yres 0x03C0 = **960**, solumType 0x64 = `STYPE_SIZE_097` e **terzo colore = 0x01**. Nella stessa
tabella il byte vale **0x02 = BWY** ("4.2 SSD Yellow") e **0x03 = BWRY** (1.6 / 2.4 / 3.0 BWRY),
quindi 0x01 significa **BWR**.

È l'evidenza più forte finora, perchè è **dato di fabbrica scritto nel tag** e non catalogo nè enum
di terzi. Non chiude comunque il caso dell'esemplare del progetto: quelle righe sono tag M3/Core,
il donor è un Pro F5. Restano decisive l'etichetta serigrafata sul vetro e la sonda.

`STYPE_SIZE_097` imposta `drawDirectionRight = true`: OEPL usa lo stesso landscape **960×672** del
firmware di questo progetto, senza mirror.

## Il die non è un SSD1677 Rev 1.0 puro

L'init di fabbrica della 9.7" scrive `0x21` con **due parametri** (`0x08 0x00`), mentre la Rev 1.0
del datasheet SSD1677 definisce quel comando con **un solo** byte e non nomina la cascade. La forma
a due byte è quella del **SSD1683**, dove il secondo byte porta `B[4] ckouten`. Conseguenza: il
silicio SOLUM è della generazione estesa, e il nostro datasheet è più vecchio del chip — utile
saperlo prima di dichiarare "non documentato" un comando che semplicemente non sta in quella
revisione.

Due conferme indipendenti. Un reference SSD1677 di terzi
(<https://github.com/bigbag/papyrix-reader>, `docs/ssd1677-driver.md`) scrive anch'esso `0x21` a
**due** byte, `0x40 0x00`, e commenta il secondo con **"single chip"**: è il `ckouten` dell'SSD1683.
E il pin table della Rev 1.0 contiene già l'hardware della cascade, solo etichettato come riservato:
`M/S#` *"reserved pin, should be connected to VDDIO"* e `CL` dichiarato **I/O**, *"left open in
application"*, presente sia fra i VIH d'ingresso sia fra i VOH d'uscita.

**Non c'è una revisione più recente da cercare**: le copie pubbliche dell'SSD1677 (cursedhardware,
e-paper-display.com) sono la stessa Rev 1.0 del 2018, byte per byte. Il riferimento upstream più
vicino sulla stessa geometria è `epd/GxEPD2_1160_T91` di GxEPD2 (Good Display GDEH116T91,
960 × 640, SSD1677): init identico al nostro tranne l'ultimo byte del soft start `0x0C` (`0x40`
invece del `0x80` di fabbrica SOLUM) e un `0x22 = 0xB1` + `0x20` in coda.

**Danno da UV**: il die è a gold bump senza incapsulamento in resina, l'esposizione UV lo rovina e
sotto sole diretto il pannello sbianca (stesso reference di terzi). Vale per il posizionamento del
pannello e per qualunque montaggio che lasci il COF scoperto. Dallo stesso reference: alcuni
pannelli SSD1677 non tollerano SPI sopra i 10 MHz — il 9.7" gira già a 10, il 12.2" ha il default a
20. Vedi [[ssd1677_command_set]] e [[gxepd2_122c_driver]].

## Pratica FCC 9.7" Core e part number del vetro

Sotto il grantee 2AFWN le pratiche 9.7" sono **quattro**: `EL097R2WRN` (2022-09-08),
**`EL097F5CRC`** (2023-11-28, Core), `EL097F6W4A` (2024-02-29, PRO, l'unica già archiviata),
`EL097H2WRN` (2025-03-17, non esaminata). Le foto interne della F5CRC sono archiviate in
`docs/097c/fcc/fcc_2AFWN-EL097F5CRC_internal_photos.pdf` (1049×787, un quarto della risoluzione
delle pratiche Core 12.2": la serigrafia dei colori sul vetro **non è leggibile**).

Se ne ricavano due cose:

- scheda tag serigrafata **`NEWTON_CORE 9.7_TAG_R01, 2023/08/25`**, una sola FFC 24 pin, una sola
  sezione boost (`097_f5crc_tag_board.jpg`);
- sul retro del vetro un'etichetta bianca **`YMS960672-097AAH-ES-W5`**, data `20230902`. È un
  part number **di pannello** del fornitore
  del vetro — 960×672, 097 — non un codice SOLUM, e non ha riscontri pubblici. **Cercare la stessa
  etichetta sul pannello del progetto**: identifica il vetro meglio del codice ESL.

Nota su `unissd.cpp` upstream: il ramo 9.7" ora è condiviso con l'11.6" e scrive `0x45 = 00 00 7F
02`, cioè finestra Y fino a **639** invece di 671, con MUX ancora a 671. La copia in
`docs/openepaperlink/oepl_display_driver_unissd.c` viene da un'altra base di codice e non ha la
discrepanza.


## Conferma esterna dell'init e della numerazione delle LUT

Good Display **GDEM102Z91** (10.2", 960 × 640, **BWR**, **SSD1677**, FPC 24 pin) è l'analogo
commerciale più vicino alla 9.7": non è un rimarchio, ma il suo demo Arduino è un driver a due
piani per lo stesso controller e conferma l'init di fabbrica SOLUM riga per riga —
`0x0C = AE C7 C3 C0 80` **identico ultimo byte compreso** (GxEPD2 `GDEH116T91` mette `0x40`, il
valore SOLUM è quello che usa anche Good Display), `0x01` con la stessa codifica del MUX,
`0x18 = 0x80`, `0x22 = 0xF7` + `0x20`, piano `0x24` diretto e piano `0x26` scritto **invertito**
(`~datasRW[i]`). Diverge solo l'entry mode (`0x11 = 0x01` contro `0x02`), cioè il verso di scan.

Nello stesso sorgente `0x3C = 0x01` è commentato **"LUT1, for white"**: conferma indipendente che
la numerazione della Table 6-4 è quella vera, quindi la mappa LUT0..LUT3 su cui è costruita la sonda
`panel_diagnostic` non è una ricostruzione ([[ssd1677_command_set]]).

**Sulla questione del quarto colore.** Su tutta la linea grande di Good Display, a parità di
risoluzione e connettore, i 3 colori stanno su SSD1677 e i 4 colori **sempre** su SSD2677
(GDEM102Z91 BWR / GDEM102F91 BWRY, GDEH116T91 B/N / GDEY116F91 BWRY). Il demo del 4 colori non usa
affatto le due RAM: scrive **un solo stream `0x10` a 2 bit per pixel** (00 bianco, 01 giallo,
10 rosso, 11 nero) — lo stesso formato del path `epdvarbwry` di OEPL per le SOLUM BWRY vere. Non
chiude la questione in logica, perchè LUT3 esiste nel silicio e decide l'OTP, ma il quarto colore
vive sempre su un formato che la 9.7" non parla. Dettaglio in [[pannelli_affini_commerciali]].

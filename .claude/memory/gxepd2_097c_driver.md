---
name: Driver custom GxEPD2_SOLUM_097c_960x672
description: Dove vive il driver SOLUM 9.7" SSD1677 (submodule GxEPD2_SOLUM_ESL, libreria Arduino), identificazione del pannello, stato APERTO della questione 4o colore e cosa deve misurare la sonda, sequenza di init di fabbrica, vincoli architetturali e costi noti
type: reference
---

Il driver del pannello SOLUM 9.7" (672w × 960h native portrait, controller SSD1677; bianco, nero e
rosso verificati sul pannello) è una **libreria Arduino a sè stante**, non un pezzo di `A:\epd`:

- submodule al path `A:\epd\GxEPD2_SOLUM_ESL`, branch `main`
- repo <https://github.com/alesimattia/GxEPD2_SOLUM_ESL>, fork di ZinggJM/GxEPD2 in cui l'albero
  upstream non è duplicato: l'unico branch remoto è `main`
- header in `src/GxEPD2_SOLUM_097c_960x672.h`; `library.properties` dichiara
  `depends=GxEPD2 (>=1.6.9),Adafruit GFX Library` e `architectures=esp32`
- licenza **GPL-3.0**, obbligata: il driver è copia modificata di `GxEPD2_1330c_GDEM133Z91`
- include nello sketch, in `Layout_097c.h`:
  `#include "GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM_097c_960x672.h"`
- le modifiche al driver si committano e pushano nel suo repo; nel padre si aggiorna il puntatore
  del submodule. `A:\epd` va clonato con `--recursive` (submodule anche `webapp`)
- upstream GxEPD2 sta in `A:\epd\GxEPD2-master`: clone gitignorato al tag 1.6.9 (`de82887`), copia
  di sola lettura per consultare i sorgenti della libreria

Doc dedicata: [A:\epd\GxEPD2_SOLUM_ESL\README.md](../../GxEPD2_SOLUM_ESL/README.md) — motivazione,
API (`showImage`, `writeImageBlack/Red/Yellow`, `preserveYellow`), pattern "yellow out-of-band",
sistema di descrittori, tabella di confronto con la base GDEM133Z91, dettaglio delle ottimizzazioni.

**QUESTIONE 4° COLORE: APERTA, decide la misura.** La documentazione punta tutta nella stessa
direzione — nessun terzo piano indirizzabile — ma non è mai stata verificata sul pannello, e una
fonte va nel verso opposto (il datasheet SOLUM del donor dichiara `PIXEL COLORS = BWRY` per la
9.7", vedi il blocco identificazione più sotto). Nessuna riga di codice va scritta o cancellata
sulla base di quanto segue prima che la sonda abbia risposto: le conclusioni le tira l'utente
guardando il pannello. Quattro riscontri documentali concordi, tutti archiviati in
`GxEPD2_SOLUM_ESL/docs/`:

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

Cosa resta da misurare, e che la documentazione non può dire:
1. quale colore rende l'accent, cioè la combinazione (BW=1, RED=1): rosso su un film BWR, giallo su
   un film BWY. Il firmware la usa già, quindi la risposta si legge a occhio;
2. cosa rende la quarta combinazione (BW=0, RED=1), che il firmware non genera mai: se è un colore
   distinto sia dal nero sia dall'accent, allora un quarto stato esiste ed è raggiungibile come
   coppia di bit sui due piani già presenti;
3. se `0x28` su questo modulo scriva davvero qualcosa, a dispetto della tabella comandi.
Le combinazioni per pixel che due piani a 1 bit possono esprimere sono esattamente 4: rispondere a
1 e 2 chiude lo spazio documentato, e 3 chiude quello non documentato.

- L'idempotency di `showImage` sul piano `0x28` è guardata da `showImagePageHint() == 0`, non
  da `isYellowPreserved()`: quel flag protegge anche un pre-write out-of-band su un'altra
  area di `0x28` (la barra temp-range di `Weather.h`) e come guardia sopprimerebbe il giallo
  dell'immagine. `writeImageYellow` imposta la propria finestra RAM, quindi aree disgiunte
  convivono.
- **Niente read-back**: sul FPC 24 pin non esiste una linea SDO (pin 12 = SDI e basta, confermato
  sullo schematico Waveshare V3 in `docs/E-Paper_ESP32_Driver_Board_V3.pdf`). Inutilizzabili tutti i
  comandi di lettura del controller — `0x1B` temperatura, `0x27` read RAM, `0x2E` User ID da OTP,
  `0x2F` status — elencati in [[ssd1677_command_set]]. La diagnostica è solo visiva. La sonda li
  tenta comunque e usa `0x2F`, che ha POR noto `0x01` con chip ID `01`, come prova di validità del
  percorso: se quello non torna, dichiara inattendibili anche gli altri due invece di stampare
  numeri senza senso. Ricablare la linea avrebbe senso solo per `0x2E` e per i byte G..J di `0x37`
  (module ID / waveform version), che identificherebbero il modulo senza ambiguità.
- **Sonda**: `A:\epd\GxEPD2_SOLUM_ESL\examples\panel_diagnostic\panel_diagnostic.ino`, cioè
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

  | driver | geometria | `0x22` refresh | init |
  |---|---|---|---|
  | `gdem3c/GDEM133Z91` | 960×680 | `0xF7` | `0x0C{AE C7 C3 C0 80}`, `0x01`, `0x3C{01}`, `0x18{80}` |
  | `gdey3c/GDEY116Z91` | 960×640 | `0xF7` | solo SWRESET + `0x3C{01}` |
  | `epd3c/750c_Z90` | 880×528 | `0xC7` | `0x0C{… 40}`, MUX 527, `0x3C`, `0x18`, + `0x22{B1}`/`0x20` |

  GDEM133Z91 ha **quattro comandi su cinque byte-identici** al firmware SOLUM (`0x0C` col quinto
  byte `0x80`, `0x3C{01}`, `0x18{80}`, `0x22{F7}`); differiva solo il conteggio gate di `0x01`.
  GDEY116Z91 lascia tutto il front-end analogico al POR e andrebbe riscritto; 750c_Z90 ha il soft
  start col quinto byte `0x40`, MUX 527 e refresh `0xC7`, che **non carica la temperatura** mentre
  la waveform di questo pannello è compensata. La geometria non discrimina: `HEIGHT` la definisce il
  driver custom a 672, conta solo `WIDTH = 960`, cioè righe da 120 byte.

  Da qui viene anche il commento sbagliato `// Set MUX as 527`: è copiato da 750c_Z90, dove 527 è
  giusto (`0x020F` = 527 -> 528 gate = la sua `HEIGHT`).

- **SSD2677 non è un'alternativa**: è un altro protocollo, e la scelta la determina il silicio, non
  la preferenza. Comandi scritti dai due driver: SSD1677 (`GDEM133Z91`) usa
  `01 0C 10 11 12 18 20 22 3C 44 45 4E 4F`, SSD2677 (`epd4c/GDEY116F51`) usa
  `00 01 02 03 04 06 07 10 12 30 41 50 60 61 62 65 83 E0 E3 E7 E9`. L'intersezione è di tre opcode
  e tutti e tre significano cose opposte: `0x01` MUX contro PWRR, **`0x10` deep sleep contro DTM1**
  (il trasferimento immagine), **`0x12` SWRESET contro display refresh**. Un driver derivato da
  SSD2677 su questo pannello manderebbe `0x10` per iniziare l'immagine, cioè lo addormenterebbe, e
  `0x12` per il refresh, cioè lo resetterebbe. SSD2677 (con template `GxEPD2_4C`, base
  `GDEY116F51`, 2bpp packed su `0x10`) è la strada giusta solo per un pannello davvero a 4 colori:
  i SOLUM BWY fino alla 11.6", non questa 9.7". Vale per il branch `Solum_12_2`: finchè quel
  pannello risponde a `0x24`/`0x26` resta correttamente su base SSD1677.

- **Refresh differenziale, la pista per ~600 ms invece di 22 s.** Il fratello monocromatico dello
  stesso silicio, `gdem/GxEPD2_1330_GDEM133T91` (960×680, SSD1677), dichiara
  `partial_refresh_time = 600` e lo ottiene scrivendo il **frame precedente nella RAM `0x26`** e
  quello corrente nella `0x24` (`_writeImage(0x26, ...) // set previous`), poi lanciando
  `_Update_Part()` = `0x22 = 0xFC`. È codice upstream che dimostra perchè
  `hasFastPartialUpdate = false` qui non è prudenza ma struttura: su questo pannello `0x26` è
  l'accent, e i due usi della stessa RAM si escludono.
  Ne resta una pista, **non verificata**: su un frame senza accent si potrebbe riusare `0x26` come
  frame precedente e lanciare `0xFC`. Il punto aperto è se la OTP di questo modulo contenga la
  waveform di Mode 2 — la misura la fa la sonda. Vedi [[ssd1677_command_set]] per la decodifica per
  bit di `0x22` che produce `0xFC` e `0xF4`.

- **Pittfall `GxEPD_YELLOW`**: il template upstream `GxEPD2_3C` mappa `GxEPD_YELLOW` sul piano red
  (`0x26`) — `GxEPD2_3C.h:196`. `drawPixel(x, y, GxEPD_YELLOW)` non scrive sul piano `0x28`. Le due
  vie previste dal driver per scrivere `0x28` sono `GxEPDImage::showImage` con descrittore
  BWRY oppure `writeImageYellow()` + `preserveYellow(true)` PRIMA di `firstPage()`. Se `0x28` si
  rivelasse inerte queste due strade non servono a niente, ma la mappatura di `GxEPD_YELLOW` sul
  piano red resta comunque il punto da conoscere: su un film BWY sarebbe esattamente quella la via
  giusta. Vedi il blocco sul 4° colore sopra.

- **Page-tracking `_show_image_page_hint`**: contatore parallelo a `_current_page` (privato in
  `GxEPD2_3C`), avanzato in `writeImage(black, color, ...)` (che il template chiama da `nextPage()`
  in full-window), azzerato in `setPaged()` e `_Update_Full()`. Regge il row-skip di `showImage`
  senza modificare GxEPD2.

- **API pubblica usata dallo sketch**: solo `GxEPDImage::showImage(display, *desc)` dentro un loop
  `firstPage/nextPage`. Il resto (API single-channel, `preserveYellow`) è compositing avanzato.

- **`showImage` hardcoda `pgm=true`** (`src/GxEPD2_SOLUM_097c_960x672.h:185` e `:221-223`): vale per
  immagini pre-compilate nello sketch, non per buffer scaricati via HTTP.

- **Variante 122c**: il pannello SOLUM 12.2" (960w × 768h landscape post-rotation) ha il proprio
  driver `GxEPD2_SOLUM_122c_960x768`, che vive nel branch `Solum_12_2` e non sul filesystem di
  `main`, con la stessa architettura SSD1677. Lo sketch sceglie il driver via `Layout::Panel`
  (typedef in `Layout_097c.h` / `Layout_122c.h`) — vedi `layout_separation.md`.

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
- dirty flag `_color_dirty` / `_yellow_dirty`: il cleanup accent viene saltato quando non serve
  (tipico sulle catene di frame B/N). Costa ~15 ms su `0x26`, che si pulisce col pattern hardware,
  e ~65 ms su `0x28`, che passa dal bus.
- `hibernate()` idempotente, azzera i dirty flag e `_preserve_yellow`: al wake il SWRESET riporta
  comunque la RAM del controller a stato noto.
- cleanup accent simmetrico tra `writeImage` e `writeImagePart` BW.
- `_preserve_yellow` azzerato in `_Update_Full()`, cioè a ogni refresh.
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

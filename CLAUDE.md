# ePaper weather dashboard


## Hardware utilizzato
1. Sistema operativo: Windows. Nome macchina: PC-030.
Utilizzo previsto: scrittura del codice; test, build ed esecuzione dell'app solo come verifica, non come ambiente di sviluppo.

**Vincoli, tutti inderogabili:**
- L'unica posizione in cui è consentito installare qualsiasi componente (software, dipendenze, runtime, SDK, toolchain, database, servizi) è `A:\tmp\sandbox`. Un'operazione che richieda un'installazione altrove è **non consentita**: va fermata e chiesto all'utente.
- Ciò che si installa lì dev'essere portable/standalone, non deve installare nè registrare componenti in Windows, non deve toccare il registro di sistema nè altre configurazioni permanenti.
- I file prodotti da test e compilazioni devono restare in `A:\app` o `A:\tmp\`.
- Ciò che sta in `A:\tmp\` non va eliminato, a meno che sia del tutto temporaneo.

2. Sistema operativo: macOS.
Utilizzo previsto: dispositivo principale di sviluppo.
Su questo dispositivo è consentito:
installare dipendenze;
installare toolchain e runtime;
compilare l’applicazione;
eseguire l’applicazione in locale;
effettuare debug;
eseguire test di sviluppo.


## Il dispositivo: cosa è il progetto

Firmware Arduino/ESP32 (`ePaper-weather-dashboard.ino`) che disegna una dashboard su un pannello
e-paper a colori: meteo (OpenWeather One Call 3.0), calendari Outlook e Google, posta Gmail,
sensore ambientale BME680 via BSEC, e un banner con la programmazione cinema servita dalla webapp.
Il dispositivo sta in light sleep, si sveglia a cadenza fissa (`DISPLAY_REFRESH_MIN`), accende il
WiFi solo dentro una finestra oraria e ridisegna il pannello.

I `.h` e il `.ino` nella root di `A:\epd` sono **firmware in produzione**: non modificarli se non
richiesto esplicitamente.

Memoria persistente del progetto: `A:\epd\.claude\memory\` (indice `MEMORY.md`), **non** sotto
`C:\Users\...\.claude\projects\`. Le memorie contengono il dettaglio di tutto ciò che qui è
riassunto: leggere l'indice a inizio lavoro.


## Board di bring-up: Waveshare E-Paper ESP32 Driver Board V3

Modulo **ESP32-WROOM-32E**, quindi **senza PSRAM**: GPIO16/17 restano liberi (su un WROVER non lo
sarebbero). Schematico in `A:\epd\GxEPD2_SOLUM_ESL\docs\E-Paper_ESP32_Driver_Board_V3.pdf`; il
manuale utente non contiene pinout ed è inutile allo scopo.

| Segnale | GPIO | | Segnale | GPIO |
|---|---|---|---|---|
| BUSY | 25 | | CLK (SCK) | 13 |
| RST | 26 | | DIN (MOSI) | 14 |
| D/C | 27 | | MISO (dummy) | 12 |
| CS | 15 | | | |

Punti non ovvi, tutti verificati:

- **La board scambia SCK e MOSI rispetto al default HSPI**: il remap non è una scelta di stile ma
  un obbligo — `hspi.begin(13, 12, 14, 15)` + `display.epd2.selectSPI(hspi, ...)`. Da qui il
  `#define USE_HSPI_FOR_EPD` nel `.ino`.
- **MISO = GPIO12 è un dummy**: sul FPC 24 pin del connettore interno **non esiste SDO**, il pin
  dati è solo SDI. Conseguenza: tutti i comandi di lettura del controller (`0x1B` temperatura,
  `0x27` read RAM, `0x2E` User ID da OTP, `0x2F` status) non tornano niente, e ogni diagnostica che
  ci conti sopra sta misurando rumore. GPIO12 è anche un pin di strapping (MTDI): se un giorno ci
  si collega un SDO vero, va garantito che non sia alto al reset, altrimenti l'ESP32 non parte.
- **GPIO33 non è portato fuori** su questa board (le net dello schematico saltano dal 32 al 34): il
  cablaggio del secondo controller del 12.2" è documentato con `CS_S = GPIO33` ed è **da sostituire
  con GPIO32** (output-capable, sull'header, non usato da nient'altro).
- GPIO liberi per un secondo controller: **32** (CS consigliato), 4/21/22 in alternativa; **34/35
  sono input-only**, quindi vanno bene per un BUSY e non per un CS. Da evitare 0/2/12 (strapping);
  5/18/19/23 sono i pin VSPI, liberi ma da tenere di riserva per altre periferiche SPI.
- **Nessun socket microSD** sulla board: il commento "use HSPI for EPD (and VSPI for SD)" degli
  esempi upstream descrive un pattern, non questo hardware.
- `hspi.begin()` e `selectSPI()` sono codice **board-level**: stanno nel `.ino`, non nei
  `Layout_*.h`. Unica eccezione: il driver 12.2" apre il bus da sè in `init()`, quindi
  `Layout_122c.h` gli passa anche sck/miso/mosi dentro `makePanel()`.


## Pannelli e-paper usati

Sono **etichette elettroniche SOLUM (ESL) recuperate** e ripilotate: nessun modulo commerciale,
nessun datasheet del controller fornito dal produttore. Convenzione di scrittura delle dimensioni:
`NwxMh` = N px in larghezza (X) × M px in altezza (Y), post-rotazione.

I codici modello si leggono così (§3.6 del datasheet PRO):

```
EL <taglia> <generazione> <colore scocca> <colore display> <tipo tag>
   097       F5            C               4                C
```

`R` nel campo colore = BWR, `4` = RED, YELLOW (BWRY). **Non è un dato per unità**: nel datasheet
ogni taglia della linea PRO è elencata con `4`, e la generazione F6 ha pannelli con la stessa cifra
e serigrafie sul vetro diverse (9.7" `BWRY Normal`, 11.6" e 12.2" `BWR normal`). Il campo distingue
la linea — PRO nominalmente a 4 colori, Core `R` a 3 — mentre il film montato lo dice solo
l'etichetta serigrafata sul vetro.

**SOLUM 9.7"** (`EL097R2CRN`, generazione R2) — 672×960 nativi portrait, usato come
**960w × 672h** landscape. Controller **SSD1677**, un solo COF e una sola coda FFC (24 pin).
**Tre colori: bianco, nero e rosso, e il quarto non esiste.** Il codice modello dell'unità, letto
sul case, ha campo colore `R` = BWR (pratica FCC `2AFWN-EL097R2CRN`, KC `R-R-SLU-EL097R2CRN`); non
è quindi il donor `EL097F5C4C` della linea PRO con la cifra `4`, e il datasheet SOLUM che dichiara
BWRY per la taglia 9.7" riguarda quella linea, non questa unità.

La misura concorda, ed è definitiva: `examples/097c/panel_diagnostic` ha stampato tutte e quattro
le combinazioni dei due piani sotto la waveform di produzione, e la Table 6-4 dell'SSD1677 si legge
sul vetro così — `(0x26, 0x24)` = `(0,0)` nero LUT0, `(0,1)` bianco LUT1, `(1,0)` e `(1,1)`
**entrambe rosse**, cioè LUT3 aliasata su LUT2 come la tabella dichiara. Con due bit per pixel le
combinazioni sono esaurite. Anche `0x28` ha risposto: alla scrittura alza il BUSY per ~10 s e non
dipinge, quindi è VCOM Sense e non un terzo piano. Conseguenza per il codice: sotto un pixel di
accent il valore del piano BW è **indifferente**, e scrivere l'accent non richiede di mascherare
`0x24`.

Resta un solo code point mai provato, e non è una coppia di bit: è la **tensione**. Le LUT del
silicio sono `LUT0..LUT4` e i livelli di sorgente quattro (VSS, VSH1, VSH2, VSL), quindi che un
SSD16xx possa fare 4 colori non è escluso dall'architettura; la Table 6-4 dice che LUT2 e LUT3 sono
aliasate *dalla waveform*, non che il film abbia tre pigmenti. La sonda carica quindi via `0x32` una
waveform in cui LUT2 va a VSH1 e LUT3 a VSH2 — tensioni dell'OTP, perchè quelle stanno ai byte
105..109 della LUT e `0x32` scrive solo 0..104 — e stampa le due bande adiacenti.

**SOLUM Newton PRO 12.2"** (`EL122H6W4A`) — 768×960 nativi, usato come **960w × 768h**. Bianco,
nero e rosso secondo il vetro (`Newton PRO 12.2" BWR normal`), ma il codice modello ha campo colore
`4`: a differenza del 9.7" **qui il quarto colore è ancora da determinare**, perchè il bring-up è
fermo alla seconda coda muta e non c'è modo di misurarlo. Per questo il driver 12.2" dichiara le
primitive del terzo piano, ancora senza corpo, mentre il 9.7" non le ha affatto. È il caso
interessante: **due controller SSD16xx**, uno per coda FFC, ciascuno da **960 × 384** con lo split
sull'asse corto — cioè, in coordinate driver, due **bande orizzontali** (righe 0..383 e 384..767).

Perchè siano due, e divisi per righe, è un conto e non un'osservazione: l'SSD1677 ha 960 source e
**680 gate**, e le tre taglie grandi hanno 672 (9.7"), 640 (11.6") e **768** gate nativi. Solo il
12.2" sfora, quindi due controller sono obbligati e lo split **deve** stare sull'asse gate — su
quello source un chip solo basterebbe. Misurato coerentemente: con una sola coda cablata si stampa
un rettangolo 960×384. Il **BUSY è attivo alto** (non basso come vorrebbe un UC8179). Le due code
escono da bordi opposti, quindi le due COF sono ruotate di 180° l'una rispetto all'altra e il
driver specchia la banda slave **nel data path** (l'SSD1677 non ha reverse scan hardware: il bit TB
di `0x01` è Reserved).

Stato noto: **la seconda coda è muta**, e la spiegazione che copre tutti i sintomi è che sia la
coda dello **slave di una coppia in cascade**. In cascade lo slave ha oscillatore e booster
disabilitati e riceve clock e tensioni dal master, quindi su un breakout con solo SPI e 3.3 V è
muto per costruzione; si indirizza sommando `0x80` all'opcode su un **solo** chip select, con
`0x21` B[4] = 1 sul master. I due pin che servono stanno nel pin table dell'SSD1677, solo dati come
riservati: `M/S#` *"reserved, connect to VDDIO"* e `CL` dichiarato **I/O** ma *"left open"*. Le
vecchie ipotesi — ordine dei pin ribaltato, rail di boost non portati, BUSY/RST fuori posizione —
restano possibili ma spiegano solo il silenzio, non l'assenza sul tag di fabbrica di un secondo CS
e di un secondo boost.

Due conseguenze che si pagano se ignorate. La prima: il **read-back non esiste** su questi FPC
(vedi sopra). La seconda: `hasFastPartialUpdate = false` è un fatto misurato. Il refresh
differenziale dell'SSD1677 usa la RAM `0x26` come "frame precedente", ma su questi pannelli `0x26`
è il piano accent, quindi i due usi si escludono; e la sonda ha misurato che **in OTP c'è una sola
waveform**, con `0xFC`, `0xFF`, `0xCF` e `0xC7` tutti a 24,6-24,8 s e uno scarto di 2 ms fra piani
identici e piani opposti — il controller non li confronta.

Quello che quelle misure **non** escludono sono tre strade, e la sonda le prova tutte e tre.

La prima è il partial con una **LUT scritta dall'MCU**: le passate sopra hanno il bit 4 di `0x22`
attivo, cioè ricaricano la waveform dall'OTP, oppure girano su una LUT che dall'OTP era già stata
caricata. La sequenza mancante è quella del GDEH116T91, stesso SSD1677 con gli stessi 960 source,
che così ottiene 700 ms: `0x3C = 0xC0` (border HiZ), `0x32` con 105 byte di waveform breve, e
`0x22 = 0xCC` col bit 4 **spento**, in modo che l'OTP non sovrascriva la LUT custom.

La seconda è la più promettente, e non richiede di inventare waveform: il §6.9 del datasheet dà
l'OTP per capace di **34 set di waveform**, WS0..WS33, uno per range di temperatura TR0..TR33, che
il silicio seleziona in base alla temperatura letta — scorrendo i range da TR0 a TR33 e caricando
l'ultimo che corrisponde. Ogni misura fatta finora ha girato col sensore interno a temperatura
ambiente, quindi ha esercitato **un solo** set. Forzando la temperatura con `0x18 = 0x48` (sensore
esterno) e `0x1A` (12 bit, valore = gradi × 16: 25 °C = `0x190`) si chiedono gli altri, e sono
waveform **di fabbrica** tarate su questo film. Sui pannelli a inchiostro quelle calde sono più
corte, perchè il pigmento migra prima.

La terza è geometrica: il **MUX** di `0x01` programma quante gate line il driver scandisce, che è
diverso dalla finestra RAM — quella dice solo dove finiscono i byte, e le passate d'area
scandivano comunque tutte e 672 le gate. Se il costo del refresh è per gate line invece che per
frame, un MUX ridotto accorcia il refresh in proporzione e dà un partial per bande che partono
dalla prima gate.

Finchè nessuno dei tre test passa, `hasFastPartialUpdate` resta `false`. Il prezzo della prima
strada, se passasse, è già noto: in Mode 2 la RAM `0x26` fa da frame precedente, quindi un frame
aggiornato in partial è senza accent, e la scelta è per frame e non per pixel.

Resta fuori portata una sola leva, per scelta: alzare le tensioni di sorgente con `0x04`
accorcerebbe la migrazione del pigmento, ma è l'unica che può danneggiare il film in modo
permanente, e senza la waveform del produttore non c'è modo di sapere quanto margine ci sia.

Il refresh **d'area** invece funziona già ed è misurato: la finestra RAM di `0x44`/`0x45` confina
davvero la zona ridipinta, in Y e in X, con bordi verticali netti e la fascia di trappola fuori
finestra rimasta intatta. Non serve a niente per la velocità, perchè la durata non dipende
dall'altezza della finestra — 168, 48 e 24 righe misurano tutte 24,65 s — quindi restringere la
finestra fa guadagnare solo sul push SPI, che è lo 0,6% del ciclo.


## Librerie

**`A:\tmp\GxEPD2-master`** — clone **gitignorato** di ZinggJM/GxEPD2 al tag 1.6.9 (`de82887`),
identico a upstream a meno dei CRLF. È una **copia di sola lettura**, tenuta lì per consultare i
sorgenti (i template `GxEPD2_3C`/`GxEPD2_BW`, i driver SSD1677 di riferimento, gli esempi
board-specific). Non modificarla e non trattarla come codice del progetto.

Sta **fuori** dalla cartella del progetto perchè arduino-cli copia l'intero albero dello sketch
dentro la build dir a ogni build, e quei 47,8 MB venivano duplicati ogni volta. Una junction dentro
`A:\epd` non risolve: **arduino-cli la segue**, misurato, e la copia torna identica.

Per consultare i sorgenti c'è quindi **`epd.code-workspace`**, workspace multi-root che monta il
progetto come prima radice e il clone come seconda, in sola lettura. Due vincoli, scritti anche
dentro al file:

- **la prima radice deve restare il progetto**: Claude Code identifica il progetto dalla working
  directory, che in un workspace multi-root è la prima cartella. Con `"."` al primo posto la chiave
  resta `A:\epd`, quindi le chat in `~\.claude\projects\a--epd` e le memorie in `.claude\memory`
  si ritrovano esattamente come aprendo la cartella;
- **i percorsi sono relativi al file di workspace**, quindi funziona anche su macOS purchè il clone
  stia in una cartella `tmp` accanto al repo. Se sul Mac sta altrove, VS Code segna la radice come
  non disponibile senza rompere niente e basta correggere quella riga.

Alla toolchain **non** arriva il clone intero: `A:\tmp\arduino\user\libraries\GxEPD2` è una
**copia potata**, che rigenera `A:\tmp\arduino\rigenera-libreria-gxepd2.ps1` e va rifatta dopo
ogni aggiornamento del clone. Tiene `library.properties`, tutti gli header tranne `src\bitmaps\`
(42,8 MB dei 43,6 di header, fuori dal grafo di include del firmware) e il solo
`src\GxEPD2_EPD.cpp`. Il motivo è il tempo di build: Arduino compila **tutti** i sorgenti di una
libreria, e dei 104 di GxEPD2 questo progetto ne usa uno — il pannello lo pilota il driver custom
del submodule e dalla libreria arrivano solo header. Gli altri 103 venivano preprocessati per il
rilevamento delle dipendenze, compilati e poi scartati dal linker: **misurati 696 s di build contro
120 s dopo la potatura**. Gli header dei driver restano: `GxEPD2_3C.h` li include sotto
`__has_include`, quindi tenerli lascia il grafo identico al clone, mentre toglierli cambierebbe
cosa vede il compilatore.

**`A:\epd\GxEPD2_SOLUM_ESL`** — submodule (branch `main`, GPL-3.0 obbligata: i driver sono copie
modificate di sorgenti GxEPD2). È una **libreria Arduino a sè stante**, header-only,
`architectures=esp32`, `depends=GxEPD2 (>=1.6.9),Adafruit GFX Library`, che estende GxEPD2 con i
driver dei pannelli SOLUM. Le modifiche ai driver si committano nel **suo** repo e nel padre si
aggiorna il puntatore del submodule; `A:\epd` va clonato con `--recursive`.

```
src/GxEPD2_SOLUM.h                 ombrello di selezione, unico header che gli sketch includono
src/GxEPD2_SOLUM_Pins.h            struct di pinout uniforme fra i driver
src/GxEPDImage.h                   namespace GxEPDImage + template showImage(), condiviso
src/GxEPD2_SOLUM_097c_960x672.h    driver 9.7"  SSD1677
src/GxEPD2_SOLUM_122c_960x768.h    driver 12.2" SSD16xx, dual controller
examples/097c/panel_diagnostic/    sonda del 9.7", a SPI diretta
examples/12_2c/dual_panel_finder/  sonda del 12.2": probe del silicio + verifica del driver
docs/                              datasheet, foto FCC, sorgenti OEPL, cablaggi
```

Meccanismi da conoscere prima di toccarla:

- **Ombrello `GxEPD2_SOLUM.h`**: uno sketch non nomina mai la classe concreta. Si definisce
  `SOLUM_PANEL_097C` **oppure** `SOLUM_PANEL_122C` *prima* dell'include (zero o due danno `#error`)
  e si usa la macro `GxEPD2_SOLUM_DRIVER_CLASS`. È l'idioma di
  `GxEPD2_display_selection_new_style.h` upstream, portato dentro la libreria invece che negli
  esempi.
- **`GxEPD2_SOLUM_Pins`**: struct di pinout uniforme (`cs, dc, rst, busy, cs2, busy2, sck, miso,
  mosi`, `-1` = assente) che assorbe le arità diverse dei costruttori — il 12.2" ha due CS e due
  BUSY. Ogni pin va guardato con `>= 0` prima di `pinMode()`/`digitalWrite()`: `-1` è un valore
  legale della struct e non deve arrivare all'API Arduino.
- **Contratto di `GxEPDImage.h`**: il template `showImage()` è unico per la libreria e pretende
  **due** metodi pubblici da ogni driver — `setPaged()` e `showImagePageHint()`. Compone i due
  piani che `GxEPD2_3C` gestisce, black e red, e non tocca un eventuale terzo piano: di un
  descrittore `FORMAT_BWRY_1BPP` rende `data0` e `data1` e ignora `data2`. Quindi nessun driver ha
  no-op imposti dal contratto, e lo stesso firmware compila contro entrambi i pannelli senza rami
  condizionali. Le primitive del terzo piano (`writeImageYellow`, `preserveYellow`,
  `isYellowPreserved`) sono **API opzionali del singolo driver**, che il chiamante usa out-of-band
  prima di `firstPage()`: il 12.2" le dichiara senza corpo perchè la sua domanda è aperta, il 9.7"
  non le ha.
- **Bus SPI sempre da `_pSPIx` / `_spi_settings` della base `GxEPD2_EPD`**, mai dall'oggetto `SPI`
  globale: un default proprio si imposta chiamando `selectSPI()` nel costruttore, non cablando le
  `SPISettings` nelle primitive. Altrimenti un `selectSPI()` dello sketch è silenziosamente inerte.
- **`GxEPD2_SOLUM_ESL` non va installata come libreria Arduino**: lo sketch la include per path
  relativo ed è header-only, installarla creerebbe due path per lo stesso header. I suoi examples
  si compilano a parte passando `--library` solo per quella build.

**`A:\epd\webapp`** — submodule (`cinema-programmation-feed`): scraper Python + renderer che serve
al firmware l'immagine della programmazione cinema già ditherata, in formato raw packed
header-less. Ha una propria memoria in `webapp\.claude\memory\`.


## Architettura del firmware: separazione layout/logica

Un solo codice applicativo, due pannelli. La separazione è tutta qui:

- **`Layout.h`** è un dispatcher: include `Layout_097c.h` o `Layout_122c.h` secondo
  `#define DISPLAY_VARIANT_097C` / `DISPLAY_VARIANT_122C` settato in testa al `.ino`, ed emette
  `#error` se nessuno o entrambi sono definiti.
- **`Layout_097c.h` / `Layout_122c.h`** definiscono lo stesso namespace `Layout` con simboli
  identici: `Panel` (alias della macro del driver), `PAGE_HEIGHT`, `makePanel()`, pin, font e ~50
  costanti di coordinate. Sono gli **unici** file che includono la libreria dei driver e gli unici
  che contengono valori pixel.
- I moduli (`Weather.h`, `Calendar.h`, `Mail.h`, `Graphics.h`, `icons.h`, `Indoor.h`, `Ota.h` e il
  `.ino`) referenziano tutto via `Layout::*`.

Vincoli da non violare:

- `Weather.h`, `Calendar.h`, `Graphics.h` dichiarano
  `extern GxEPD2_3C<Layout::Panel, Layout::PAGE_HEIGHT> display;`: **devono usare
  `Layout::PAGE_HEIGHT`, non ricalcolare `Panel::HEIGHT / 8`**. Sono argomenti template, quindi un
  valore diverso da quello del `.ino` produce un tipo diverso e il **link fallisce**.
- `PAGE_HEIGHT = Panel::HEIGHT / 8` è deliberato: la `SOLUM_MAX_HEIGHT()` della libreria
  spenderebbe ~65 KB di buffer su un pannello largo 960 px, mentre qui la RAM serve anche al resto
  del firmware (~20 KB, otto page).
- Modifiche di coordinate/font: **solo** nei `Layout_*.h`, mai hardcoded nei moduli. Le baseline
  del banner sono espresse come `BANNER_Y + offset`, così lo scaling 097c→122c si propaga da sè.
- Nei `Layout_*.h` **non** stanno le cadenze di fetch (`*_FETCH_MIN`, `OTA_WINDOW_MIN`,
  `WIFI_ACTIVE_HOUR_*`, `CINEMA_DAILY_FETCH_HOUR`): sono timing/orchestrazione e vivono nel `.ino`.
- **Per aggiungere un terzo pannello**: driver nella libreria (rispettando il contratto) + nuovo
  `Layout_<nome>.h` con gli stessi simboli + ramo `#elif` nel dispatcher + `#define
  DISPLAY_VARIANT_<NOME>` nel `.ino`. Nè i moduli nè la riga di costruzione del display si toccano.


## Build su questa macchina

Comando unico: **`A:\tmp\arduino\build.ps1`** (`-Clean`, `-Dettagli`, `-Board <fqbn>`,
`-Partizioni <schema>`). FQBN `esp32:esp32:esp32`, `PartitionScheme=huge_app`. Gli oggetti
intermedi stanno in `A:\tmp\arduino-build\<sketch>` e nessun binario viene esportato: **da qui non
si flasha**, è solo compilazione di verifica e l'upload avviene da un altro PC.

- `arduino-cli` 1.5.2 in `A:\tmp\arduino`, con config **non** nel percorso di default: passare
  sempre `--config-file A:/tmp/arduino/arduino-cli.yaml`.
- Core esp32 3.3.11 + toolchain + cache in `C:\xz\arduino` — deroga autorizzata al vincolo "tutto
  su `A:\tmp`", perchè il core completo chiede ~3,4 GB. È installato con un **index filtrato**
  (`A:\tmp\arduino-setup\package_esp32_min_index.json`) che tiene solo i target usati:
  reinstallando o cambiando versione va rigenerato con lo stesso filtro.
- Una **junction, non una copia** (Arduino pretende che la cartella si chiami come il `.ino`):
  `A:\tmp\arduino\sketch\ePaper-weather-dashboard` → `A:\epd`. La libreria `GxEPD2` invece è una
  copia potata, vedi la sezione Librerie.
- **Le build non stanno nella codebase**: senza `--build-path` arduino-cli mette gli intermedi in
  `C:\xz\arduino\cache\sketches\<hash>` e, quando si esportano i binari, li lascia in una
  cartella `build\` dentro lo sketch. `build.ps1` passa `--build-path`
  `A:\tmp\arduino-build\ePaper-weather-dashboard`; per gli examples del submodule si passa a mano
  `--build-path A:\tmp\arduino-build\<nome-esempio>`. Quella cartella serve alla compilazione
  incrementale e non va svuotata fra una build e l'altra.
- **`Env.h` è gitignored ed è obbligatorio per compilare**: `build.ps1` copia `Env_template.h` se
  manca, quindi il firmware compila ma non funziona in campo finchè non ci sono credenziali vere.
- Gli **examples del submodule non li compila questo build**: Arduino concatena i `.ino` solo dalla
  root dello sketch.

## Convenzioni di codice
- Lingua commenti/doc: **italiano**
- Per i commenti multi riga usa lo stile javadoc /** */ se il linguaggio lo supporta
- Se il commento è singola riga usa //
- Naming TypeScript: camelCase per variabili/funzioni, PascalCase per tipi/interfacce
- Naming DB: snake_case per tabelle e colonne (Prisma mappa con @map)
- Nessun `console.log` in produzione
- Non tradurre dall'inglese i nomi delle funzioni o variabili
- Non usare @since o @modified o comunque riferimenti storici
- Quando sviluppi o modifichi codice, privilegia sempre soluzioni native o già disponibili nelle librerie/framework in uso:
Evita di scrivere codice custom quando esistono componenti, API, utility, hook, helper o pattern ufficiali che risolvono già il problema in modo adeguato;
L’obiettivo è ridurre complessità, manutenzione, bug e duplicazione, mantenendo il codice il più idiomatico possibile rispetto allo stack utilizzato.

## Regole comportamentali per Claude
- Non committare mai automaticamente
- ponimi altre domande se ci sono problemi o hai dubbi
- Non refactoring non richiesto: aggiusta solo quello che è stato chiesto
- Mostra sempre i percorsi file completi nei riferimenti al codice
- Per modifiche che toccano più di 3 file, presenta prima un piano
- Aggiungi sempre una breve descrizione sopra i metodi e le funzioni crei e scrivi anche i riferimenti alle classi che li usano se sono classi esterne al file corrente
- Quando modifichi delle funzioni o metodi già esistenti adegua il commento in modo che rifletta lo stato attuale del codice


## Memorie
- Le memorie devono riflettere lo stato finale del codice, non stati di avanzamento nè modifiche. Descrivi cosa il codice fa nel presente e non come ci si è arrivati
evita quindi "fasi non fatte/da fare", date di modifica, "aggiornato", "rimosso" e simili formulazioni storiche o di progresso.
# ePaper weather dashboard


## Hardware utilizzato

**1. Windows (PC-030).** Uso previsto: scrittura del codice; test, build ed esecuzione solo come verifica, non come ambiente di sviluppo. Vincoli, tutti inderogabili:
- L'unica posizione in cui è consentito installare qualsiasi componente (software, dipendenze, runtime, SDK, toolchain, database, servizi) è `A:\tmp\sandbox`. Un'operazione che richieda un'installazione altrove è **non consentita**: va fermata e chiesto all'utente.
- Ciò che si installa lì dev'essere portable/standalone: non deve installare nè registrare componenti in Windows, nè toccare il registro di sistema o altre configurazioni permanenti.
- I file prodotti da test e compilazioni restano in `A:\app` o `A:\tmp\`.
- Ciò che sta in `A:\tmp\` non va eliminato, a meno che sia del tutto temporaneo.

**2. macOS.** Dispositivo principale di sviluppo: sono consentiti installare dipendenze, toolchain e runtime, compilare, eseguire in locale, fare debug ed eseguire test di sviluppo.


## Il dispositivo

Firmware Arduino/ESP32 (`ePaper-weather-dashboard.ino`) che disegna una dashboard su un pannello e-paper a colori: meteo (OpenWeather One Call 3.0), calendari Outlook e Google, posta Gmail, sensore ambientale BME680 via BSEC (pubblicato anche su Tuya via MQTT) e un banner con la programmazione cinema servita dalla webapp.

Il dispositivo sta in light sleep, accende il WiFi solo dentro una finestra oraria e ridisegna il pannello quando lo scheduler ha dati nuovi. Tempi e cadenze: `Timings.h` (default, pavimenti, override in NVS), `Scheduler.h` (tabella dei task nel `.ino`, sleep dinamico clampato a [30, 300] s). Ogni frame è un **refresh pieno** da ~24 s: il partial del driver esiste ma il firmware non lo usa, perchè renderebbe un frame senza rosso; la scelta è esplicita nel `.ino` con `DISPLAY_PARTIAL_REFRESH` più uno `static_assert`. Prima del light sleep il pannello va in **deep sleep** con `display.hibernate()`, che gli fa perdere la RAM: al risveglio il driver rifà init e ripulisce i due piani da sè.

I `.h` e il `.ino` nella root di `A:\epd` sono **firmware in produzione**: non modificarli se non richiesto esplicitamente. La mappa dei produttori di dati del display (task, adattatori, disegnatori) è nel `README.md`, sezione "Moduli applicativi".

Memoria persistente: `A:\epd\.claude\memory\` (indice `MEMORY.md`), **non** sotto `C:\Users\...\.claude\projects\`. Contiene il dettaglio di tutto ciò che qui è riassunto: leggere l'indice a inizio lavoro.


## Board di bring-up: Waveshare E-Paper ESP32 Driver Board V3

Modulo **ESP32-WROOM-32E**, quindi **senza PSRAM**. Schematico in `A:\epd\GxEPD2_SOLUM_ESL\docs\E-Paper_ESP32_Driver_Board_V3.pdf`; il manuale utente non contiene pinout.

| Segnale | GPIO | | Segnale | GPIO |
|---|---|---|---|---|
| BUSY | 25 | | CLK (SCK) | 13 |
| RST | 26 | | DIN (MOSI) | 14 |
| D/C | 27 | | MISO (dummy) | 12 |
| CS | 15 | | | |

Punti non ovvi, tutti verificati (dettaglio in `waveshare_esp32_driver_board.md`):

- **La board scambia SCK e MOSI rispetto al default HSPI**: il remap è un obbligo, `hspi.begin(13, 12, 14, 15)` più `display.epd2.selectSPI(hspi, ...)`. Non esiste una macro che lo abiliti: sta tutto in quelle due chiamate del `.ino`, che sono codice **board-level** e non vanno nei `Layout_*.h`. Unica eccezione: il driver 12.2" apre il bus da sè in `init()`, quindi `Layout_122c.h` gli passa sck/miso/mosi dentro `makePanel()`.
- **MISO = GPIO12 è un dummy**: sul FPC a 24 pin non esiste SDO. Ogni comando di lettura del controller (`0x1B`, `0x27`, `0x2E`, `0x2F`) non torna niente, e ogni diagnostica che ci conti misura rumore. GPIO12 è anche un pin di strapping (MTDI).
- **GPIO33 non è portato fuori**: il cablaggio documentato del secondo controller del 12.2" con `CS_S = GPIO33` è **da sostituire con GPIO32**. Liberi per un secondo controller: 32 (CS consigliato), 4/21/22; 34/35 sono input-only (solo BUSY); da evitare 0/2/12 (strapping).
- **Nessun socket microSD**: il commento "HSPI for EPD (and VSPI for SD)" degli esempi upstream descrive un pattern, non questo hardware.


## Pannelli e-paper usati

Sono **etichette elettroniche SOLUM (ESL) recuperate** e ripilotate, senza datasheet del controller dal produttore. Le dimensioni si scrivono `NwxMh` = N px in larghezza × M px in altezza, post-rotazione. Il campo colore del codice modello (`R` = BWR, `4` = BWRY) **non è un dato per unità**: il film montato lo dice solo l'etichetta serigrafata sul vetro. Dettaglio e misure in `gxepd2_097c_driver.md`, `gxepd2_122c_driver.md`, `ssd1677_command_set.md`.

**SOLUM 9.7"** (`EL097R2CRN`): 672×960 nativi, usato come **960w × 672h**. Controller **SSD1677**, un solo COF. **Tre colori, il quarto non esiste** (misurato: con `(0x26, 0x24)` = `(0,0)` nero, `(0,1)` bianco, `(1,x)` rosso; VSH1 = nero, VSH2 = rosso, punto di lavoro del nero 15 V). Sotto un pixel di accent il piano BW è indifferente.

**SOLUM Newton PRO 12.2"** (`EL122H6W4A`): 768×960 nativi, usato come **960w × 768h**. Due controller SSD16xx da 960×384, uno per coda FFC, divisi per righe (l'SSD1677 ha 680 gate, il pannello 768): BUSY attivo alto, banda slave specchiata nel data path. Il driver pilota una sola banda: **la seconda coda FFC non risponde**. Il quarto colore del film non è determinato.

Vincoli comuni, misurati:
- **Read-back inesistente** (vedi board) e **nessuna waveform di Mode 2 in OTP**: il refresh differenziale del silicio non è una scorciatoia.
- **Il partial esiste** (639 ms contro 24 s) con waveform propria via `0x32` e tensioni POR via `0x04` (un waveform setting è di 110 byte e `0x32` ne scrive 105). Sotto quella LUT `0x26` cambia significato in `(precedente, nuovo)`, quindi un frame partial è **senza rosso**: per questo vive fuori dal template, in API opt-in che il firmware non chiama. Aggiunge inchiostro bene e lo toglie male (limite del film, non della LUT): 28 frame sono il punto in cui il nero satura, la durata è 20 ms × frame + 83 ms. Le aree mai pilotate non sbiadiscono, quindi un refresh pieno serve solo ad azzerare il pavimento di grigio dell'area di lavoro, uno ogni 5-10 passate.
- **Il refresh d'area non esiste**: la finestra RAM non confina la zona ridipinta, `refresh(x, y, w, h)` fa un refresh pieno. A confinare è la LUT, non la geometria (sotto la waveform del partial `LUT0` e `LUT3` sono a zero, quindi i pixel il cui bit non cambia fra le due RAM non vengono pilotati), ed è comunque inutile per la velocità: 168, 48 e 24 righe misurano tutte 24,65 s, e restringere la finestra guadagna solo sul push SPI, lo 0,6% del ciclo.
- **Leve che non accorciano il refresh pieno**: i banchi di waveform per temperatura (34 set WS0..WS33, uno per range TR0..TR33, forzabili con `0x18 = 0x48` e `0x1A`) danno al meglio ~1 s, misurati 22963 ms a 40 °C e 22961 a 70 °C contro 24007 a 20 °C, mentre verso il freddo la waveform si allunga fino a 59067 ms a 0 °C. Il MUX di `0x01` non è una leva: 24010 / 24031 / 24033 ms a MUX 671 / 335 / 167, e il datasheet lo limita a 300..680.
- Per scrivere testo e forme in partial si disegna su una `GFXcanvas1` e si passa il buffer a `drawImagePartial(..., invert = true, pgm = false)`, con `x` e `w` multipli di 8 (ricetta in `GxEPD2_SOLUM_ESL/README.md` §3.1).
- **Da non usare su un tre colori** (misurato sul vetro): il bit SM nel terzo byte di `0x01` dimezza il pilotaggio (nero grigio a righe alternate) e il `{40 00}` di `0x21` prima del refresh pieno fa sparire l'accent. Alzare VSH1 sopra i 15 V è l'unica leva che può danneggiare il film in modo permanente, quindi si resta ai valori POR.


## Librerie

**`A:\tmp\GxEPD2-master`**: clone **gitignorato** di ZinggJM/GxEPD2 al tag 1.6.9, **di sola lettura**, per consultare i sorgenti. Non modificarlo e non trattarlo come codice del progetto. Sta fuori dal progetto perchè arduino-cli copia l'intero albero dello sketch a ogni build (arduino-cli segue le junction e copierebbe il clone comunque). Per consultarlo c'è `epd.code-workspace`, multi-root con due vincoli:
- **la prima radice deve restare il progetto** (`"."`): Claude Code identifica il progetto dalla prima cartella, e così chat e memorie si ritrovano come aprendo la cartella;
- **i percorsi sono relativi al file di workspace**: su macOS il clone deve stare in una cartella `tmp` accanto al repo, altrimenti VS Code segna la radice come non disponibile e basta correggere quella riga.

Alla toolchain **non** arriva il clone: `C:\tmp\arduino\user\libraries\GxEPD2` è una **copia potata** (senza `src\bitmaps\`, con il solo `src\GxEPD2_EPD.cpp`) che rigenera `C:\tmp\arduino\rigenera-libreria-gxepd2.ps1` e va rifatta dopo ogni aggiornamento del clone. Il pannello lo pilota il driver del submodule, da GxEPD2 arrivano solo header, quindi la potatura accorcia la build. Gli header dei driver restano perchè `GxEPD2_3C.h` li include sotto `__has_include`.

**`A:\epd\GxEPD2_SOLUM_ESL`**: submodule (branch `main`, GPL-3.0), libreria Arduino a sè stante, header-only, che estende GxEPD2 con i driver dei pannelli SOLUM. Le modifiche ai driver si committano nel **suo** repo e nel padre si aggiorna il puntatore; `A:\epd` va clonato con `--recursive`. **Non va installata come libreria Arduino**: lo sketch la include per path relativo e un'installazione creerebbe due path per lo stesso header. Gli examples si compilano a parte con `--library` solo per quella build.

Struttura di `src/`: `GxEPD2_SOLUM.h` (ombrello), `GxEPD2_SOLUM_Pins.h` (pinout uniforme), `GxEPDImage.h` (`showImage()` condiviso), `GxEPD2_SOLUM_097c_960x672.h` e `GxEPD2_SOLUM_122c_960x768.h` (driver). Meccanismi da conoscere prima di toccarla:
- **Ombrello**: uno sketch non nomina mai la classe concreta; definisce `SOLUM_PANEL_097C` **oppure** `SOLUM_PANEL_122C` prima dell'include (zero o due danno `#error`) e usa la macro `GxEPD2_SOLUM_DRIVER_CLASS`.
- **`GxEPD2_SOLUM_Pins`**: `-1` = pin assente. Ogni pin va guardato con `>= 0` prima di `pinMode()`/`digitalWrite()`.
- **Contratto di `GxEPDImage.h`**: pretende da ogni driver `setPaged()` e `showImagePageHint()`; compone i due piani black e red e ignora un eventuale terzo. Le primitive del terzo piano (`writeImageYellow`, `preserveYellow`, `isYellowPreserved`) sono API opzionali del singolo driver.
- **Bus SPI sempre da `_pSPIx` / `_spi_settings`** della base `GxEPD2_EPD`, mai dall'oggetto `SPI` globale: un default proprio si imposta con `selectSPI()` nel costruttore, altrimenti un `selectSPI()` dello sketch resta silenziosamente inerte.

**`A:\epd\webapp`**: submodule (`cinema-programmation-feed`), scraper Python + renderer che serve al firmware l'immagine cinema già ditherata, in formato raw packed header-less. Ha una propria memoria in `webapp\.claude\memory\`.


## Architettura del firmware: separazione layout/logica

Un solo codice applicativo, due pannelli:

- **`Layout.h`** è un dispatcher: include `Layout_097c.h` o `Layout_122c.h` secondo `#define DISPLAY_VARIANT_097C` / `DISPLAY_VARIANT_122C` in testa al `.ino`, ed emette `#error` se nessuno o entrambi sono definiti.
- **`Layout_097c.h` / `Layout_122c.h`** definiscono lo stesso namespace `Layout` con simboli identici (`Panel`, `PAGE_HEIGHT`, `makePanel()`, pin, font, ~50 costanti di coordinate). Sono gli **unici** file che includono la libreria dei driver e che contengono valori pixel.
- I moduli (`Weather.h`, `Calendar.h`, `Mail.h`, `Indoor.h`, `Graphics.h`, `icons.h`, `Maintenance.h` e il `.ino`) referenziano tutto via `Layout::*`.

Vincoli da non violare:

- `Weather.h`, `Calendar.h`, `Graphics.h` dichiarano `extern GxEPD2_3C<Layout::Panel, Layout::PAGE_HEIGHT> display;`: **devono usare `Layout::PAGE_HEIGHT`, non ricalcolare `Panel::HEIGHT / 8`**. Sono argomenti template: un valore diverso produce un tipo diverso e il **link fallisce**.
- `PAGE_HEIGHT = Panel::HEIGHT / 8` è deliberato: `SOLUM_MAX_HEIGHT()` spenderebbe ~65 KB di buffer, mentre la RAM serve anche al resto del firmware.
- Modifiche di coordinate e font: **solo** nei `Layout_*.h`, mai hardcoded nei moduli. Le baseline del banner sono `BANNER_Y + offset`, così lo scaling 097c→122c si propaga da sè.
- Nei `Layout_*.h` **non** stanno le cadenze (`*_FETCH_MIN`, `OTA_WINDOW_MIN`, `WIFI_ACTIVE_HOUR_*`, `CINEMA_DAILY_FETCH_HOUR`): sono timing e vivono in `Timings.h`, mentre l'orchestrazione sta in `Scheduler.h` e nella tabella dei task del `.ino`.
- **Per aggiungere un terzo pannello**: driver nella libreria (rispettando il contratto), nuovo `Layout_<nome>.h` con gli stessi simboli, ramo `#elif` nel dispatcher e `#define DISPLAY_VARIANT_<NOME>` nel `.ino`. Nè i moduli nè la riga di costruzione del display si toccano.


## Build su questa macchina

Comando unico: **`C:\tmp\arduino\build.ps1`** (`-Clean`, `-Dettagli`, `-Board <fqbn>`, `-Partizioni <schema>`). FQBN `esp32:esp32:esp32`, `PartitionScheme=no_fs`: due slot applicative da 1984 KB e nessuna SPIFFS. Le due slot servono all'OTA dalla finestra di manutenzione, e `huge_app` ne ha una sola, quindi produrrebbe un firmware che non può aggiornarsi da `/update`. **Da qui non si flasha**: nessun binario viene esportato, è solo compilazione di verifica e l'upload avviene da un altro PC.

- L'installazione Arduino (core esp32 3.3.11, `arduino-cli` 1.5.2) sta tutta in `C:\tmp\arduino`, deroga autorizzata al vincolo "tutto su `A:\tmp\sandbox`" perchè il core non entra nello spazio libero di `A:`. Su `A:` restano solo gli output delle build, in `A:\tmp\arduino-build\<sketch>`: gli script passano tutti `--build-path`, e per gli examples del submodule si passa a mano `--build-path A:\tmp\arduino-build\<nome-esempio>`. Quella cartella serve alla compilazione incrementale e non va svuotata.
- `arduino-cli` ha il config **non** nel percorso di default: passare sempre `--config-file C:/tmp/arduino/arduino-cli.yaml`.
- Il core esp32 è installato con un **index filtrato** (`C:\tmp\arduino\setup\package_esp32_min_index.json`, generato da `pkg_esp32.json` nella stessa cartella): reinstallando o cambiando versione va rigenerato con lo stesso filtro, e si installa sempre con la versione esplicita, mai con `core upgrade`. Accanto c'è l'index filtrato del core esp8266, usato da `A:\obd`.
- Lo sketch è una **junction, non una copia** (Arduino pretende che la cartella si chiami come il `.ino`): `C:\tmp\arduino\sketch\ePaper-weather-dashboard` → `A:\epd`. La libreria `GxEPD2` invece è una copia potata, vedi la sezione Librerie.
- **`Env.h` è gitignored ed è obbligatorio per compilare**: `build.ps1` copia `Env_template.h` se manca, quindi il firmware compila ma in campo non funziona finchè non ci sono credenziali vere.
- Gli **examples del submodule non li compila questo build**: Arduino concatena i `.ino` solo dalla root dello sketch.


## Convenzioni di codice

- Lingua di commenti e documentazione: **italiano**. Commento multi riga in stile javadoc `/** */`, singola riga con `//`.
- Non tradurre dall'inglese i nomi di funzioni e variabili.
- Non usare `@since`, `@modified` nè altri riferimenti storici.
- Il logging passa dalle macro `LOG`/`LOGV` di `Log.h`.
- Privilegia soluzioni native o già presenti nelle librerie in uso (GxEPD2, Adafruit GFX, core ESP32) al codice custom, per ridurre complessità, manutenzione e duplicazione.
- Sopra ogni metodo o funzione nuova una breve descrizione, con i riferimenti alle classi che la usano se sono esterne al file; modificando un metodo esistente, il commento va riallineato allo stato attuale.
- Le memorie riflettono lo stato finale del codice, non l'avanzamento: niente "da fare", date di modifica, "aggiornato", "rimosso" e simili.

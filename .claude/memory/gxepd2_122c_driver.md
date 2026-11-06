---
name: Driver custom GxEPD2_SOLUM_122c_960x768 (pannello SOLUM 12.2")
description: Il 12.2" nel submodule - misure di dual_panel_finder (coda LUNGA = master = righe 384..767 dritta, 0x11=0x03 obbligatorio, TB da G679, MUX 384, pattern 0x46/0x47 inefficaci, Mode 2 OTP vuoto, tempi per banco di temperatura, +800 ms di artefatto nelle durate del log), topologia APERTA H1 cascade / H2 cablaggio con coda corta a BUSY flottante, ramo cascade sul modello dualssd, partial a LUT custom portato dal 9.7", identità EL122H6W4A e codice modello, evidenze FCC/OEPL
type: reference
---

Driver del **SOLUM Newton PRO 12.2"** (768w × 960h nativi, pilotato 960w × 768h, BWR, due code
FFC). File `GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM_122c_960x768.h`; libreria e contratto in
[[gxepd2_solum_esl_library]]. Doc: `README_122c.md`, `docs/122c/identificazione_pannello.md`,
`docs/122c/connessioni.html`. Strumento e log: `examples/12_2c/dual_panel_finder/` +
`dual_panel_finder.log` (osservazioni sul vetro in coda, righe `#`).

## Misure (dual_panel_finder, coda LUNGA sul connettore della Waveshare)

- **Mappatura**: la coda LUNGA (CS 15, BUSY 25) dipinge la metà BASSA, dritta, gate 0 alla
  giunzione e gate 383 sul bordo fisico dal lato della coda. Nel driver: master = coda lunga =
  righe **384..767**, specchiatura (false,false); slave = coda corta = righe 0..383, default
  (true,true) DEDOTTO (COF ruotato 180°), non misurato. Dispatch: `M` riceve `y - PART_HEIGHT`,
  `S` riceve `y`.
- **Coda CORTA** nello stesso connettore: BUSY **flottante** (segue il pull), nessuna risposta a
  SWRESET/pattern/power on, nemmeno in 3 fili o con reset 2..50 ms. Esclude "chip indipendente
  alimentato" (avrebbe BUSY pilotato).
- **`0x11 = 0x03` obbligatorio** dopo SWRESET: le candidate senza 0x11 (MINIMAL, 1330c,
  1160_T91, per-CS) fanno il refresh ma il vetro non cambia. Datasheet dice POR 0x03; l'init di
  fabbrica (`docs/openepaperlink/oepl_display_driver_unissd.c`) usa `0x11=0x02` con
  `0x44={BF 03 00 00}` e `0x4E=959` → [inferenza] l'OTP lo porta a X decrescente al reset.
- **X decrescente in hardware**: finestra da dichiarare XSA > XEA (959→0) e cursore a 959; D83
  §8.2 mostra che ribalta anche i bit nel byte → potrebbe sostituire `_reverseBits` per lo slave.
  NON provato: la sonda `e` dichiarava la finestra crescente (fasce X- vuote = errore dello
  sketch, non del silicio).
- **TB=1** (Reserved su D77) funziona come D83 "scan from G299 to G0": parte da G679, quindi con
  MUX 384 solo ~88 righe (G296..G383) cadono su gate fisiche. Usabile solo con MUX 680 + righe RAM
  296..679; non toglie lo specchio X.
- **MUX**: con 384 o 679 si vedono 384 righe; a MUX 767 con finestra 0..766 le righe oltre 679
  ricadono in cima (ritorno a capo dell'indirizzo RAM, 680 righe). MUX = `PART_HEIGHT-1` = 383.
- **Polarità**: 0x24 bit=1 bianco; `0x21 = 08` INVERTE il BW (fabbrica lo usa e compensa nei dati).
  Tre colori: bande 3 e 4 stesso rosso. Mai scrivere 0x21=08 nel driver.
- **`0x46`/`0x47` NON riempiono la RAM visualizzata** (BUSY 1-6 ms, contro ~15 ms sul 9.7" dove
  funzionano): [inferenza] codici 111 non validi su questo die (D83: 111 = NA). Fondi solo col
  push SPI. La riga `DRV _writeScreenBuffer per pattern` del riepilogo dello sketch è sbagliata.
- **Die ≠ 9.7"**: TB funziona, pattern 111 no, secondo byte di 0x21 accettato, ma LUT in formato
  D77 (105 byte, frame rate a 50 Hz). Ciò che è misurato sul 9.7" va riverificato qui.
- **Aliasing bit 7** (sonda 2, con 0x21 B[4]=0): 0x92/0x94/0x95 eseguiti dal master come
  0x12/0x14/0x15 (stessi BUSY), 0xC6/0xC7 no. Con B[4]=1 non misurato. Sonda `b` non ha
  osservato la banda del master.
- **Tempi (BUSY)**: pieno 0xF7 18,35 s a ambiente (driver `_Update_Full` 18,34 s); banchi forzati
  0x1A: 0 °C 54,8 s, 20 °C 24,3 s, 40 °C 17,8 s, 70 °C 17,8 s (oltre il range resta l'ultimo
  banco). Power on 0xC0 82 ms, power off 0xC3 221 ms, SWRESET 2 ms, load LUT dall'OTP 1-5 ms.
- **Mode 2 OTP vuoto**: 0xFC dopo reset = 86 ms, non dipinge ma lampeggia e sbiadisce il rosso;
  0xFC dopo un F7 riesegue la waveform Mode 1 rimasta caricata (18,2 s). 0xC7/0xCF con LUT vuota
  = 221 ms (solo power off). La finestra non confina il refresh (trappola nera); passate su
  finestra costano come un pieno.
- **Partial LUT custom** (LUT_PARTIAL_1160 = byte del 9.7"): 28 frame in 638 ms = stesso modello
  del 9.7" (20 ms × frame + 83 ms); 200 frame 4047 ms.
- **Deep sleep** `0x10=0x03`: ignora un refresh intero, RAM persa (pixel casuali), reset+init lo
  svegliano. `0x10=0x11` accettato (BUSY alto), ritenzione RAM non misurata.
- **Clock**: metà del frame scritta a 20 MHz uscita pulita; driver a 10 MHz (guadagno ~36 ms).
- **ARTEFATTO**: nelle sessioni 1-3 del log ogni durata `22=..` ha **+800 ms** (vecchia
  `waitRefresh` contava la finestra di attesa del secondo fronte). Corretto nello sketch (tFall,
  come `examples/097c/panel_diagnostic`). Le durate di `waitBusy` non erano toccate.
- Rifiuto isolato di `0x22=F7` con l'init SOLUM (schermata 3), mai ripetuto in ~25 passate: non
  spiegato.

## Topologia: APERTA

- **H1 coda corta = slave in cascade** (M/S# a VSS): oscillatore e booster spenti, CL + rail +
  VDD + VCOM dal master (D83 §6.12), BUSY_N lasciato aperto (SSD2677) → BUSY flottante e silenzio,
  come misurato. Coerente col tag di fabbrica: un CS, un BUSY, una sola sezione analogica, secondo
  connettore nudo alimentato da piste del primo (foto `docs/122c/fcc/122_pcb_ffc_*`). L'obiezione
  "cascade = 800 sources × 300 gates" descrive l'uso tipico, NON vieta lo split sulle gate (ogni
  chip 960 source × 384 gate della propria metà): l'esclusione che stava qui era sbagliata.
- **H2**: piedinatura/alimentazione della coda corta diverse (code a 21 pin, connettore da 24).
- **Discrimina il multimetro** a pannello staccato: M/S# coda corta → VSS (H1) o VDDIO; continuità
  CL e rail fra le code. Poi: H1 → un CS, ponte CL+rail, `ADDRESSING_CASCADE`; H2 → secondo
  breakout con CS_S=GPIO32, BUSY_S=GPIO35, `ADDRESSING_DUAL_CS`.
- **Retro del tag** (foto Core ricomposte dal PDF FCC: le immagini sono strisce 4032×252, solo il
  render di pagina con pymupdf le ordina; `docs/122c/fcc/122_core_tag_board_retro.jpg`,
  `122_core_tag_retro_fascio.jpg`): 16-18 piste dirette dal connettore col booster a quello nudo,
  uguale sul retro PRO. Torna con le 9 linee slave←master dell'SSD1680 (CL, VDD, VDDIO, VGH, VGL,
  VSH1, VSH2, VSL, VCOM) + VCI/VSS + bus. H1 rafforzata; il contatto CL resta ignoto.
- **Coda corta = FFC diverso** da `FPC-7717` (sagoma propria): pinout da mappare, non ereditare.
- **Waveshare J1 flottanti** (netlist): 1, 4, 19 senza net, 6/7 solo su R5/R6 non montate → unici
  posti possibili per CL e M/S# sul FFC. 24 − 21 contatti misurati = 3 assenti: [inferenza] 2 dei 5
  flottanti esistono sul FFC.
- **SSD1680 Rev 0.14** (`docs/122c/SSD1680_Solomon-Systech.pdf`): VDDIO nella lista cascade, cap VDD
  solo sul master, BUSY slave aperto (verbatim nel pin table).
- **Semantica opcode in cascade** [inferenza]: `GxEPD2_579c_GDEY0579Z93::_writeFromImage` scrive lo
  slave PRIMA del master per quadrante → opcode nudi = solo master, |0x80 = solo slave. Con B[4]=0
  il master esegue 0x9x come 0x1x (sonda 2): 0x21 B[4]=1 va mandato PRIMA di ogni |0x80. La sonda
  `b` lo fa (e manda anche 0x81 MUX); `_InitDisplay` del driver manda 0x91 prima di 0x21 e nessun
  0x81: da allineare alla sonda dopo il bring-up.
- **Firmware di fabbrica**: nessun dump pubblico (il teardown Newton M3 di jirkabalhar.cz non lo
  pubblica); quel nRF52811 non aveva APPROTECT → dump SWD possibile solo avendo il tag 12.2".

## Stato del driver

| Voce | Valore |
|---|---|
| dispatch | M `y - PART_HEIGHT`, S `y`; `ScreenPart::writeImagePart` ritorna bool |
| `_InitDisplay` | UNA passata nuda: 0x12, attesa BUSY + `delay(10)` (D77 Fig. 9-1, come GxEPD2_1160_T91), 0x0C `AE C7 C3 C0 80`, 0x01 MUX 383, 0x3C=01, 0x18=80, 0x11=03; in cascade solo `0x11\|0x80 = 03` allo slave e `0x21 = 00 10`; `_using_partial_mode=false` |
| cascade | modello `dualssd.cpp` / `GxEPD2_579c_GDEY0579Z93`: comandi comuni nudi (0x12, 0x21, 0x3C, 0x20, 0x10), a `\|0x80` solo 0x11/0x44/0x45/0x4E/0x4F/0x24/0x26; MAI 0x92 (aliasing = SWRESET del master) |
| `full_refresh_time` / `busy_timeout` | 60000 ms (ripiego senza BUSY) / 120 s |
| `partial_refresh_time` | 2000 ms |
| `hasFastPartialUpdate` / `hasPartialUpdate` | false / true; `refresh(x,y,w,h)` = pieno |
| partial | API del 9.7": `writeScreenBufferPrevious`, `writeImageToPrevious` (override), `refreshPartial(...)`, `drawImagePartial(Part)`, `setPartialLut`; `GxEPD2_SOLUM_122c_lut_partial[110]` copia del 9.7" (header 9.7" in produzione non toccato); `_Init_Part` 0x3C=C0 + 0x32 105 B + 0x04 3 B in broadcast; `_Update_Part` gancio + `_PowerOn` + 0x22=CC + 0x20; `_previous_in_color_ram`, `_cleanColorIfPrevious` in `_Update_Full`/`setPaged`/scritture d'accent, `_cleanAccentIfDirty` lo azzera; piani SOLO via push SPI |
| `_Update_Full` | pulizia 0x26 se frame precedente, 0x3C=01 broadcast, `_using_partial_mode=false`, gancio, 0xF7 |
| `hibernate` | 0x10=0x03, riarma `_initial_write`, azzera i flag |
| gancio avanzamento | in `_Update_Full` e `_Update_Part` |
| clock | 10 MHz dai costruttori, `selectSPI()` dello sketch lo sostituisce |

Il template chiama `writeImageToPrevious` solo in `nextPageBW()` (non usato): override sicuro.
`_waitWhileAnyBusy` ignora lo slave se manca `cs2` o `busy2` (GPIO35 input-only senza pull: non
togliere la doppia guardia) e sempre in cascade. Pin/init/reset/BUSY dalla base GxEPD2_EPD, senza
copie ([[feedback_native_methods]]); bus da `_pSPIx`/`_spi_settings` passato alle ScreenPart in una
`Bus` temporanea (GxEPD2_3C copia il driver).

**Verifica a banco ancora da fare** (sketch pronto, fase driver `r`): pieno = tile degli angoli
bassi + metà inferiore della giunzione dritti nella metà bassa, barra rossa a `H-96`, badge a
`PART_HEIGHT+8`; partial = quadrato nero ~0,64 s poi bianco, barra rossa intatta, cornice che non
si scurisce (in sonda `l` variante T64 si è scurita con 0x3C=C0).

## Identità e contesto

- Due linee, stesso pannello: Newton **PRO** `2AFWN-EL122H6W4A` (tag `PRO_12.2_Nordic_TAG_R02`,
  nRF52811, vetro `Newton PRO 12.2" BWR normal`) e Newton **Core/M3** `2AFWN-EL122H5CRC` (vetro
  `M3 12.2" NEWTON BWR Normal`); `2AFWN-EL122R2WRN` generazione precedente. Code `FPC-7717`, 21
  contatti misurati. 190.1×237.6 mm, pitch 0.2475 mm, esercizio 0..40 °C.
- Codice modello `EL <taglia><gen><scocca><colore><tag>`: il `4` di `EL122H6W4A` è designazione di
  LINEA (tutta la PRO è `...W4A`), il film lo dice il vetro: BWR.
- Due controller perchè 768 gate > 680 del chip (9.7" 672 e 11.6" 640 stanno in uno, una coda).
  Code da bordi lunghi opposti, ognuna col proprio COF. SSD16xx, non UC8179 (800×600 non copre).
- OEPL non ha il 12.2"; `dualssd` solo per 792×272 (split sulle source). SSD2677 = fratello 4 colori
  con command set UC, da non confondere (datasheet in `docs/`).
- 12.48" GDEY1248Z51 (UC8179): 4 CS, 4 BUSY, 2 boost per controller indipendenti; il tag 12.2" ha 1
  CS e 1 boost → argomento per H1. Modello architetturale `GxEPD2_1248c` (ScreenPart).
- Board: Waveshare ESP32 Driver V3, CS_S su GPIO32 (33 non portato fuori), BUSY_S GPIO35
  ([[waveshare_esp32_driver_board]]). Datasheet: D77 = `docs/SSD1677_Rev1.0_2018-11_*.pdf`
  (pagina fisica = stampata − 1), D83 = `docs/SSD1683_Rev1.0_2021-01_*.pdf`; `pdftotext` c'è.

## Sketch dual_panel_finder

SPI diretta, una coda alla volta; `.ino` + Config/Report/Graphics/Controller/ProbesElettriche/
ProbesFrame/ProbesWaveform/ProbesDriver. 24 sonde, `t` le esegue tutte (filtro `sondaAttiva`
a uint32_t: con uint16_t saltava i bit 16-22). Tasti: minuscole e cifre tutte occupate, la sonda
`C` (S_CL, bit 23, ProbesElettriche) usa la maiuscola: cerca CL col multimetro sui J1 flottanti
1/4/6/7/19 in tre stati fermi a tasto (clock spento; 0x22=0x80 + 0x21=00 10; 0x21=00 00), solo
clock e mai analogico (rail scarichi accanto ai contatti); esiti `clContatto`/`altoContatto` nella
scheda DRV; esce subito se cfg.coda non è LUNGA (default CORTA). Fase driver `r`: frame pieno + due partial,
durate misurate dalla fine del gate (`driverGateFineMs`), non dalla chiamata. Log: registrare con
un ponte che imposti il baud SUL DESCRITTORE APERTO: su macOS `stty -f` + reopen lo perde e si
leggono byte illeggibili. `tools/registra_run.py` ora lo fa con termios sul descrittore (non
ancora provato a banco dopo la correzione). Fondo della sonda `e` via push SPI; le sonde `a`, `y`,
`u` usano ancora i pattern per il fondo (residui sul vetro).

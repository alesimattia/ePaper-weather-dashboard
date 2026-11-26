---
name: Refresh differenziale B/N del 9.7"
description: Il partial usato dentro il loop paged, frame per frame: orchestrazione in GxEPDFrame::render della libreria (armo, uscita alla prima pagina rifiutata, rifacimento pieno), CRC32 del piano rosso per pagina contro l'ultimo frame pieno, ruoli di 0x24 e 0x26 scambiati a ogni passata con righe VS di LUT1 e LUT2 scambiate, controller in Sleep e non in deep sleep finchè c'è un riferimento, frame pieno sempre con reset e init, un guasto del BUSY che non convalida il riferimento, politica in PartialRefresh.h con l'isteresi del freddo, elementi in nero scelti per non spostare il rosso, rischi non risolti dalle fonti e cosa prova l'esempio differential_check
type: project
---

Un frame è un refresh pieno (24 s, scrive il rosso) oppure un partial B/N (~0,64 s, lascia fermo il
rosso già sul vetro). Il partial è corretto solo se il piano rosso del frame nuovo è identico a quello
sul vetro, e il driver lo verifica mentre il template gli passa i piani. Il partial in sè, la sua LUT e
i suoi limiti stanno in [[gxepd2_097c_driver]]; qui c'è il modo in cui lo si usa a ogni frame.

## Dove sta

- **Meccanismo**: sezione REFRESH DIFFERENZIALE in testa a `GxEPD2_SOLUM_097c_960x672.h`.
- **Stato e regole senza I/O**: `GxEPD2_SOLUM_Differential.h`, provato su host. Il driver fa solo I/O.
- **Orchestrazione del frame**: `GxEPDFrame::render()` in `GxEPD2_SOLUM_ESL/src/GxEPDFrame.h`, nella
  libreria. Tiene il protocollo con il driver sopra il loop paged (armo, uscita alla prima pagina
  rifiutata, rifacimento pieno, salto del frame invariato, esito con guasto del BUSY e durata); il
  firmware passa solo il disegno di una pagina e la decisione della politica.
- **Politica**: `PartialRefresh.h` (funzioni pure: decisione, contatori del giorno, `finalReason()` che
  traduce l'esito di `render()` nel motivo finale, alias dei trait di `GxEPDFrame`) più le costanti `PARTIAL_*` nella sezione compile-time di `Timings.h`, non
  configurabili da NVS: vedi [[timings_nvs_config]]. Tetto di partial consecutivi 6, soglia di
  temperatura 18 °C con isteresi di 2 °C, gap massimo dall'ultimo frame in **millisecondi** (due
  cicli più il margine del poll: `CYCLE_S` è in secondi e `BSEC_MARGINE_POLL_MS` in ms, e mescolarli
  rende pieno ogni frame; c'è uno `static_assert`).
- **Integrazione**: `Weather::renderFrame()` chiama `GxEPDFrame::render()` e logga l'esito;
  `initDisplay()` nel `.ino` accende il differenziale e il salto del frame invariato; interruttore generale
  `DISPLAY_PARTIAL_REFRESH`, a 0 il driver non calcola nessun CRC e il percorso di scrittura è quello
  di sempre.
- **Esempio sul vetro**: `GxEPD2_SOLUM_ESL/examples/097c/differential_check`.

## Come funziona

1. Il firmware decide con `decide()`; se il partial è possibile `GxEPDFrame::render()` arma il driver
   con `beginDifferentialFrame()` **prima di `firstPage()`**, perchè `firstPage()` chiama `setPaged()`
   nel driver.
2. Il loop paged disegna come sempre. A ogni `writeImage(black, color)` il driver calcola il CRC32
   (funzione della ROM, `esp_rom_crc32_le`) del piano colore della pagina e lo confronta con quello
   dell'ultimo frame pieno. Un riferimento vale 8 pagine da 84 righe; la struct ne regge 16.
3. Tutte uguali: il B/N nuovo va nella RAM che non contiene il B/N del vetro e `refresh(false)` lancia
   il partial. Una diversa: il driver salta le scritture e non tocca il pannello, `render()` esce dalla
   passata alla prima pagina rifiutata (le pagine dopo non cambierebbero l'esito, e il driver disarma
   il frame anche senza arrivare al refresh) e rifà il frame in pieno con una seconda passata, un
   rendering in più trascurabile contro i 24 s del pieno.
4. Se `beginDifferentialFrame()` non arma (nessun riferimento) il primo rendering è già pieno e **non**
   se ne fa un secondo: raddoppierebbe a 48 s.

L'esito di `render()` vale `Full`, `Partial`, `Skipped` o `Refused`, con la mask delle pagine rifiutate,
il guasto del BUSY e la durata dell'ultimo refresh. Per l'uscita anticipata la mask contiene la sola
prima pagina rifiutata, perchè le successive non arrivano più alla macchina a stati, e `render()` la
legge prima della passata piena, che la ripulirebbe; il log dice "pieno: rosso cambiato dalla pagina N"
con N l'indice di quella pagina.

Non c'è un frame store, perchè senza PSRAM non c'è dove tenerlo. Dopo un pieno il B/N sul vetro sta in
0x24 e 0x26 contiene il rosso; i ruoli si **scambiano a ogni passata**. La LUT la sceglie solo la
coppia di bit (Tables 6-4 e 6-5 del datasheet non hanno nessun concetto di precedente o nuovo), quindi
con il nuovo in 0x26 le righe VS di LUT1 e LUT2 si mandano scambiate, mentre TP, RP e frame rate sono
per gruppo e restano uguali. Un pixel rosso ha il bit B/N a 1 in entrambe le RAM (lo garantisce
`drawPixel` del template) e cade su LUT3 a zero.

## Il salto del frame invariato viene prima

In `refresh(false)` un guasto del BUSY già alzato precede tutto (non rinfresca, non registra il frame
e butta il riferimento), poi il controllo del frame invariato ([[frame_skip_unchanged]]) precede il partial: se i due piani nero e colore sono identici a quelli dell'ultimo frame mandato al vetro non parte nè il partial nè il pieno, la catena non cresce e i ruoli delle RAM non si scambiano. Vale anche per un frame non armato, dove il riferimento resta spento. L'esito è `Reason::Unchanged`, contato a parte da partial e pieni, e il frame saltato aggiorna comunque `lastFrameMs`.

## Invarianti

- **Un partial non segue mai un reset senza un 0xF7 in mezzo.** Un SWRESET riporta VCOM e VGH ai POR e
  solo 0xF7 li ricarica dall'OTP; la catena delle sonde è stata misurata in quella condizione. Per
  questo ogni frame pieno riparte da reset e init (`setPaged()` forza `_hibernating` e abbassa
  `_init_display_done`, e `_hibernating` solo con RST cablato perchè è `_reset()` ad abbassarlo), e
  `_InitDisplay()` fuori da un frame spegne il riferimento.
- **Con un riferimento valido niente deep sleep**: `hibernate()` esce dopo il power off e il
  controller resta in Sleep, documentato a 25-40 µA con la RAM accessibile (datasheet SSD1677 p. 42);
  il deep sleep documentato **perde** la RAM. Si evita così la dipendenza dal comportamento di
  `0x10 = 0x01`, misurato su un esemplare e non documentato per il 1677. Senza riferimento resta
  `0x03` come prima.
- **Qualunque scrittura di RAM che non viene dal frame spegne il riferimento.** Le tre primitive di
  scrittura invalidano quando la guardia interna (`_diffInternalWrite`, RAII) è bassa; il loop paged,
  le pulizie di `setPaged()` e `_Update_Full()` e il riempimento di `_initial_write` la alzano.
  `refresh(x, y, w, h)` invalida e disarma, e le passate delle API opt-in (sottoclasse dell'esempio)
  invalidano.
- **Il riferimento si convalida solo a refresh pieno finito.** Pagine contigue dall'alto che coprono
  l'altezza, nessuna scrittura estranea (`dirty`) e nessun guasto del BUSY: un refresh con il BUSY
  ancora alto allo scadere dell'attesa (`Timeout`) o sceso prima della durata minima plausibile
  (`TooShort`) non convalida il riferimento, che si butta, e il frame dopo è pieno; vale anche per un
  partial. Il conteggio delle pagine si azzera a ogni commit, così un refresh pieno fuori dal loop non
  rivalida con CRC vecchi.
- **Dopo un partial 0x26 contiene B/N**: `_markPreviousInColorRam()` alla prima scrittura del nuovo in
  0x26 e dopo ogni partial, perchè un refresh pieno la rileggerebbe come accent.
- **L'orientamento è un parametro** di `_Init_Part` e `_Update_Part`; le API opt-in, che presuppongono
  0x26 = frame precedente, passano sempre l'orientamento normale. La LUT si ricarica quando
  l'orientamento caricato cambia, e conta nella finestra di manutenzione dove nessun reinit cade fra
  due frame.
- Un frame armato **si disarma da sè** in `refresh(false)`; la mask delle pagine con rosso cambiato
  resta leggibile fino al prossimo `arm()`, e `GxEPDFrame::render()` la legge prima del secondo
  rendering.

## Aggancio al firmware

`renderFrame()` e `initDisplay()` non sono template, e un `if constexpr` scartato fuori da un template
viene comunque compilato: le chiamate all'API passano tutte da helper template di `GxEPDFrame` (con gli
alias in `PartialRefresh`), che con un driver senza `hasDifferentialRefresh` non fanno niente. Così il
driver 122c, che dichiara solo il salto del frame invariato, non si tocca; la sua compatibilità la prova un test del trait con pannelli finti, perchè
`build.ps1` prende il pannello dal define del `.ino` e un `-D` esterno fa scattare l'`#error` di
`Layout.h`. Una temperatura NaN non supera mai la soglia (la guardia è la negazione di `>=`), e il
giorno dei contatori entra come argomento e si azzera solo con ora valida.

Sensore e temperatura in `decide()`: input `sensorPresent` = `Indoor::attivo()` (sensore Indoor, BME680 o
BME280, inizializzato in `begin()`). **Sensore assente → partial senza controllo della temperatura** (niente soglia nè
isteresi; catena, gap e primo frame valgono comunque), scelta dell'utente: rischio accettato di nero
sbiadito al freddo, perchè la LUT del partial non compensa la temperatura. Sensore collegato con
campione non valido (BSEC non ancora stabilizzato, campione scaduto): `SensorInvalid`, frame pieno.

L'isteresi del freddo è un latch di `onFrameDone()`: la decisione `TooCold` lo arma e `Partial` lo
disarma, mentre `LongGap`, `ChainLimit` e `SensorInvalid` lo lasciano com'è, perchè non dicono niente
della temperatura e azzerarlo toglierebbe l'isteresi al frame dopo. Con il latch armato la soglia sale
di `PARTIAL_TEMP_HYSTERESIS_C`.

## Perchè questi elementi sono in nero

Un testo rosso che cambia a ogni ciclo o a ogni fetch impedisce il partial. Sono in nero la temperatura
del riquadro Interno e il suo "--" e il tratto previsione del grafico.
La previsione è tratteggiata (`TC_DASH_ON` e `TC_DASH_OFF` in `Layout_common.h`) per restare
distinguibile dal passato; la fase parte dal vertice "adesso", con un contatore locale a
`drawTempChart` che avanza solo sui passi della previsione, e va replicata uguale in
`tools/layout_preview.py`. Restano rossi, con il loro ritmo di cambio: temperature e icone dei blocchi
meteo (le previsioni `hourly[3/6/9]` scorrono con l'ora), eventi di oggi e cella di oggi, icone di
alba e tramonto, badge Tuya, cinema (una volta al giorno) e percorso. **Il percorso dalle 7 alle 9 si
scarica a ogni ciclo e contiene rosso che cambia quasi sempre**, quindi quei frame sono quasi tutti
pieni. Mettere in nero anche le temperature meteo rende poco: la quota di partial ha comunque il tetto
N/(N+1) della catena e un frame rifiutato azzera la catena.

## Verifica

- Su host: `test/test_partial.cpp`, due volte nel runner, con l'interruttore spento e acceso. Prova la
  macchina a stati (rifiuti a pagina 0, intermedia e ultima, uscita anticipata con la sola prima pagina
  nella mask, pagine fuori ordine, invalidazione a frame armato, refresh fuori dal loop, alternanza
  dei ruoli, disarmo, mask), la politica con il latch dell'isteresi, `finalReason()` dai quattro esiti
  di `GxEPDFrame::Outcome` e il trait. Le chiamate SPI del driver e `GxEPDFrame::render()` con un
  display vero non girano su host.
- Sul vetro: `differential_check` prova con il codice vero tre cose che nessuna sonda copre, perchè
  `drawPartial` delle sonde riscrive 0x26 dopo ogni refresh e la catena tiene l'analog acceso:
  l'orientamento scambiato, il power off 0xC3 fra due passate e un sonno di minuti in Sleep. Se
  mostrasse una perdita di stato il ripiego è SWRESET, init e `0x22 = 0xB1` (3 ms, ricarica VGH e VCOM
  dall'OTP) prima di ogni partial; se fallisse l'orientamento scambiato non c'è ripiego senza i dati
  del frame precedente.

## Rischi che le fonti non risolvono

- **DC**: una transizione bianco→nero porta +120 V·frame e nero→bianco −120, quindi il ciclo di un
  pixel è bilanciato; ma il pieno dell'OTP non restituisce quei valori, e la tenuta del film con molti
  partial al giorno non è determinabile dal datasheet nè dal log.
- **Alone sul rosso**: nelle sonde il testimone rosso sta a 28 righe dalla fascia pilotata, e a 6-9 px
  da un testo nero che cambia nei blocchi meteo non è mai stato misurato.
- **Soglia di temperatura**: non ha base misurata; il sensore Indoor non misura il vetro e il sensore interno
  del controller ha ±2 °C. Senza sensore non c'è nessuna guardia: il partial al freddo non è mai stato
  misurato sul vetro.
- **Alimentazione**: il riferimento vive solo se il pannello resta alimentato fra i frame. Il netlist
  testuale della Waveshare V3 mostra una rete `EPD_3.3V` con un S8050 e R19 su IO4, forse un interruttore
  (non confermato, lo schema non si rende qui): il firmware non pilota IO4 e chi lo facesse deve
  invalidare il riferimento. Vedi [[waveshare_esp32_driver_board]].
- La quota reale di partial si legge dal log: ogni frame riporta esito, motivo e contatori del giorno.

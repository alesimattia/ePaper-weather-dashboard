---
name: Refresh differenziale B/N del 9.7"
description: Il partial usato dentro il loop paged, frame per frame: CRC32 del piano rosso per pagina contro l'ultimo frame pieno, ruoli di 0x24 e 0x26 scambiati a ogni passata con righe VS di LUT1 e LUT2 scambiate, controller in Sleep e non in deep sleep finchè c'è un riferimento, frame pieno sempre con reset e init, politica in PartialRefresh.h, elementi in nero scelti per non spostare il rosso, rischi non risolti dalle fonti e cosa prova l'esempio differential_check
type: project
---

Un frame è un refresh pieno (24 s, scrive il rosso) oppure un partial B/N (~0,64 s, lascia fermo il
rosso già sul vetro). Il partial è corretto solo se il piano rosso del frame nuovo è identico a quello
sul vetro, e il driver lo verifica mentre il template gli passa i piani. Il partial in sè, la sua LUT e
i suoi limiti stanno in [[gxepd2_097c_driver]]; qui c'è il modo in cui lo si usa a ogni frame.

## Dove sta

- **Meccanismo**: sezione REFRESH DIFFERENZIALE in testa a `GxEPD2_SOLUM_097c_960x672.h`.
- **Stato e regole senza I/O**: `GxEPD2_SOLUM_Differential.h`, provato su host. Il driver fa solo I/O.
- **Politica**: `PartialRefresh.h` (funzioni pure: decisione, contatori del giorno, trait e helper di
  aggancio al driver) più le costanti `PARTIAL_*` nella sezione compile-time di `Timings.h`, non
  configurabili da NVS: vedi [[timings_nvs_config]]. Tetto di partial consecutivi 6, soglia di
  temperatura 18 °C con isteresi di 2 °C, gap massimo dall'ultimo frame in **millisecondi** (due
  cicli più il margine del poll: `CYCLE_S` è in secondi e `BSEC_MARGINE_POLL_MS` in ms, e mescolarli
  rende pieno ogni frame; c'è uno `static_assert`).
- **Integrazione**: `Weather::renderFrame()` e `initDisplay()` nel `.ino`; interruttore generale
  `DISPLAY_PARTIAL_REFRESH`, a 0 il driver non calcola nessun CRC e il percorso di scrittura è quello
  di sempre.
- **Esempio sul vetro**: `GxEPD2_SOLUM_ESL/examples/097c/differential_check`.

## Come funziona

1. Il firmware decide con `decide()`; se il partial è possibile chiama `beginFrame()` **prima di
   `firstPage()`**, perchè `firstPage()` chiama `setPaged()` nel driver.
2. Il loop paged disegna come sempre. A ogni `writeImage(black, color)` il driver calcola il CRC32
   (funzione della ROM, `esp_rom_crc32_le`) del piano colore della pagina e lo confronta con quello
   dell'ultimo frame pieno. Un riferimento vale 8 pagine da 84 righe; la struct ne regge 16.
3. Tutte uguali: il B/N nuovo va nella RAM che non contiene il B/N del vetro e `refresh(false)` lancia
   il partial. Una diversa: il driver salta le scritture, non tocca il pannello e il firmware rifà il
   frame in pieno. Il rendering in più dopo un rifiuto costa poche centinaia di ms contro 24 s.
4. Se `beginFrame()` non arma (nessun riferimento) il primo rendering è già pieno e **non** se ne fa un
   secondo: raddoppierebbe a 48 s.

Non c'è un frame store, perchè senza PSRAM non c'è dove tenerlo. Dopo un pieno il B/N sul vetro sta in
0x24 e 0x26 contiene il rosso; i ruoli si **scambiano a ogni passata**. La LUT la sceglie solo la
coppia di bit (Tables 6-4 e 6-5 del datasheet non hanno nessun concetto di precedente o nuovo), quindi
con il nuovo in 0x26 le righe VS di LUT1 e LUT2 si mandano scambiate, mentre TP, RP e frame rate sono
per gruppo e restano uguali. Un pixel rosso ha il bit B/N a 1 in entrambe le RAM (lo garantisce
`drawPixel` del template) e cade su LUT3 a zero.

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
  `refresh(x, y, w, h)` invalida e disarma, `refreshPartial()` e le API opt-in invalidano.
- **Il riferimento si convalida solo a refresh pieno finito.** Pagine contigue dall'alto che coprono
  l'altezza, nessuna scrittura estranea (`dirty`) e BUSY basso dopo l'attesa: la base esce dal timeout
  stampando "Busy Timeout!" ma senza segnalarlo, quindi il driver rilegge il pin. Il conteggio delle
  pagine si azzera a ogni commit, così un refresh pieno fuori dal loop non rivalida con CRC vecchi.
- **Dopo un partial 0x26 contiene B/N**: `_markPreviousInColorRam()` alla prima scrittura del nuovo in
  0x26 e dopo ogni partial, perchè un refresh pieno la rileggerebbe come accent.
- **L'orientamento è un parametro** di `_Init_Part` e `_Update_Part`; le API opt-in, che presuppongono
  0x26 = frame precedente, passano sempre l'orientamento normale. La LUT si ricarica quando
  l'orientamento caricato cambia, e conta nella finestra di manutenzione dove nessun reinit cade fra
  due frame.
- Un frame armato **si disarma da sè** in `refresh(false)`; la mask delle pagine con rosso cambiato
  resta leggibile fino al prossimo `arm()`, e il firmware la legge prima del secondo rendering.

## Aggancio al firmware

`renderFrame()` e `initDisplay()` non sono template, e un `if constexpr` scartato fuori da un template
viene comunque compilato: le chiamate all'API passano tutte da helper template su `PartialRefresh`,
che con un driver senza `hasDifferentialRefresh` non fanno niente. Così il driver 122c non dichiara
niente e non si tocca; la sua compatibilità la prova un test del trait con pannelli finti, perchè
`build.ps1` prende il pannello dal define del `.ino` e un `-D` esterno fa scattare l'`#error` di
`Layout.h`. Una temperatura NaN non supera mai la soglia (la guardia è la negazione di `>=`), e il
giorno dei contatori entra come argomento e si azzera solo con ora valida.

Sensore e temperatura in `decide()`: input `sensorPresent` = `Indoor::attivo()` (BME680 inizializzato
in `begin()`). **Sensore assente → partial senza controllo della temperatura** (niente soglia nè
isteresi; catena, gap e primo frame valgono comunque), scelta dell'utente: rischio accettato di nero
sbiadito al freddo, perchè la LUT del partial non compensa la temperatura. Sensore collegato con
campione non valido (BSEC non ancora stabilizzato, campione scaduto) → `SensorInvalid`, frame pieno.

## Perchè questi elementi sono in nero

Un testo rosso che cambia a ogni ciclo o a ogni fetch impedisce il partial. Sono in nero la temperatura
del riquadro Interno e il suo "--", il tratto previsione del grafico e la busta delle mail non lette.
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
  macchina a stati (rifiuti a pagina 0, intermedia e ultima, pagine fuori ordine, invalidazione a
  frame armato, refresh fuori dal loop, alternanza dei ruoli, disarmo, mask), la politica e il trait.
  Le chiamate SPI del driver non girano su host.
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
- **Soglia di temperatura**: non ha base misurata; il BME680 non misura il vetro e il sensore interno
  del controller ha ±2 °C. Senza BME680 non c'è nessuna guardia: il partial al freddo non è mai stato
  misurato sul vetro.
- **Alimentazione**: il riferimento vive solo se il pannello resta alimentato fra i frame. Il netlist
  testuale della Waveshare V3 mostra una rete `EPD_3.3V` con un S8050 e R19 su IO4, forse un interruttore
  (non confermato, lo schema non si rende qui): il firmware non pilota IO4 e chi lo facesse deve
  invalidare il riferimento. Vedi [[waveshare_esp32_driver_board]].
- La quota reale di partial si legge dal log: ogni frame riporta esito, motivo e contatori del giorno.

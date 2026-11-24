---
name: Vincoli numerici layout (097c + 122c)
description: Come Layout_common.h deriva la geometria dalla misura dello schermo del pannello, le somme che la tengono in piedi e i vincoli fra font e misure delle zone
type: project
---

Convenzione dimensioni in questo file: `NwxMh` = N px larghezza (X) × M px altezza (Y); costanti `*_W` = larghezza, `*_H` = altezza.

**Da dove vengono i numeri.** `Layout_097c.h` / `Layout_122c.h` danno solo `SCREEN_W/H` (dal driver); `Layout_common.h` deriva tutto il resto da quei valori e da sette token: `MARGIN` 10 (dal bordo), `GUTTER` 10 (fra zone affiancate), `STACK_GAP` 16 (fra zone impilate), `BOX_RADIUS` 12, `TITLE_INSET` 16, `BOX_PAD` 12, `TOP_BOX_Y` 14. Vedi [[layout_separation]].

**Perchè STACK_GAP 16 e TOP_BOX_Y 14.** Un titolo in FreeSansBold12pt seduto sul bordo ha la baseline a mezza cap height (8 px) e sporge di circa 9 px sopra il bordo: fra due riquadri impilati servono quindi almeno 16 px, e il riquadro in cima allo schermo ha il bordo a y=14 perchè il titolo resti a 5 px dal bordo.

**Colonne (uguali sui due pannelli, larghezza 960):**
- sinistra x=10..620: wallpaper (`CINEMA_X/Y` = 10, `CINEMA_W` 610);
- destra x=630..950: scheda calendario (`SIDEBAR_W` 320);
- banner: Interno 180 + 10 + Meteo 300 + 10 + Previsioni 440 = 940 = 960 - 2 × 10. Meteo = blocco corrente 160 + colonna sole 140; Previsioni = 3 blocchi da 146.

**Riquadro Meteo.** L'orario del meteo corrente (`slots[0].epoch` = `current.dt`) sta sul bordo superiore accanto al titolo, in `FONT_BODY` sulla baseline del titolo, a `TITLE_TIME_GAP` da "Meteo" e dentro la striscia bianca allungata (`drawWeatherTitleTime()`); il blocco corrente lascia vuota la riga `WX_TIME_BASELINE`, che le Previsioni usano. Colonna sole dall'alto: grafico (`TC_TOP_Y` = `BANNER_Y` + 8, `TC_H` 92), barra temp-range (`TRB_BASELINE`), alba e tramonto su `SUNRISE_BASELINE` = `WX_TEMP_BASELINE` e `SUNSET_BASELINE` = `WX_DESC_BASELINE`. Il grafico è largo `TC_W` = `TRB_W` + `TC_LABEL_W` e centrato su `TC_CENTER_X` = `SUN_COL_CENTER_X` - `TC_LABEL_W` / 2: la curva coincide in x con la barra e la colonna delle etichette min/max sporge a sinistra nella zona libera accanto all'icona del blocco corrente (con "-12" restano 7 px dal riquadro dell'icona). `TC_W` va dichiarato dopo `TC_LABEL_W`.

**Termometro dell'Interno.** È alto `TEMPERATURE_ICON_H` = `SMALL_ICON_SIZE` + 1 (stelo con punta arrotondata) e si disegna 1 px sopra `smallIconY()`, così il bulbo resta allineato alle altre icone; è l'unica icona di `icons.h` non quadrata.

**Altezze (097c / 122c):**
- banner alto 196, `BANNER_Y` = `SCREEN_H - MARGIN - 196` = 466 / 562; i suoi titoli sporgono di circa 10 px sopra il bordo;
- wallpaper da `CINEMA_Y` a `BANNER_Y - STACK_GAP`: y=10..450 / 10..546, `CINEMA_H` 440 / 536, con il fondo allineato a quello della scheda calendario;
- scheda calendario y=14..450 / 14..546: intestazione dei giorni alta 22 e griglia del mese 6 righe da 20, filetto a y=179 su entrambi, eventi da y=185 alti 259 / 355.

**Culling per pagina.** Il loop paged ha 8 pagine (fasce da 84 px sul 097c, 96 sul 122c; con `ROTATION 0` la pagina k è [k·PH, (k+1)·PH)). `Weather::renderFrame()` chiama `Calendar::draw()` e `drawBanner()` solo se il loro riquadro, allargato in alto di `STACK_GAP` per il titolo, tocca la fascia; dentro, intestazione e celle della griglia e righe evento (`EVT_ROW_H`) si saltano fuori fascia. Il criterio regge perchè ogni testo, filetto, badge e icona sta dentro la propria riga o il proprio riquadro, discendenti compresi (misurato sui font): un elemento nuovo che sporge oltre il suo riquadro verrebbe tagliato. Non ci sono sbiancamenti: il buffer di ogni pagina lo azzera il template, e prima dei riquadri disegna solo il wallpaper, tagliato all'altezza `CINEMA_H`. `GxEPD2_3C` non ridefinisce `fillRect`, quindi Adafruit_GFX la esegue pixel per pixel attraverso `drawPixel` virtuale, e il clip sulla pagina avviene solo lì dentro: uno sbiancamento di un'area grande costa centinaia di migliaia di chiamate per pagina, e non se ne aggiungono. Il log `render` (LOGV) misura i ms di solo disegno per frame.

**Righe minime.** Gli eventi si dividono in 7, 6 o 5 righe, il massimo con righe da almeno 40 px (`MIN_EVT_ROW_H` in `Calendar.h`, che `tools/layout_preview.py` legge dal sorgente): 6 righe da 43 sul 097c, 7 da 50 sul 122c. La soglia viene da orario e data in 9pt: con `EVT_TIME_BASELINE` 16 e `EVT_DATE_FROM_BOTTOM` 6, sotto i 40 px i discendenti di "tutto gg" toccano la data sotto. Con un solo calendario configurato la cache di una sorgente ha 5 eventi, quindi la sesta riga del 097c resta "--" (vedi [[calendar_module]]). Il limite delle righe evento (in `Calendar.h`) e le aree non positive (in `Layout_common.h`) fermano la build con `static_assert`; la misura del wallpaper no, per scelta dell'utente.

**Font per elemento, oltre ai ruoli di `Layout_common.h`.** Intestazione e numeri del mese, orario e data degli eventi, cifre della barra temp-range sono in `FONT_SMALL`; titolo degli eventi in `FONT_BODY`. Nell'Interno temperatura e umidità sono in `FONT_LARGE`, qualità dell'aria e pressione in `FONT_BODY`, per scelta dell'utente. Righe dell'Interno: T, RH, pressione in riga 3 con entrambi i sensori; IAQ col pedice in riga 4 solo col BME680, che col BME280 resta vuota. Il pedice in riga 4 finisce a `BANNER_Y+180` e il badge TUYA ERR (FreeSans9pt, H 12 px) parte da `BANNER_Y+189`.

**Font e larghezze che vincolano le zone** (misurate dalle glyph table GFX):
- "Mediocre 200" (caso peggiore reale di `Indoor::iaqLabel`) misura 139 px in 12pt e 107 in 9pt. Nell'Interno largo 180 la riga IAQ ripiega su `FONT_SMALL` quando testo + `INDOOR_UNIT_GAP` + pedice supera `INDOOR_X + INDOOR_W - BOX_PAD`: ripiegano "Discreta", "Pessima" e "Mediocre", le altre restano in 12pt. Senza ripiego l'Interno non scende sotto 200. Dopo il ripiego la riga più larga è la stessa IAQ in 9pt con il pedice, che fissa l'Interno ad almeno ~170; la pressione ("1014" in 12pt più "mBar" in 9pt) è più corta;
- le etichette meteo di `icons_weather.h` e la riga "100% 12.5mm" entrano nei blocchi (152 px utili nel corrente, 138 nelle previsioni) in 12pt o in 9pt, quindi non vengono mai troncate. Il blocco corrente non scende sotto 160 perchè "Poco nuvoloso" (151 px) e le altre etichette frequenti fino a 151 px restino in 12pt; la più larga in 9pt è "Temporale forte" (125 px);
- "-12" in 9pt misura 25 px: la colonna delle etichette del mini-grafico (`TC_LABEL_W`) è 28.

**Cadenze accoppiate non documentate:**
- Il display non ha una cadenza propria: il frame si disegna a ogni ciclo del sensore (~301 s) dentro la fascia WiFi, a passo di rete concluso, e non esiste un rate limit separato. Il refresh invece il driver non lo lancia se il frame è identico a quello sul vetro ([[frame_skip_unchanged]]). Il pavimento è la durata di un refresh pieno, 24 s, e un `static_assert` impone `CYCLE_S >= DISPLAY_REFRESH_PIENO_S`. Il risveglio è la scadenza esatta del sensore, senza clamp. Vedi [[timing_chain]] e [[scheduler_task_table]].
- Le fasce di percorso e cinema (`ROUTE_WINDOW_*`, `CINEMA_WINDOW_*` in `Routefinder.h`, sovrascrivibili da `/route`) devono stare dentro la fascia WiFi e non sovrapporsi, altrimenti il task non accenderebbe mai la radio o le due immagini si contenderebbero il buffer; lo impongono `static_assert` e validazione a runtime ([[routefinder_morning_route]]).

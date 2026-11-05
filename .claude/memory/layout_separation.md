---
name: Separazione layout/logica via Layout.h dispatcher
description: Architettura del firmware per supportare due display SOLUM (097c 960w x 672h, 122c 960w x 768h) con stessa logica applicativa
type: project
---

Convenzione dimensioni in questo file: `NwxMh` = N px larghezza (X) × M px altezza (Y).

Il firmware ePaper supporta due pannelli (SOLUM 9.7" 960w x 672h e SOLUM 12.2" 960w x 768h, landscape post-rotation) con la stessa logica applicativa. La separazione è realizzata da:

- `Layout.h` (dispatcher): include `Layout_097c.h` o `Layout_122c.h` in base a `#define DISPLAY_VARIANT_097C` / `DISPLAY_VARIANT_122C` settato nel `.ino`. Emette `#error` se nessuno o entrambi sono definiti.
- `Layout_097c.h` / `Layout_122c.h`: definiscono nel namespace `Layout` solo il pannello (`Panel`, `PAGE_HEIGHT`, `makePanel()`, pin, `BUSY_WAKE_PIN`, `ROTATION`, `SCREEN_W/H` presi da `Panel::WIDTH_VISIBLE` e `Panel::HEIGHT`) e due manopole, `CINEMA_H` e `MAIL_ROWS_PER_COL`; poi `#define LAYOUT_PANEL_SELECTED` e includono `Layout_common.h`.
- `Layout_common.h`: token, font e tutte le coordinate, derivate dalle manopole e da `SCREEN_W/H`; dà `#error` se incluso senza un Layout di pannello. Numeri e somme in [[layout_invariants]].
- I moduli (`Weather.h`, `Calendar.h`, `Mail.h`, `Graphics.h`, `icons.h`, `.ino`) referenziano tutto via `Layout::*` e includono `Layout.h`. Nessun #include diretto al driver fuori dai Layout_*.h.
- `Weather.h`, `Calendar.h` e `Graphics.h` dichiarano `extern GxEPD2_3C<Layout::Panel, Layout::PAGE_HEIGHT> display;`. **Devono usare `Layout::PAGE_HEIGHT`, non ricalcolare `Panel::HEIGHT / 8`**: sono argomenti template, quindi un valore diverso da quello del `.ino` dà un tipo diverso e il link fallisce. Verificato cambiando `PAGE_HEIGHT` a `/12` e ricompilando.
- Le include guard non devono avere il nome di una costante `Layout::`: `Mail.h` usa `MAIL_MODULE_H` perchè una guardia `MAIL_H` cancellerebbe `Layout::MAIL_H` (verificato: errore di compilazione in `Mail::draw()`).

**Come i Layout arrivano al driver.** Ogni `Layout_*.h` definisce il proprio `SOLUM_PANEL_*` e include l'ombrello `GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM.h`, che gli restituisce la classe come macro: nessun Layout nomina il driver. Poi espone tre simboli che il `.ino` usa senza sapere quale pannello sia montato:

- `using Panel = GxEPD2_SOLUM_DRIVER_CLASS;`
- `PAGE_HEIGHT = Panel::HEIGHT / 8`: scelta deliberata, la `SOLUM_MAX_HEIGHT()` della libreria spenderebbe ~65 KB di buffer su un pannello 960 px di larghezza, qui la RAM serve anche al resto del firmware.
- `makePanel()`: factory che costruisce il driver passando `GxEPD2_SOLUM_Pins`, la struct di pinout uniforme della libreria. È ciò che assorbe le arità diverse dei costruttori (il 12.2" ha due CS e due BUSY) e tiene il `.ino` a una riga sola: `GxEPD2_3C<Layout::Panel, Layout::PAGE_HEIGHT> display(Layout::makePanel());`. Il template copia il driver, quindi un driver non può tenere riferimenti a se stesso (vedi [[gxepd2_solum_esl_library]]).

**Why:** supportare più pannelli con lo stesso codice applicativo e la stessa resa, cambiando solo ciò che il pannello impone. Entrambi i driver stanno nel submodule `GxEPD2_SOLUM_ESL` (vedi [[gxepd2_solum_esl_library]]); del 12.2" **è validata una sola banda**, vedi [[gxepd2_122c_driver]].

**How to apply:**
- Modifiche di coordinate, token e font: in `Layout_common.h`; altezza del wallpaper e righe della Posta: nelle manopole del `Layout_<pannello>.h`. Mai valori pixel nei moduli.
- Le baseline del banner sono `BANNER_Y + offset` e `BANNER_Y` deriva da `SCREEN_H`, così l'altezza diversa del 12.2" si propaga senza toccare la baseline math.
- Per aggiungere un terzo pannello: driver nella libreria (contratto in [[gxepd2_solum_esl_library]]) + nuovo `Layout_<nome>.h` con i simboli di pannello e le due manopole + ramo `#elif` nel dispatcher + `#define DISPLAY_VARIANT_<NOME>` nel `.ino`. Nè i moduli nè la riga di costruzione del display vanno toccati.
- Dopo ogni modifica di coordinate, font, icone o disegno dei moduli si rigenerano le anteprime con `python tools/layout_preview.py`: `preview.svg` (mostrata dal README su GitHub), `preview_<pannello>.html` (frame nel browser con zoom e zone) e `docs/layout_<pannello>.png`. Lo script trova i pannelli dai `Layout_*.h`, legge costanti, font GFX, icone e wallpaper veri e replica il disegno dei moduli e le primitive di Adafruit_GFX: cambiando il disegno di un modulo va allineata la funzione corrispondente dello script. Le anteprime non si eliminano, perchè l'utente le usa per vedere subito l'interfaccia implementata.

**Cosa NON sta nei Layout:**
- `hspi.begin(13,12,14,15)` e `selectSPI(hspi, SPISettings(10MHz))`: board-level (Waveshare ESP32 Driver Board), restano nel `.ino`. Eccezione: `Layout_122c.h` passa 13/12/14 anche dentro `makePanel()`, perchè quel driver apre il bus da sè in `init()`.
- Cadenze e timeout: stanno tutti in `Timings.h`. Sono timing/orchestrazione, non layout: vedi [[timings_nvs_config]].
- L'URL del server cinema: lo compone il `.ino` da `CINEMA_ORIGIN` e dalle costanti `Layout::CINEMA_*`, vedi [[esp32_cinema_consumer]].
- Le icone meteo: in `icons_weather.h`, generato, vedi [[weather_icons]].
- `Indoor.h`/`Maintenance.h`: solo logica, nessuna coordinata.

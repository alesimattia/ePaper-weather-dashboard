---
name: Separazione layout/logica via Layout.h dispatcher
description: Architettura del firmware per supportare due display SOLUM (097c 960w x 672h, 122c 960w x 768h) con stessa logica applicativa
type: project
---

Convenzione dimensioni in questo file: `NwxMh` = N px larghezza (X) × M px altezza (Y).

Il firmware ePaper supporta due pannelli (SOLUM 9.7" 960w x 672h e SOLUM 12.2" 960w x 768h, landscape post-rotation) con la stessa logica applicativa. La separazione è realizzata da:

- `Layout.h` (dispatcher): include `Layout_097c.h` o `Layout_122c.h` in base a `#define DISPLAY_VARIANT_097C` / `DISPLAY_VARIANT_122C` settato nel `.ino`. Emette `#error` se nessuno o entrambi sono definiti.
- `Layout_097c.h` / `Layout_122c.h`: definiscono lo stesso namespace `Layout` con simboli identici (`Panel`, `PAGE_HEIGHT`, `makePanel()`, pin display, font, costanti pixel). ~50 costanti coord/dim + 5 font stanno qui e non nei moduli applicativi.
- I moduli (`Weather.h`, `Calendar.h`, `Graphics.h`, `icons.h`, `.ino`) referenziano tutto via `Layout::*` e includono `Layout.h`. Nessun #include diretto al driver fuori dai Layout_*.h.
- `Weather.h`, `Calendar.h` e `Graphics.h` dichiarano `extern GxEPD2_3C<Layout::Panel, Layout::PAGE_HEIGHT> display;`. **Devono usare `Layout::PAGE_HEIGHT`, non ricalcolare `Panel::HEIGHT / 8`**: sono argomenti template, quindi un valore diverso da quello del `.ino` dà un tipo diverso e il link fallisce. Verificato cambiando `PAGE_HEIGHT` a `/12` e ricompilando.

**Come i Layout arrivano al driver.** Ogni `Layout_*.h` definisce il proprio `SOLUM_PANEL_*` e include l'ombrello `GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM.h`, che gli restituisce la classe come macro: nessun Layout nomina il driver. Poi espone tre simboli che il `.ino` usa senza sapere quale pannello sia montato:

- `using Panel = GxEPD2_SOLUM_DRIVER_CLASS;`
- `PAGE_HEIGHT = Panel::HEIGHT / 8` — scelta deliberata: la `SOLUM_MAX_HEIGHT()` della libreria spenderebbe ~65 KB di buffer su un pannello 960 px di larghezza, qui la RAM serve anche al resto del firmware.
- `makePanel()` — factory che costruisce il driver passando `GxEPD2_SOLUM_Pins`, la struct di pinout uniforme della libreria. È ciò che assorbe le arità diverse dei costruttori (il 12.2" ha due CS e due BUSY) e tiene il `.ino` a una riga sola:
  `GxEPD2_3C<Layout::Panel, Layout::PAGE_HEIGHT> display(Layout::makePanel());`

**Why:** supportare più pannelli con lo stesso codice applicativo. Entrambi i driver stanno nel submodule `GxEPD2_SOLUM_ESL` (vedi [[gxepd2_solum_esl_library]]); del 12.2" **è validata una sola banda** (960x384 stampa con una coda cablata), mentre il funzionamento delle due bande insieme non è ancora provato, vedi [[gxepd2_122c_driver]].

**How to apply:**
- Modifiche al layout (coord, font, dimensioni cinema): editare SOLO `Layout_097c.h` e/o `Layout_122c.h`. Mai hardcoded nei moduli.
- Modifiche alla logica applicativa: editare `Weather.h` / `Calendar.h` / `.ino`. I valori pixel arrivano sempre da `Layout::*`.
- Le baseline del banner (`ICON_Y`, `DESC_BASELINE`, ecc.) sono espresse come `BANNER_Y + offset` nei Layout, così lo scaling 097c→122c (BANNER_Y 460→556, +96 px) si propaga in cascata senza riscrivere la baseline math.
- Per aggiungere un terzo pannello: driver nella libreria (contratto in [[gxepd2_solum_esl_library]]) + nuovo `Layout_<nome>.h` con gli stessi simboli + ramo `#elif` nel dispatcher + `#define DISPLAY_VARIANT_<NOME>` nel `.ino`. Nè i moduli nè la riga di costruzione del display vanno toccati.

**Differenze chiave 097c vs 122c (già codificate in Layout_122c.h):**
- SCREEN_H (altezza Y) 672 → 768; BANNER_Y 460 → 556 (banner ancorato al fondo); CINEMA_H 300 → 335; MAIL_H 160 → 221; EVT_H 230 → 326. Tutto il resto invariato (stessa larghezza X 960w).
- Font identici per ora: `FONT_LARGE_BOLD = FreeSansBold18pt7b`, `FONT_LARGE = FreeSans18pt7b`, `FONT_BODY = FreeSans12pt7b`, `FONT_SMALL = FreeSans9pt7b`, `FONT_MICRO = Picopixel`. Cambiarli in Layout_122c.h se servisse.

**Cosa NON sta nei Layout_*.h:**
- `hspi.begin(13,12,14,15)` e `selectSPI(hspi, SPISettings(10MHz))`: board-level (Waveshare ESP32 Driver Board), restano nel `.ino`. Eccezione: `Layout_122c.h` passa 13/12/14 anche dentro `makePanel()`, perchè quel driver apre il bus da sè in `init()`.
- Cadenze fetch (`WEATHER_FORECAST_FETCH_MIN`, `CAL_*_FETCH_MIN`, `MAIL_GOOGLE_FETCH_MIN`, `OTA_WINDOW_MIN`, `BOOT_WIFI_TIMEOUT_MS`, `WIFI_ACTIVE_HOUR_*`, `CINEMA_DAILY_FETCH_HOUR`): timing/orchestrazione, non layout.
- `Indoor.h`/`Mail.h`/`Ota.h`: solo logica, nessuna coord — invariati dal refactor.
- `Graphics::drawFieldsetRect`: già parametrico, layout-agnostic.

---
name: Separazione layout/logica via Layout.h dispatcher
description: Architettura del firmware per supportare due display SOLUM (097c 960x672, 122c 960x768) con stessa logica applicativa
type: project
---

Il firmware ePaper supporta due pannelli (SOLUM 9.7" 960x672 e SOLUM 12.2" 960x768) con la stessa logica applicativa. La separazione e' realizzata da:

- `Layout.h` (dispatcher): include `Layout_097c.h` o `Layout_122c.h` in base a `#define DISPLAY_VARIANT_097C` / `DISPLAY_VARIANT_122C` settato nel `.ino`. Emette `#error` se nessuno o entrambi sono definiti.
- `Layout_097c.h` / `Layout_122c.h`: definiscono lo stesso namespace `Layout` con simboli identici (typedef `Panel`, pin display, font, costanti pixel). Il refactor ha estratto ~50 costanti coord/dim + 5 font dai moduli applicativi.
- I moduli (`Weather.h`, `Calendar.h`, `Graphics.h`, `icons.h`, `.ino`) referenziano tutto via `Layout::*` e includono `Layout.h`. Nessun #include diretto al driver fuori dai Layout_*.h.

**Why:** L'utente vuole supportare 2 pannelli con stesso codice. Il driver `GxEPD2_SOLUM_122c_960x768` vive nel branch `Solum_12_2`. Quando si mergia, Layout_122c.h compila contro quel driver senza altre modifiche ai moduli.

**How to apply:**
- Modifiche al layout (coord, font, dimensioni cinema): editare SOLO `Layout_097c.h` e/o `Layout_122c.h`. Mai hardcoded nei moduli.
- Modifiche alla logica applicativa: editare `Weather.h` / `Calendar.h` / `.ino`. I valori pixel arrivano sempre da `Layout::*`.
- Le baseline del banner (`ICON_Y`, `DESC_BASELINE`, ecc.) sono espresse come `BANNER_Y + offset` nei Layout, cosi' lo scaling 097c→122c (BANNER_Y 460→556, +96 px) si propaga in cascata senza riscrivere la baseline math.
- Per aggiungere un terzo pannello: scrivere driver + nuovo `Layout_<nome>.h` (stessi simboli) + ramo `#elif` nel dispatcher + `#define DISPLAY_VARIANT_<NOME>` nel `.ino`. I moduli non vanno toccati.

**Differenze chiave 097c vs 122c (gia' codificate in Layout_122c.h):**
- SCREEN_H 672 → 768; BANNER_Y 460 → 556 (banner ancorato al fondo); CINEMA_H 440 → 536; EVT_H 230 → 326. Tutto il resto invariato (stessa larghezza 960).
- Font identici per ora: `FONT_LARGE_BOLD = FreeSansBold18pt7b`, `FONT_LARGE = FreeSans18pt7b`, `FONT_BODY = FreeSans12pt7b`, `FONT_SMALL = FreeSans9pt7b`, `FONT_MICRO = Picopixel`. Cambiarli in Layout_122c.h se servisse.

**Cosa NON sta nei Layout_*.h:**
- Pin HSPI bus (13/12/14/15) e `SPISettings(10MHz)`: board-level (Waveshare ESP32 Driver Board), restano nel `.ino`.
- Cadenze fetch (`WEATHER_FORECAST_FETCH_MIN`, `CAL_*_FETCH_MIN`, `MAIL_GOOGLE_FETCH_MIN`, `OTA_WINDOW_MIN`, `BOOT_WIFI_TIMEOUT_MS`, `WIFI_ACTIVE_HOUR_*`, `CINEMA_DAILY_FETCH_HOUR`): timing/orchestrazione, non layout.
- `Indoor.h`/`Mail.h`/`Ota.h`: solo logica, nessuna coord — invariati dal refactor.
- `Graphics::drawFieldsetRect`: gia' parametrico, layout-agnostic.

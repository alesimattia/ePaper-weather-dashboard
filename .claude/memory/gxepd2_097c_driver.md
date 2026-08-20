---
name: Driver custom GxEPD2_SOLUM_097c_960x672
description: Dove vive il driver SOLUM 4-color SSD1677 (submodule GxEPD2_SOLUM_ESL, libreria Arduino) + vincoli architetturali e costi noti
type: reference
---

Il driver del pannello SOLUM 9.7" (672w × 960h native portrait, controller SSD1677, 4 colori
nativi B/W/R/Y) è una **libreria Arduino a sè stante**, non un pezzo di `A:\epd`:

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
La sua §0 tiene le caratteristiche del pannello: codici modello SOLUM (`EL097F5*4C` / `EL097F6*4A`
Pro, Newton Core), pitch 0,210 mm e 120,95 dpi, range 0~40 °C, pinout FPC 24 pin, volumi di
transfer SPI.

**Vincoli architetturali** quando si lavora sul driver:

- **Origine**: derivato da
  [`GxEPD2_1330c_GDEM133Z91`](https://github.com/ZinggJM/GxEPD2/blob/1.6.9/src/gdem3c/GxEPD2_1330c_GDEM133Z91.cpp),
  l'unico driver SSD1677 della libreria (13.3" 3-colori). Non esiste in GxEPD2 un driver più vicino
  al SOLUM: quelli in `epd4c/` usano controller diversi (JD79665AA, SSD2677 — 2bpp packed, non 3
  piani separati).

- **Pittfall `GxEPD_YELLOW`**: il template upstream `GxEPD2_3C` mappa `GxEPD_YELLOW` sul piano red
  (`0x26`) — `GxEPD2_3C.h:196`. `drawPixel(x, y, GxEPD_YELLOW)` non scrive sul piano `0x28`. Le sole
  vie per pilotare il giallo sono `GxEPDImage::showImage` con descrittore BWRY, oppure
  `writeImageYellow()` + `preserveYellow(true)` PRIMA di `firstPage()`.

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
  davvero. **Non ancora verificato su hardware**: atteso lo stesso risultato visivo e ~260 ms in
  meno sui 22 s di refresh.
- dirty flag `_color_dirty` / `_yellow_dirty`: il cleanup accent viene saltato quando non serve
  (costa ~65 ms per canale, ~130 ms sui due, tipico sulle catene di frame B/N).
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

Toccando il driver, aggiornare anche la tabella di confronto e i bullet di dettaglio nel suo README,
così la doc resta allineata al codice.

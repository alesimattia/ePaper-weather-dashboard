---
name: Driver custom GxEPD2_097c_SOLUM_672x960
description: Pointer al README dedicato + sintesi vincoli architetturali del driver SOLUM 4-color SSD1677
type: reference
---

Il driver custom del pannello SOLUM 9.7" (672×960, controller SSD1677, 4 colori
nativi B/W/R/Y) ha una **doc dedicata** in:

[c:\epd\GxEPD2_097c_SOLUM_672x960\README.md](../../GxEPD2_097c_SOLUM_672x960/README.md)

Quel file contiene tutto il dettaglio: motivazione, API (`showImage`,
`writeImageBlack/Red/Yellow`, `preserveYellow`), pattern "yellow out-of-band",
sistema di descrittori, tabella di confronto vs base GDEM133Z91 (15 righe),
bullet di dettaglio delle ottimizzazioni applicate.

**Vincoli architetturali da ricordare** quando si lavora sul driver:

- **Base di partenza**: fork di
  [`GxEPD2_1330c_GDEM133Z91`](https://github.com/ZinggJM/GxEPD2/blob/master/src/gdem3c/GxEPD2_1330c_GDEM133Z91.cpp)
  (l'unico driver SSD1677 in libreria, 13.3" 3-colori). Verificato a piu' riprese
  che NON esiste un driver GxEPD2 piu' vicino al SOLUM (i driver `epd4c/`
  usano controller diversi: JD79665AA, SSD2677 — 2bpp packed, NON 3-plane).

- **Pittfall `GxEPD_YELLOW`**: il template upstream `GxEPD2_3C` mappa
  `GxEPD_YELLOW` sul piano red (`0x26`) — vedi `GxEPD2_3C.h:196`. `drawPixel(x, y, GxEPD_YELLOW)`
  non scrive sul piano `0x28` (yellow). L'unica via per pilotare il giallo è
  `GxEPDImage::showImage` con descrittore BWRY o
  `writeImageYellow()` + `preserveYellow(true)` PRIMA di `firstPage()`.

- **Page-tracking `_show_image_page_hint`**: contatore parallelo a
  `_current_page` (privato in `GxEPD2_3C`), avanzato in
  `writeImage(black, color, ...)` (chiamato da nextPage), resettato in
  `setPaged()` (override) e `_Update_Full()`. Permette il row-skip in
  `showImage` senza modificare GxEPD2.

- **API pubblica usata dal sketch**: solo `GxEPDImage::showImage(display, *desc)`
  dentro un loop `firstPage/nextPage`. Tutto il resto (single-channel API,
  `preserveYellow`) è compositing avanzato.

**Ottimizzazioni applicate (state-of-the-art al 2026-04-30):**
- Dirty flags `_color_dirty` / `_yellow_dirty` (~160 ms risparmiati su catene B/N)
- `hibernate()` idempotente + reset flag al SWRESET
- Cleanup accent simmetrico tra `writeImage` e `writeImagePart` BW
- `_preserve_yellow` reset centralizzato in `_Update_Full()`
- `showImage` row-skip via page-hint counter (~145 ms/refresh)
- Bulk SPI via `_pSPIx->writeBytes(buf, n)` in `_writeImage` / `_writeImagePart` / `_writeScreenBuffer` (~290 ms/refresh)

Il refresh elettroforetico fisico resta ~22 s (limite hardware del pannello,
irriducibile in software). L'overhead software pre-refresh è ora ~30 ms.

Quando si tocca il driver, aggiornare anche la tabella di confronto e il
bullet di dettaglio nel README dedicato per mantenere la doc allineata.

---
name: Aritmetica del blob cinema
description: Calcoli stride/plane/total per il body /cinema/arduino + dipendenze numeriche tra firmware e webapp
type: reference
---

Calcoli derivati. Le costanti `CINEMA_*` vivono nel namespace `Layout` (`Layout_097c.h` o `Layout_122c.h`); il `.ino` le riferisce come `Layout::CINEMA_*`.

**097c (620×440 BWRY):**
- `stride = ceil(620/8) = 78` byte/riga (`Layout::CINEMA_STRIDE`).
- Dimensione singolo piano = `78 * 440 = 34320` byte (`Layout::CINEMA_PLANE_SZ`).
- Total body = `3 piani * 34320 = 102960` byte (`Layout::CINEMA_TOTAL_SZ`).
- `Content-Length` atteso = 102960 esatti; ogni mismatch → fallback PROGMEM.

**122c (620×536 BWRY):**
- `stride = 78` (invariato, stessa width 620).
- `Layout::CINEMA_PLANE_SZ = 78 * 536 = 41808` byte.
- `Layout::CINEMA_TOTAL_SZ = 3 * 41808 = 125424` byte.
- `Content-Length` atteso = 125424.

**Memoria ESP32:**
- 097c: 3 buffer da 34320 ≈ 100 KB.
- 122c: 3 buffer da 41808 ≈ 123 KB.
- Il modello Waveshare driver board ha tipicamente 4 MB PSRAM (modulo WROVER): allocazione comoda per entrambe le varianti.
- Senza PSRAM (modulo WROOM bare): `ESP.getFreeHeap()` ≈ 200 KB libere. 097c (100 KB) passa al limite, 122c (123 KB) e' rischioso e potrebbe richiedere PSRAM. `allocPlaneBuffer()` preferisce PSRAM e fa fallback heap-interno.

**Encoding bit (compatibilita' GxEPD2):**
- `pack_mask_msb` lato webapp riceve `mask = (indices != color_idx)` → bit=1 dove pixel NON è di quel colore.
- Conseguenza dedotta: padding di riga (per width non multiplo di 8) viene riempito con `1` perchè `np.ones(...)` nel pad. Coerente con la convenzione "1 = bianco/default": il pad non viene mai "acceso" su nessun canale.

**Catena dimensioni → URL → ESP32 (compile-time per variante):**
- Per cambiare display: scommentare l'altro `DISPLAY_VARIANT_*` nel `.ino`. Il `Layout_<variante>.h` corrispondente espone `CINEMA_W`, `CINEMA_H`, `CINEMA_URL` con la query `width=...&height=...&colors=bwry` gia' coerente. Niente da toccare a mano lato firmware.
- Cambiare `Layout::CINEMA_W` / `CINEMA_H` SENZA aggiornare anche `width=`/`height=` in `Layout::CINEMA_URL` produce `Content-Length` mismatch silenzioso e fallback PROGMEM perpetuo. Stessa cosa per `colors`: se si vuole BWR (2 piani) bisogna cambiare URL E sostituire i 3 buffer/loop con 2 nel `.ino`.

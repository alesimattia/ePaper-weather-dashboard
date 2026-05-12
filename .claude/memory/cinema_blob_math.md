---
name: Aritmetica del blob cinema
description: Calcoli stride/plane/total per il body /cinema/arduino + dipendenze numeriche tra firmware e webapp
type: reference
---

Calcoli derivati. Le costanti `CINEMA_*` vivono nel namespace `Layout` (`Layout_097c.h` o `Layout_122c.h`); il `.ino` le riferisce come `Layout::CINEMA_*`.

Convenzione dimensioni in questo file: `NwxMh` = N px larghezza (X) × M px altezza (Y).

**097c (620w × 300h BWRY):**
- `stride = ceil(620/8) = 78` byte/riga (`Layout::CINEMA_STRIDE`).
- Dimensione singolo piano = `78 * 300 = 23400` byte (`Layout::CINEMA_PLANE_SZ`).
- Total body = `3 piani * 23400 = 70200` byte (`Layout::CINEMA_TOTAL_SZ`).
- `Content-Length` atteso = 70200 esatti; ogni mismatch → fallback PROGMEM.
- Fascia bianca y=300..460 (160h px) riservata alla UI mail (`Mail.h`).

**122c (620w × 335h BWRY):**
- `stride = 78` (invariato, stessa width 620).
- `Layout::CINEMA_PLANE_SZ = 78 * 335 = 26130` byte.
- `Layout::CINEMA_TOTAL_SZ = 3 * 26130 = 78390` byte.
- `Content-Length` atteso = 78390.
- Fascia bianca y=335..556 (221h px) riservata alla UI mail.

**Memoria ESP32:**
- 097c: 3 buffer da 23400 ≈ 69 KB.
- 122c: 3 buffer da 26130 ≈ 77 KB.
- La riduzione di CINEMA_H (440→300 su 097c, 536→335 su 122c) ha liberato ~30-50 KB rispetto al layout pre-UI-mail.
- Il modello Waveshare driver board ha tipicamente 4 MB PSRAM (modulo WROVER): allocazione comoda per entrambe le varianti.
- Senza PSRAM (modulo WROOM bare): `ESP.getFreeHeap()` ≈ 200 KB libere; ora entrambe le varianti rientrano comodamente in heap interno. `allocPlaneBuffer()` preferisce PSRAM e fa fallback heap-interno.

**Encoding bit (compatibilita' GxEPD2):**
- `pack_mask_msb` lato webapp riceve `mask = (indices != color_idx)` → bit=1 dove pixel NON è di quel colore.
- Conseguenza dedotta: padding di riga (per width non multiplo di 8) viene riempito con `1` perchè `np.ones(...)` nel pad. Coerente con la convenzione "1 = bianco/default": il pad non viene mai "acceso" su nessun canale.

**Catena dimensioni → URL → ESP32 (compile-time per variante):**
- Per cambiare display: scommentare l'altro `DISPLAY_VARIANT_*` nel `.ino`. Il `Layout_<variante>.h` corrispondente espone `CINEMA_W`, `CINEMA_H`, `CINEMA_URL` con la query `width=...&height=...&colors=bwry` gia' coerente. Niente da toccare a mano lato firmware.
- Cambiare `Layout::CINEMA_W` / `CINEMA_H` SENZA aggiornare anche `width=`/`height=` in `Layout::CINEMA_URL` produce `Content-Length` mismatch silenzioso e fallback PROGMEM perpetuo. Stessa cosa per `colors`: se si vuole BWR (2 piani) bisogna cambiare URL E sostituire i 3 buffer/loop con 2 nel `.ino`.

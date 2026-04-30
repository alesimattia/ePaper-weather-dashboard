---
name: Aritmetica del blob cinema
description: Calcoli stride/plane/total per il body /cinema/arduino + dipendenze numeriche tra firmware e webapp
type: reference
---

Calcoli derivati (non visibili come singoli valori in nessun file, vanno ricostruiti):

**Default ESP32 (620x440 BWRY):**
- `stride = ceil(620/8) = 78` byte/riga (`CINEMA_STRIDE` nel `.ino`).
- Dimensione singolo piano = `78 * 440 = 34320` byte (`CINEMA_PLANE_SZ`).
- Total body = `3 piani * 34320 = 102960` byte (`CINEMA_TOTAL_SZ`).
- `Content-Length` atteso = 102960 esatti; ogni mismatch → fallback PROGMEM.

**Memoria ESP32:**
- 3 buffer da 34320 ≈ 100 KB. Il modello ESP32 della Waveshare driver board ha tipicamente 4 MB PSRAM (modulo WROVER): allocazione comoda.
- Senza PSRAM (modulo WROOM bare): `ESP.getFreeHeap()` ≈ 200 KB libere → 100 KB su heap interno passano al limite, lasciano poco margine per ArduinoJson/HTTPClient. Da qui la preferenza PSRAM in `allocPlaneBuffer()`.

**Encoding bit (compatibilita' GxEPD2):**
- `pack_mask_msb` riceve `mask = (indices != color_idx)` → bit=1 dove pixel NON è di quel colore.
- Conseguenza dedotta: padding di riga (per width non multiplo di 8) viene riempito con `1` perchè `np.ones(...)` nel pad. Coerente con la convenzione "1 = bianco/default": il pad non viene mai "acceso" su nessun canale.

**Catena dimensioni → URL → ESP32:**
- Cambiare `CINEMA_W` o `CINEMA_H` nel `.ino` SENZA aggiornare anche `width=`/`height=` in `CINEMA_URL` produce `Content-Length` mismatch silenzioso e fallback PROGMEM perpetuo. Stessa cosa per `colors`: se nel `.ino` si vuole BWR (2 piani) bisogna cambiare URL E sostituire i 3 buffer/loop con 2.

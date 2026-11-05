---
name: Aritmetica del blob cinema
description: Calcoli stride/plane/total per il body /cinema/arduino, padding dei bit, URL composto dalle costanti del layout
type: reference
---

Le costanti `CINEMA_*` le deriva `Layout_common.h` (vedi [[layout_invariants]]); il `.ino` compone l'URL e alloca i buffer da quelle, quindi misura chiesta al server, buffer e `Content-Length` atteso non possono divergere.

Convenzione dimensioni in questo file: `NwxMh` = N px larghezza (X) × M px altezza (Y).

**Due piani su entrambi i pannelli.** Nessuno dei due ha il giallo: `CINEMA_PLANES` = 2 e l'URL chiede `colors=bwr`. Il parametro lo dà `cinemaColorsParam()` nel `.ino` dal numero di piani.

**097c (610w × 290h):**
- `stride = ceil(610/8) = 77` byte/riga (`CINEMA_STRIDE`);
- piano = `77 × 290 = 22330` byte (`CINEMA_PLANE_SZ`);
- body = `2 × 22330 = 44660` byte (`CINEMA_TOTAL_SZ`), che è il `Content-Length` atteso: ogni mismatch → fallback PROGMEM.

**122c (610w × 325h):** stride 77, piano 25025 byte, body 50050.

**Padding di riga.** 610 non è multiplo di 8: il server (`pack_mask_msb` di `dithering.py`) riempie con bit a 1 (non colore) i 6 bit bassi dell'ultimo byte di ogni riga. `GxEPDImage::showImage()` usa lo stride `ceil(W/8)` e legge solo i pixel fino a W, quindi il padding non arriva mai al pannello e il suo valore non conta.

**Memoria ESP32.** ~44 KB sul 097c, ~49 KB sul 122c. `allocPlaneBuffer()` preferisce la PSRAM e ripiega sull'heap interno; sulla board di bring-up (WROOM-32E, senza PSRAM) i piani stanno nell'heap interno, che dopo la connessione WiFi ha intorno ai 200 KB liberi.

**Server.** L'endpoint serve qualsiasi misura sotto i suoi massimi (4000 px per lato, 5 MP) esattamente com'è chiesta: niente clamp, niente quantizzazione a 25 px (quella vale solo per la pagina `/cinema` del browser).

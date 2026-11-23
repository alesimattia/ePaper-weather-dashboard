---
name: Aritmetica del blob cinema
description: Calcoli stride/plane/total per il body /cinema/arduino, padding dei bit, URL composto dalle costanti del layout
type: reference
---

Le costanti `CINEMA_*` le deriva `Layout_common.h` (vedi [[layout_invariants]]); il `.ino` compone l'URL e alloca i buffer da quelle, quindi misura chiesta al server, buffer e `Content-Length` atteso non possono divergere.

Convenzione dimensioni in questo file: `NwxMh` = N px larghezza (X) × M px altezza (Y).

**Due piani su entrambi i pannelli.** Nessuno dei due ha il giallo: `CINEMA_PLANES` = 2 e l'URL chiede `colors=bwr`. Il parametro lo dà `cinemaColorsParam()` nel `.ino` dal numero di piani.

**097c (610w × 440h):**
- `stride = ceil(610/8) = 77` byte/riga (`CINEMA_STRIDE`);
- piano = `77 × 440 = 33880` byte (`CINEMA_PLANE_SZ`);
- body = `2 × 33880 = 67760` byte (`CINEMA_TOTAL_SZ`), che è il `Content-Length` atteso: ogni mismatch è un download fallito e resta l'immagine precedente.

**122c (610w × 536h):** stride 77, piano 41272 byte, body 82544.

L'altezza è `BANNER_Y - STACK_GAP - CINEMA_Y`: il wallpaper occupa tutta la colonna sinistra fino al banner.

**Padding di riga.** 610 non è multiplo di 8: il server (`pack_mask_msb` di `dithering.py`) riempie con bit a 1 (non colore) i 6 bit bassi dell'ultimo byte di ogni riga. `GxEPDImage::showImage()` usa lo stride `ceil(W/8)` e legge solo i pixel fino a W, quindi il padding non arriva mai al pannello e il suo valore non conta.

**Memoria ESP32.** ~66 KB sul 097c, ~81 KB sul 122c. Percorso e cinema stanno in heap, due piani allocati separati da `CinemaStore.h` al boot, perchè un array statico non entra nel segmento DRAM ([[esp32_cinema_consumer]]); il wallpaper, stesso formato, sta in flash in uno dei due slot da 21 settori della partizione `wallpaper` ([[fallback_progmem_uniformity]]). Il download scrive a blocchi da 1 KB.

**Server.** L'endpoint serve qualsiasi misura sotto i suoi massimi (4000 px per lato, 5 MP) esattamente com'è chiesta: niente clamp, niente quantizzazione a 25 px (quella vale solo per la pagina `/cinema` del browser).

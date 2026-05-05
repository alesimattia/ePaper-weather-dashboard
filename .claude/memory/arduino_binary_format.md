---
name: Design arduino binary format header-less
description: /cinema/arduino ritorna solo i piani 1bpp MSB-packed concatenati, senza magic/version/metadati
type: feedback
---

Il body HTTP di `/cinema/arduino` è SOLO i piani 1bpp MSB-packed concatenati, nessun header. Ordine piani deterministico: `bw=[black]`, `bwr=[black,red]`, `bwry=[black,red,yellow]`. Convenzione bit compatibile GxEPD2: `1` = pixel NON appartiene a quel colore (cioè bianco/default per quel canale).

Stride = `ceil(width/8)`. Padding di riga riempito con bit=1. `Content-Length` atteso = `N_planes * stride * height` (vedi `cinema_blob_math.md` per i numeri concreti).

**Why:** L'ESP32 ha gia' width/height/colors nella query string che ha inviato lui, quindi ogni parsing di header sarebbe codice sprecato. Obiettivo esplicitato dall'utente: "il formato arduino restituito dal server sia il piu' efficiente da renderizzare una volta ricevuto dal esp32". Header-less = `http.getStream().readBytes()` direttamente nei buffer GxEPD2, essenzialmente un memcpy.

**How to apply:** Non aggiungere mai magic/version/checksum al body di `/cinema/arduino`. Se serve validazione, usare `Content-Length` (gia' settato da FastAPI) e l'errore HTTP. Rifiutare `colors=rgb` con 400 su questo endpoint (il display e-paper non renderizza RGB; il `Literal` in main.py lo blocca a livello di FastAPI). Se in futuro serve multi-versione, usare `Accept` header o versionare l'URL (`/cinema/arduino/v2`), non introdurre metadati nel body. Il consumer ESP32 in `ePaper-weather-dashboard-097c.ino` controlla `Content-Length == CINEMA_TOTAL_SZ` e va in fallback PROGMEM se mismatch.

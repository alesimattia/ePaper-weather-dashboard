---
name: Cautela su modifiche al firmware Arduino
description: I file Arduino in c:\epd\ root sono il firmware ESP32 in produzione - modifiche solo su richiesta esplicita
type: feedback
---

I file in `c:\epd\` root (`ePaper-weather-dashboard.ino`, `Calendar.h`, `Weather.h`, `Indoor.h`, `Env.h`, `Ota.h`, `Graphics.h`, `icons.h`, `GxEPD2_097c_SOLUM_672x960/`, `epd_image_converter.pyw`, `preview.html`, `img_test/`) sono il firmware Arduino per e-paper SOLUM 672x960 4-colori (BWRY).

**Why:** L'utente ha detto esplicitamente "non toccare i file del progetto arduino esistente" durante lo sviluppo della webapp. Questi file girano su un device fisico e una modifica accidentale richiede flash via OTA o smontaggio. Notare che il flusso OTA esiste apposta (`Ota.h`, finestra 3 min al boot) ma resta un'operazione manuale.

**How to apply:**
- Se una richiesta della webapp implicherebbe toccare questi file, prima conferma con l'utente.
- Puoi LEGGERLI per riferimento (es. `epd_image_converter.pyw` contiene le funzioni di dithering/packing portate in `c:\epd\webapp\dithering.py`; il `.ino` contiene la logica del consumer cinema).
- Se l'utente chiede esplicitamente di modificarli, procedi.
- Lo sketch principale e' `ePaper-weather-dashboard.ino` (NON `GxEPD2_1330c_GDEM133Z91.ino` come citato in alcuni commenti vecchi nel codice e nei file `Indoor.h`/`Ota.h`).
- `Env.h` e' gitignored: contiene credenziali (WiFi, OWM key, Microsoft Graph refresh token, Google Calendar OAuth, posizione GPS). Il template e' `Env_template.h`.

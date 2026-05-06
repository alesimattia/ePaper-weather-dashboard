---
name: Cautela su modifiche al firmware Arduino
description: I file Arduino in c:\epd\ root sono il firmware ESP32 in produzione - modifiche solo su richiesta esplicita
type: feedback
---

I file in `c:\epd\` root (`ePaper-weather-dashboard.ino`, `Layout.h`, `Layout_097c.h`, `Layout_122c.h`, `Calendar.h`, `Weather.h`, `Indoor.h`, `Mail.h`, `Env.h`, `Ota.h`, `Graphics.h`, `icons.h`, `GxEPD2_SOLUM_097c_960x672/`, `epd_image_converter.pyw`, `preview_097c.html`, `img_wallpaper/`) sono il firmware Arduino per e-paper SOLUM (097c 672x960 / 122c 768x960) 4-colori (BWRY). Il pannello attivo si seleziona via `#define DISPLAY_VARIANT_097C` o `DISPLAY_VARIANT_122C` in testa al `.ino`.

**Why:** L'utente ha detto esplicitamente "non toccare i file del progetto arduino esistente" durante lo sviluppo della webapp. Questi file girano su un device fisico e una modifica accidentale richiede flash via OTA o smontaggio. Notare che il flusso OTA esiste apposta (`Ota.h`, finestra 3 min al boot) ma resta un'operazione manuale.

**How to apply:**
- Se una richiesta della webapp implicherebbe toccare questi file, prima conferma con l'utente.
- Puoi LEGGERLI per riferimento (es. `epd_image_converter.pyw` contiene le funzioni di dithering/packing portate in `c:\epd\webapp\dithering.py`; il `.ino` contiene la logica del consumer cinema).
- Se l'utente chiede esplicitamente di modificarli, procedi.
- Lo sketch principale è `ePaper-weather-dashboard.ino`. Selezione del display via `#define DISPLAY_VARIANT_097C` o `DISPLAY_VARIANT_122C` in testa al `.ino`.
- `Env.h` è gitignored: contiene credenziali (WiFi, OWM key, Microsoft Graph refresh token, Google Calendar OAuth, posizione GPS). Il template è `Env_template.h`.

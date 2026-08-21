---
name: Cautela su modifiche al firmware Arduino
description: I file Arduino in A:\epd\ root sono il firmware ESP32 in produzione - modifiche solo su richiesta esplicita
type: feedback
---

I file in `A:\epd\` root (`ePaper-weather-dashboard.ino`, `Layout.h`, `Layout_097c.h`, `Layout_122c.h`, `Calendar.h`, `Weather.h`, `Indoor.h`, `Mail.h`, `Env.h`, `Ota.h`, `Graphics.h`, `icons.h`, `Env_template.h`, `GxEPD2_SOLUM_ESL/` (submodule), `epd_image_converter.pyw`, `preview_097c.html`, `preview_122c.html`, `wallpaper/`) sono il firmware Arduino per e-paper SOLUM (097c 672w x 960h / 122c 768w x 960h native portrait — convenzione NwxMh = larghezza X × altezza Y). Il firmware è **scritto** per BWRY (3 piani, descrittori e blob a 3 canali), ma che il pannello renda davvero un 4o colore non è verificato: vedi [[gxepd2_097c_driver]]. Il pannello attivo si seleziona via `#define DISPLAY_VARIANT_097C` o `DISPLAY_VARIANT_122C` in testa al `.ino`.

**Why:** L'utente ha detto esplicitamente "non toccare i file del progetto arduino esistente" durante lo sviluppo della webapp. Questi file girano su un device fisico e una modifica accidentale richiede flash via OTA o smontaggio. Notare che il flusso OTA esiste apposta (`Ota.h`, finestra 3 min al boot) ma resta un'operazione manuale.

**How to apply:**
- Se una richiesta della webapp implicherebbe toccare questi file, prima conferma con l'utente.
- Puoi LEGGERLI per riferimento (es. `epd_image_converter.pyw` contiene le funzioni di dithering/packing portate in `A:\epd\webapp\dithering.py`; il `.ino` contiene la logica del consumer cinema).
- Se l'utente chiede esplicitamente di modificarli, procedi.
- Lo sketch principale è `ePaper-weather-dashboard.ino`. Selezione del display via `#define DISPLAY_VARIANT_097C` o `DISPLAY_VARIANT_122C` in testa al `.ino`.
- Ogni modifica a questi file va validata compilando: vedi [[build_toolchain_arduino]] (`A:\tmp\arduino\build.ps1`). La compilazione qui è possibile e va usata prima di consegnare modifiche al firmware; il flash resta manuale e avviene da un'altra macchina.
- Arduino concatena in una sola unità di compilazione **solo** i `.ino` nella root dello sketch, e ricorre soltanto in `<sketch>/src/`. Un secondo `.ino` nella root darebbe `setup()`/`loop()` duplicati e romperebbe la build: gli sketch di servizio vanno altrove. Le sonde dei pannelli stanno in `GxEPD2_SOLUM_ESL/examples/097c/panel_diagnostic/` e `GxEPD2_SOLUM_ESL/examples/12_2c/dual_panel_finder/`, cioè nel submodule del driver, che il build del firmware ignora (`examples/` non è sotto `<sketch>/src/`) — vedi [[gxepd2_097c_driver]].
- `Env.h` è gitignored: contiene credenziali (WiFi, OWM key, Microsoft Graph refresh token, Google Calendar OAuth, posizione GPS). Il template è `Env_template.h`.

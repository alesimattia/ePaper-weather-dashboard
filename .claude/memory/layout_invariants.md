---
name: Vincoli numerici layout 960x672
description: Somme che validano le costanti pixel del banner + relazioni implicite tra cadenze (DISPLAY_REFRESH_MIN ↔ BSEC ↔ HOUR_END)
type: project
---

Numeri non immediatamente visibili che vincolano le costanti sparse fra `Weather.h`, `Calendar.h` e il `.ino`:

**Banner orizzontale (Weather.h, y=460..672):**
- `INDOOR_RR(5,154) + gap10 + WEATHER_RR(169,306) + gap10 + FORECAST_RR(485,470) + inset5 = 960`
- Verifica: `5 + 154 + 10 + 306 + 10 + 470 + 5 = 960` ✓ Larghezza display.
- Inset laterali 5px, gap interni 10px (dedotto sottraendo le `_X` consecutive).

**Aree verticali (height=672):**
- Wallpaper cinema 0..440 (h=440), banda bianca 440..460 (h=20, non disegnata), banner 460..672 (h=212).
- Sidebar destra `x=620..960` (w=340) sovrappone wallpaper+banda+banner; il calendar mese vive a `CAL_X=630, CAL_Y=10, CAL_W=320, CAL_H=200`. `CAL_W=320` con `gridW=308` è divisibile esattamente per 7 (44 px/cella) → centratura giorni della settimana è deduttivamente esatta.

**Spaziatura baseline indoor (4 righe in `INDOOR_RR_W=154`, `BANNER_H=212`):**
- Altezza testo visiva FreeSans18pt ≈ 27 px (ascent 22 + descent 5).
- 5 spazi uguali tra rr_top, 4 righe, rr_bottom: `G = (202 - 4*27)/5 = 19 px`.
- Step baseline = `27 + 19 = 46 px`. Costanti `INDOOR_ROWn_BASELINE` = 46, 92, 138, 184. Coerenti con la formula.

**Cadenze accoppiate non documentate:**
- `DISPLAY_REFRESH_MIN = 5` == BSEC2 ULP sample period (300s, hardcoded dalla libreria Bosch). Coincidenza calibrata: ogni wake produce esattamente un nuovo sample. Cambiare `DISPLAY_REFRESH_MIN < 5` spreca wake; `> 5` ritarda l'aggiornamento UI ma non perde sample (BSEC tiene il piu' recente).
- Il daily fetch cinema parte all'ora `CINEMA_DAILY_FETCH_HOUR` locale (default 7, allineato a `WIFI_ACTIVE_HOUR_START`). Vincolo implicito: `CINEMA_DAILY_FETCH_HOUR` DEVE cadere dentro la finestra `[WIFI_ACTIVE_HOUR_START, WIFI_ACTIVE_HOUR_END]`, altrimenti il WiFi è giu' all'ora del trigger e il daily refresh non scatta MAI. Le due costanti restano separate per poter spostare l'una senza toccare l'altra.
- `EVT_H = 230 = EVT_ROW_H * MAX_EVENTS_DISPLAYED = 46 * 5`. Stesso step baseline 46 del banner indoor: scelta di uniformita' visiva trans-modulo.

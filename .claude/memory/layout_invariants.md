---
name: Vincoli numerici layout (097c + 122c)
description: Somme che validano le costanti pixel + relazioni implicite tra cadenze (DISPLAY_REFRESH_MIN ↔ BSEC ↔ HOUR_END)
type: project
---

Convenzione dimensioni in questo file: `NwxMh` = N px larghezza (X) × M px altezza (Y); costanti `*_W` = larghezza, `*_H` = altezza.

Numeri non immediatamente visibili che vincolano le costanti definite in `Layout_097c.h` / `Layout_122c.h` e usate da `Weather.h` / `Calendar.h` / `.ino` via `Layout::*`:

**Banner orizzontale (uguale in entrambe le varianti, larghezza display 960w):**
- `INDOOR_RR(5,154) + gap10 + WEATHER_RR(169,306) + gap10 + FORECAST_RR(485,470) + inset5 = 960`
- Verifica: `5 + 154 + 10 + 306 + 10 + 470 + 5 = 960` ✓
- Inset laterali 5 px, gap interni 10 px.

**Aree verticali:**
- 097c (`SCREEN_H=672`): wallpaper 0..300 (`CINEMA_H=300`), fascia mail 300..460 (`MAIL_Y=300`, `MAIL_H=160`), banner 460..672 (`BANNER_Y=460`, `BANNER_H=212`). Verifica: `300 + 160 + 212 = 672` ✓
- 122c (`SCREEN_H=768`): wallpaper 0..335 (`CINEMA_H=335`), fascia mail 335..556 (`MAIL_Y=335`, `MAIL_H=221`), banner 556..768 (`BANNER_Y=556`, `BANNER_H=212`). Verifica: `335 + 221 + 212 = 768` ✓ Scaling: dei 96 px in più rispetto al 097c, 35 vanno al wallpaper e 61 alla fascia mail; il banner è ancorato al fondo con `BANNER_H` invariato.
- Sidebar destra `x=620..960` (`SIDEBAR_W=340`) sovrappone wallpaper+banda+banner. Calendar mese a `CAL_X=630, CAL_Y=10, CAL_W=320, CAL_H=200` in entrambe le varianti. `CAL_W=320` con `gridW=308` è divisibile esattamente per 7 (44 px/cella) → centratura giorni esatta.

**Spaziatura baseline indoor (4 righe in `INDOOR_RR_W=154`, `BANNER_H=212`, IDENTICA nelle due varianti):**
- Altezza testo visiva FreeSans18pt ≈ 27 px (ascent 22 + descent 5).
- 5 spazi uguali tra rr_top, 4 righe, rr_bottom: `G = (202 - 4*27)/5 = 19 px`.
- Step baseline = `27 + 19 = 46 px`. Costanti `INDOOR_ROWn_BASELINE = BANNER_Y + {46, 92, 138, 184}`. La cascata `BANNER_Y + offset` propaga lo shift +96 sul 122c senza toccare le baseline.

**Aree eventi:**
- 097c: `EVT_H = 230 = EVT_ROW_H * MAX_EVENTS_DISPLAYED = 46 * 5`. Stesso step baseline 46 del banner indoor (uniformità visiva trans-modulo). EVT_ROW_H derivato da `Layout::EVT_H / Calendar::MAX_EVENTS_DISPLAYED` in `Calendar.h` (non costante in Layout perchè dipende dal numero di eventi).
- 122c: `EVT_H = 326 = 230 + 96`. EVT_ROW_H = 326/5 = 65.2 → 65 (troncato int). NB: rompe l'uniformità baseline 46 del banner indoor: se l'estetica è importante, da rivedere a posteriori del primo test fisico sul 122c.

**Cadenze accoppiate non documentate:**
- `DISPLAY_REFRESH_MIN = 5` == BSEC2 ULP sample period (300 s, hardcoded dalla libreria Bosch). Coincidenza calibrata: ogni wake produce esattamente un nuovo sample. Cambiare `DISPLAY_REFRESH_MIN < 5` spreca wake; `> 5` ritarda l'aggiornamento UI ma non perde sample (BSEC tiene il più recente).
- Il daily fetch cinema parte all'ora `CINEMA_DAILY_FETCH_HOUR` locale (default 7, allineato a `WIFI_ACTIVE_HOUR_START`). Vincolo implicito: `CINEMA_DAILY_FETCH_HOUR` DEVE cadere dentro la finestra `[WIFI_ACTIVE_HOUR_START, WIFI_ACTIVE_HOUR_END]`, altrimenti il WiFi è giù all'ora del trigger e il daily refresh non scatta MAI. Le due costanti restano separate per poter spostare l'una senza toccare l'altra.

---
name: Vincoli numerici layout (097c + 122c)
description: Come Layout_common.h deriva la geometria dalle due manopole del pannello, le somme che la tengono in piedi e i vincoli fra font e misure delle zone
type: project
---

Convenzione dimensioni in questo file: `NwxMh` = N px larghezza (X) × M px altezza (Y); costanti `*_W` = larghezza, `*_H` = altezza.

**Da dove vengono i numeri.** `Layout_097c.h` / `Layout_122c.h` danno solo `SCREEN_W/H` (dal driver), `CINEMA_H` (290 / 325) e `MAIL_ROWS_PER_COL` (2 / 3); `Layout_common.h` deriva tutto il resto da quei valori e da sette token: `MARGIN` 10 (dal bordo), `GUTTER` 10 (fra zone affiancate), `STACK_GAP` 16 (fra zone impilate), `BOX_RADIUS` 12, `TITLE_INSET` 16, `BOX_PAD` 12, `TOP_BOX_Y` 14. Vedi [[layout_separation]].

**Perchè STACK_GAP 16 e TOP_BOX_Y 14.** Un titolo in FreeSansBold12pt seduto sul bordo ha la baseline a mezza cap height (8 px) e sporge di circa 9 px sopra il bordo: fra due riquadri impilati servono quindi almeno 16 px, e il riquadro in cima allo schermo ha il bordo a y=14 perchè il titolo resti a 5 px dal bordo.

**Colonne (uguali sui due pannelli, larghezza 960):**
- sinistra x=10..620: wallpaper (`CINEMA_X/Y` = 10, `CINEMA_W` 610) e Posta (`MAIL_W` 610, due colonne da 305);
- destra x=630..950: scheda calendario (`SIDEBAR_W` 320);
- banner: Interno 200 + 10 + Meteo 280 + 10 + Previsioni 440 = 940 = 960 - 2 × 10. Meteo = blocco corrente 162 + colonna sole 118; Previsioni = 3 blocchi da 146.

**Altezze (097c / 122c):**
- banner alto 196, `BANNER_Y` = `SCREEN_H - MARGIN - 196` = 466 / 562; la fascia sbiancata parte da `BANNER_CLEAR_Y` = `BANNER_Y - 10`, per coprire i titoli che sporgono;
- wallpaper y=10..300 / 10..335;
- Posta da `CINEMA_Y + CINEMA_H + 16` a `BANNER_Y - 16`: 316..450 / 351..546, righe da 57 / 58 px (contenuto da +14 a -6 del riquadro);
- scheda calendario y=14..450 / 14..546: griglia del mese 6 righe da 24, filetto a y=207 su entrambi, eventi da y=213 alti 231 / 327.

**Righe minime.** Gli eventi si dividono in 7, 6 o 5 righe, il massimo con righe da almeno 45 px (`MIN_EVT_ROW_H` in `Calendar.h`): 5 righe da 46 sul 097c, 7 da 46 sul 122c. Una riga di Posta deve contenere i 41 px di mittente più oggetto. Entrambi i limiti, e le aree non positive, fermano la build con `static_assert`; la misura del wallpaper no, per scelta dell'utente.

**Font e larghezze che vincolano le zone** (misurate dalle glyph table GFX):
- "Mediocre 500" (caso peggiore di `Indoor::iaqLabel`) misura 139 px in 12pt e 107 in 9pt: il riquadro Interno largo 200 lo tiene in 12pt con il pedice;
- le etichette meteo di `icons_weather.h` e la riga "100% 12.5mm" entrano nei blocchi (154 px utili nel corrente, 138 nelle previsioni) in 12pt o in 9pt, quindi non vengono mai troncate;
- "-12" in 9pt misura 25 px: la colonna delle etichette del mini-grafico (`TC_LABEL_W`) è 28.

**Cadenze accoppiate non documentate:**
- `disp_min` e il periodo BSEC non sono accoppiati: `disp_min` è il **rate limit** del pannello (distanza minima fra due ridisegni, con ridisegno solo su dati nuovi), mentre il campione BSEC ogni 300 s è un task a sé con la propria scadenza. Il risveglio è il minimo delle due, clampato in `[30, 300] s`. Il pavimento reale di `disp_min` è la durata di un refresh, 24 s. Vedi [[timing_chain]].
- Il daily fetch cinema parte all'ora `CINEMA_DAILY_FETCH_HOUR` locale (default 7, allineato a `WIFI_ACTIVE_HOUR_START`). Vincolo implicito: `CINEMA_DAILY_FETCH_HOUR` DEVE cadere dentro la finestra `[WIFI_ACTIVE_HOUR_START, WIFI_ACTIVE_HOUR_END]`, altrimenti il WiFi è giù all'ora del trigger e il daily refresh non scatta MAI. Le due costanti restano separate per poter spostare l'una senza toccare l'altra.

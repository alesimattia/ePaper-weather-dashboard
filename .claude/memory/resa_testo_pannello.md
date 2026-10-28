---
name: Resa del testo sul pannello
description: I font GFX sono a 7 bit e scartano in silenzio ogni byte fuori da 0x20-0x7E; conversione UTF-8 alla ricezione con Graphics::foldToFontCharset (vocale più apostrofo); overflow delle descrizioni meteo gestito con FONT_SMALL e troncamento
type: project
---

**I font sono a 7 bit.** I FreeSans*pt7b e Picopixel coprono solo 0x20..0x7E, e
`Adafruit_GFX::write()` disegna e fa avanzare il cursore solo se il byte cade fra
`first` e `last` del font: i due byte UTF-8 di una lettera accentata spariscono
senza lasciare spazio, quindi "caffè" esce "caff". Il glifo ° non esiste:
`drawTempWithDegree()` lo disegna come cerchietto.

**Conversione alla ricezione.** `Graphics::foldToFontCharset(char*)` riduce sul
posto una stringa UTF-8 ai caratteri disegnabili, subito dopo la copia nei buffer
fissi: titoli degli eventi Outlook e Google, mittente e oggetto Mail, descrizione
OpenWeather (`lang=it`). Regole, scelte dall'utente:
- à á è é ì í ò ó ù ú e le maiuscole diventano vocale più apostrofo (`e'`);
- le altre lettere latine Latin-1 diventano la lettera base (ñ → n, ç → c,
  ä → a, ß → ss, æ → ae);
- virgolette tipografiche e angolari diventano `"`, gli apici `'`, i trattini
  medio e lungo `-`, lo spazio non separabile uno spazio, … diventa `...`, € `EUR`;
- emoji, altri script e sequenze troncate dal buffer vengono scartati.

Nessuna resa supera i byte UTF-8 che sostituisce, quindi il risultato non è mai
più lungo dell'originale e i buffer fissi restano validi.

**Overflow delle descrizioni meteo.** `renderBlock()` riceve la larghezza utile,
calcolata in `drawBanner()`: nel blocco corrente
`2 × (centro - (WEATHER_RR_X + DESC_SIDE_PAD))` (circa 168 px), perchè il blocco
parte a sinistra del riquadro Weather; nelle previsioni
`BLOCK_FC_W - 2 × DESC_SIDE_PAD` (152 px). Oltre, il testo passa a `FONT_SMALL`
e, se ancora non entra, viene troncato con `Calendar::detail::truncateToWidth()`.
Larghezze in FreeSans12pt calcolate dai glifi: "temporale con pioggia leggera"
306 px, "pioviggine leggera" 201, "cielo coperto" 160, "pioggia moderata" 158,
"pioggia leggera" 149. `DESC_SIDE_PAD` sta in entrambi i `Layout_*.h`, che hanno
la stessa geometria dei blocchi (vedi [[layout_separation]]).

**How to apply:** ogni nuovo testo che arriva dalla rete va passato da
`foldToFontCharset()` alla ricezione, non al disegno. Ogni nuovo testo centrato
in uno spazio fisso va misurato con `getTextBounds()` e troncato, come fanno
calendario, mail e blocchi meteo.

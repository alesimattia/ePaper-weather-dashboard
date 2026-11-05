---
name: Resa del testo sul pannello
description: I font GFX sono a 7 bit e scartano in silenzio ogni byte fuori da 0x20-0x7E; conversione UTF-8 alla ricezione con Graphics::foldToFontCharset (vocale più apostrofo); etichette meteo brevi dal codice condizione, passaggio a FONT_SMALL e troncamento che rimisura il ".." 
type: project
---

**I font sono a 7 bit.** I FreeSans*pt7b coprono solo 0x20..0x7E, e
`Adafruit_GFX::write()` disegna e fa avanzare il cursore solo se il byte cade fra
`first` e `last` del font: i due byte UTF-8 di una lettera accentata spariscono
senza lasciare spazio, quindi "caffè" esce "caff". Il glifo ° non esiste:
`drawTempWithDegree()` lo disegna come cerchietto.

**Conversione alla ricezione.** `Graphics::foldToFontCharset(char*)` riduce sul
posto una stringa UTF-8 ai caratteri disegnabili, subito dopo la copia nei buffer
fissi: titoli degli eventi Outlook e Google, mittente e oggetto Mail. Il meteo non
scarica testo: l'etichetta viene dal codice condizione ed è già ASCII (vedi
[[weather_icons]]). Regole, scelte dall'utente:
- à á è é ì í ò ó ù ú e le maiuscole diventano vocale più apostrofo (`e'`);
- le altre lettere latine Latin-1 diventano la lettera base (ñ → n, ç → c,
  ä → a, ß → ss, æ → ae);
- virgolette tipografiche e angolari diventano `"`, gli apici `'`, i trattini
  medio e lungo `-`, lo spazio non separabile uno spazio, … diventa `...`, € `EUR`;
- emoji, altri script e sequenze troncate dal buffer vengono scartati.

Nessuna resa supera i byte UTF-8 che sostituisce, quindi il risultato non è mai
più lungo dell'originale e i buffer fissi restano validi.

**Riga sotto la temperatura dei blocchi meteo.** `renderBlock()` ha
`larghezza del blocco - 2 × WX_DESC_SIDE_PAD` utili: 154 px nel blocco corrente,
138 nelle previsioni. Oltre, il testo passa a `FONT_SMALL` e, se ancora non entra,
viene troncato. Le etichette di `icons_weather.h` e la riga "pop% X.Xmm" sono
scelte e misurate perchè entrino sempre in 12pt o in 9pt: il testo `lang=it` di
OpenWeather non si usa più perchè sforava anche in 9pt ("temporale con pioggia
leggera" 241 px). Una riga "etichetta + mm" invece sforerebbe, per questo il
blocco corrente usa la stessa riga "pop% mm" delle previsioni.

**Troncamento.** `Graphics::truncateToWidth()` accorcia finchè il prefisso con
".." in coda rientra, misurato con i puntini già scritti, e toglie gli spazi in
coda al prefisso. Lo usano eventi, mail e blocchi meteo.

**How to apply:** ogni nuovo testo che arriva dalla rete va passato da
`foldToFontCharset()` alla ricezione, non al disegno. Ogni nuovo testo centrato
in uno spazio fisso va misurato con `getTextBounds()` e troncato, come fanno
calendario, mail e blocchi meteo.

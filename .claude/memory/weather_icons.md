---
name: Icone meteo e etichette dal codice condizione
description: Pipeline icons/weather -> tools/weather_icons.py -> icons_weather.h, regole di colore delle icone Material, tabella codice OpenWeather -> icona ed etichetta, vincoli di larghezza delle etichette
type: project
---

**Pipeline.** `icons/weather/` tiene le icone base (PNG 512x512 a tre colori, ritoccabili), `icons.json` (sorgente del font, regole di colore, tabella delle condizioni) e in `source/` il font Material Symbols Rounded con licenza Apache 2.0. `tools/weather_icons.py extract` rende le icone base dal font, dopo aver verificato lo SHA256; `convert --side 88` le riduce e scrive `icons_weather.h` nella root, separato da `icons.h` per richiesta dell'utente. L'uscita è deterministica. L'utente vuole in futuro un'interfaccia `.pyw` sopra lo script: le funzioni restano indipendenti dalla riga di comando.

**Stile scelto dall'utente.** Material Symbols in variante vuota (Fill 0, peso 400, optical size 48), perchè quella piena è troppo carica sul pannello. Sole, luna, gocce, neve, grandine, fulmini ed eruzione in rosso, nuvola sempre nera. Le regole sono il campo `accent` di ogni icona: `all` (tutta rossa), `none` (tutta nera), `outside_shape` (nera solo la forma che contiene `shape_seed`). Nella variante vuota il contorno del sole si salda a quello della nuvola: lo script assegna ogni pixel alla forma più vicina della variante **piena** del glyph, dove le forme sono staccate da un taglio bianco. Un riempimento per componente connessa o per dilatazione dal buco della nuvola colorava male i punti di saldatura (verificato).

**Tabella delle condizioni.** Copre i 55 codici documentati da OpenWeather. L'icona è la più precisa disponibile: intensità della pioggia, tre icone di neve, nevischio e pioggia mista distinti, 801-802 con il sole visibile, 803 una nuvola, 804 due nuvole; varianti notturne solo dove Material le ha (sereno, nubi, rovesci di neve). Codice sconosciuto o 0 → nuvola ed etichetta `--`.

**Etichette.** Brevi, ASCII stampabile senza accenti (font a 7 bit), scelte dall'utente al posto del testo `lang=it` di OpenWeather, che nei blocchi da 138-154 px sforava anche in 9pt. Ognuna entra nel blocco più stretto in 12pt o in 9pt (verificato con le glyph table). Lo script rifiuta etichette vuote o non ASCII e intervalli sovrapposti.

**Firmware.** `Weather.h` legge `weather[0].id` e il suffisso `n` di `weather[0].icon` per ogni slot (OpenWeather calcola giorno/notte anche per le ore previste), e chiede `WeatherIcons::forCondition()` e `labelFor()`. Uno slot non valido non ha icona. L'icona si disegna con `WeatherIcons::SIDE` centrato nello spazio `Layout::ICON_SIZE`, così un lato rigenerato diverso dà un'icona fuori misura e non un errore. Vedi [[layout_invariants]] e [[resa_testo_pannello]].

**Ambiente.** Pillow sta nel site utente del Python 3.14 di sistema: con `python -E -P` resta raggiungibile, con `python -I` no.

# Icone meteo

Icone base delle condizioni meteo mostrate nel banner, una per file PNG
512x512 a tre colori: bianco, nero (`#000000`) e rosso (`#FF0000`), gli stessi
del pannello. Si possono ritoccare o sostituire con un disegno proprio,
purchè restino in quei tre colori.

`tools/weather_icons.py convert` le riduce al lato del banner e genera
`icons_weather.h`, con un piano nero e un piano rosso per icona e la tabella che
associa i codici condizione di OpenWeather alle icone. `icons_weather.h` non si
modifica a mano.

## Sorgente

Le icone derivano da **Material Symbols Rounded** di Google, nella variante
vuota (asse Fill 0), peso 400, optical size 48, grade 0.

| Voce | Valore |
|---|---|
| Repository | https://github.com/google/material-design-icons |
| Commit | `10ecc14790612ca8fd164e3f4ffe9c16fc50d5b7` |
| File | `variablefont/MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf` |
| SHA256 | `7e74e36533ccee12c2b81e75bfcf3ccf123d3bb7e7e46d65d94445f23db0c97c` |
| Licenza | Apache License 2.0, testo in `source/LICENSE` |

`source/` contiene il font, rinominato `MaterialSymbolsRounded.ttf`, e il
file dei codepoint dello stesso commit. `tools/weather_icons.py extract` rigenera
da lì tutti i PNG, e verifica prima lo SHA256 del font.

## Colori

La colorazione la decide `icons.json`, campo `accent` di ogni icona:

- `all`: tutta rossa. Si usa per sole, luna e precipitazioni senza nuvola.
- `none`: tutta nera. Si usa per nuvole, nebbia, vento, polvere e tornado.
- `outside_shape`: nera solo la forma che contiene `shape_seed` (coordinate 0..1,
  sulla nuvola o sul monte), rossi sole, luna, gocce, neve, grandine, fulmini ed
  eruzione.

Nella variante vuota dei glyph il contorno del sole si salda a quello della
nuvola. Per questo ogni pixel viene assegnato alla forma più vicina della
variante piena, dove le forme sono staccate da un taglio bianco.

## Codici condizione

`conditions` in `icons.json` copre tutti i 54 codici documentati da
OpenWeather (https://openweathermap.org/weather-conditions). Giorno e notte
differiscono solo per sereno (800) e poche nubi (801); il firmware ricava la
notte dal suffisso `n` del campo `icon`. Un codice assente o sconosciuto
ripiega su `fallback`.

# Icone meteo

Icone base delle condizioni meteo mostrate nel banner, una per file PNG
512x512 a tre colori: bianco, nero (`#000000`) e rosso (`#FF0000`), gli stessi
del pannello. Si possono ritoccare o sostituire con un disegno proprio,
purchè restino in quei tre colori.

`tools/weather_icons.py convert` le riduce al lato del banner e genera
`icons_weather.h`, con un piano nero e un piano rosso per icona e la tabella che
associa i codici condizione di OpenWeather alle icone e alle etichette brevi.
`icons_weather.h` non si modifica a mano: si cambia `icons.json` o una PNG e si
rigenera.

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

`conditions` in `icons.json` copre tutti i 55 codici documentati da
OpenWeather (https://openweathermap.org/weather-conditions). Ogni riga dà un
intervallo di codici, l'icona di giorno, quella di notte e l'etichetta mostrata
sotto la temperatura.

L'icona è la più precisa che Material Symbols offre per il codice: pioggia
debole o moderata e pioggia forte hanno icone diverse, la neve ne ha tre per
intensità, nevischio e pioggia mista a neve sono distinti, e 801-802 (sole
ancora visibile), 803 e 804 (coperto) hanno ciascuno la sua. Giorno e notte
differiscono per sereno, poche nubi, nubi sparse e rovesci di neve: per pioggia,
neve e temporale Material non ha varianti notturne. Il firmware ricava la notte
dal suffisso `n` del campo `icon` di OpenWeather.

L'etichetta invece resta breve: ASCII stampabile, senza accenti (i font del
pannello sono a 7 bit), misurata per entrare nel blocco più stretto in 12 pt o
in 9 pt. `weather_icons.py` rifiuta etichette vuote o fuori dall'ASCII e
intervalli sovrapposti.

Un codice assente o sconosciuto, compreso lo 0 di un dato mancante, ripiega su
`fallback` con l'etichetta `--`.

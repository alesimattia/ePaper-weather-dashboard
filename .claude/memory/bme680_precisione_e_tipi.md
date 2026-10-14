---
name: BME680 - precisione delle grandezze e tipi in cui si rappresentano
description: BSEC espone ogni output come float generico; risoluzione e accuratezza da datasheet per T/RH/P/IAQ, quali decimali sono informazione e quali rumore, e i tipi di Indoor::Sample che ne discendono
type: reference
---

## Il float di BSEC non dice niente sulla natura del dato

`bsec_output_t` (`bsec2/src/inc/bsec_datatypes.h`) ha **un solo campo per il
valore**, qualunque sia la grandezza:

```c
int64_t time_stamp;
float   signal;      // temperatura, umidità, pressione, IAQ, gas, CO2 eq...
uint8_t signal_dimensions;
uint8_t sensor_id;   // discrimina quale grandezza sia
uint8_t accuracy;    // 0-3, solo per i virtual sensor che la popolano
```

`bsecData` è un alias di questa struct. Quindi "nel codice è float" descrive la
firma di un'API generica, non il dato: quattro grandezze su cinque sono float
solo perchè BSEC non ha altro campo da offrire. L'unico tipo che porta
informazione è `accuracy`, dichiarato `uint8_t` e documentato come enumerazione
0..3 nella struct stessa.

## Risoluzione e accuratezza, da datasheet BME680

| Grandezza | Risoluzione di uscita | Accuratezza |
|---|---|---|
| Temperatura | 0,01 °C | ±0,5 °C a 25 °C; ±1,0 °C su 0-65 °C |
| Umidità | 0,008 %RH (rumore 0,01 %RH) | ±3 %RH fra 20 e 80 %RH a 25 °C |
| Pressione | 0,18 Pa, cioè 0,0018 hPa | relativa ±0,12 hPa |
| IAQ (BSEC) | 1 punto su scala 0-500 | la dichiara `iaqAccuracy`, non una tolleranza |

L'accuratezza **assoluta** della pressione non compare nel datasheet Bosch
consultato; fonti terze citano ±1 hPa, quindi va presa con cautela. Quella
relativa basta comunque a decidere, perchè la pressione atmosferica si legge
per tendenza.

## Quali decimali sono informazione

- **Temperatura, sì.** La risoluzione sta due ordini di grandezza sotto
  l'accuratezza assoluta, ma la ripetibilità è molto migliore di quest'ultima:
  il primo decimale non dà il valore vero con quella precisione, però rende
  visibili le variazioni reali, che in un ambiente chiuso è quello che si
  guarda.
- **Pressione, sì.** BSEC emette Pa e `Indoor.h` divide per 100, quindi i
  decimali in hPa sono misura e non cifre inventate; il primo sta due ordini
  sopra la risoluzione e all'ordine dell'accuratezza relativa.
- **Umidità, marginale.** Il sensore risolve il centesimo di punto percentuale
  ma il valore vero può stare tre punti più in là: fra 47,3 % e 47 % non c'è
  differenza misurabile, resta utile solo per la deriva.
- **IAQ, no.** Non è una misura fisica ma l'uscita di un classificatore, con
  scala 0-500 e risoluzione 1; l'incertezza la dichiara `iaqAccuracy`. Il float
  è un artefatto dell'API.

## I tipi di Indoor::Sample

```c
float    temperature;   // unico con decimale significativo
uint8_t  humidity;      // 0-100, arrotondata
uint16_t pressure;      // mBar, numericamente identici agli hPa
uint16_t iaq;           // 0-500
uint8_t  iaqAccuracy;   // 0-3
```

La struct misura **20 byte** (verificato con `static_assert`), contro i 28 che
occuperebbe con quattro float. Il guadagno di memoria è simbolico, perchè
l'istanza è una sola: la ragione dei tipi interi è che descrivono il dato.

Due conseguenze pratiche:

- la conversione a intero avviene nella callback BSEC e passa da un helper che
  **scarta il NaN**, perchè `lroundf()` su NaN ha comportamento indefinito e
  dentro un campo intero un valore arbitrario non è più distinguibile da una
  misura vera, mentre in un float resterebbe riconoscibile;
- **1 hPa = 1 mBar**, quindi la conversione da Pa è una sola divisione e
  l'unità mostrata è una scelta di etichetta, non di calcolo.

Correlate: [[bsec_modi_campionamento]] per come BSEC viene pilotato nel tempo,
[[tuya_module]] per la mappatura di questi tipi sul device model Tuya.

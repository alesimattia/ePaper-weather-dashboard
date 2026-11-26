---
name: Sensore Indoor (BME680 o BME280) - selezione, precisione e tipi
description: un #define in Indoor.h sceglie BME680 (BSEC) o BME280 (Adafruit forced); cosa si spegne col BME280 e come; guasto I2C silenzioso di Adafruit, controllo di range con pressione NaN scartata e ctrl_hum riletto a ogni poll per riconoscere un reset del chip; scadenza del campione a un periodo e mezzo; BSEC espone ogni output come float generico; risoluzione e accuratezza da datasheet, i tipi di Indoor::Sample (campi IAQ solo BME680) e le riduzioni solo al display
type: reference
---

## Selezione del sensore (Indoor.h)

- `#define INDOOR_SENSOR_BME280` oppure `INDOOR_SENSOR_BME680` in testa a `Indoor.h`;
  zero o due → `#error`. I consumatori (`Weather.h`, `Tuya.h`, `.ino`, test stub di
  `test_tuya.cpp`) testano direttamente `#ifdef INDOOR_SENSOR_BME680`; nessuna macro di
  capacità. `tools/layout_preview.py` (`read_indoor_has_iaq()`) legge il define da
  `Indoor.h` senza commenti.
- Macro di cablaggio comuni: `INDOOR_I2C_ADDR` 0x77, `INDOOR_SDA_PIN` 21,
  `INDOOR_SCL_PIN` 16, `INDOOR_I2C_CLOCK_HZ` 400 kHz; tag di log `INDOOR_TAG`.
- `#include <bsec2.h>` dentro `#ifdef`: arduino-cli scopre le librerie con `g++ -E`
  (prova in `.build/includes.cache`, voci `missing_include_h`), quindi col BME280
  bsec2 e BME68x non entrano nella build. Misurato sul Mac: BME280 1476708 B flash /
  78752 B DRAM, BME680 1526968 / 83504 (−50 KB flash, −4,7 KB DRAM).
- Col BME280: campi `iaq`/`iaqAccuracy` di `Sample` assenti (uso non protetto = errore di
  compilazione); riquadro con pressione in riga 3 (con entrambi i sensori) e riga 4
  vuota (IAQ in riga 4 solo BME680); Tuya a 3 property; nessuna NVS; niente
  `msUntilNextPoll()`, quindi l'hook `msUntilSensorPoll` non si registra e lo scheduler
  tiene il ciclo interno e non sveglia di notte, vedi [[scheduler_task_table]].
- Restano senza `#ifdef` perchè inutilizzati non costano: `shownIaq()`, `iaqLabel()`,
  `INDOOR_ICON_AIR_QUALITY`, le costanti `BSEC_*` di `Timings.h` (da cui deriva `CYCLE_S`).
- Diagnostica `I2C_DEBUG` comune ai due: indirizzi 0x76/0x77 e registro chip ID 0xD0
  uguali; per sensore `CHIP_ID` (0x61 / 0x60), `CALIB_REG`/`CALIB_LEN` (BME68X_COEFF1 23 B /
  0x88 24 B = trim dig_T1..dig_P9). Il log distingue BMP280 (0x58), spesso venduto
  come BME280.

## BME280 con Adafruit BME280 2.3.0

- `bme.begin(addr, &Wire)` dopo `Wire.begin(SDA, SCL)`: il `Wire.begin()` interno di BusIO
  a bus avviato (core 3.3.12) fa solo `log_w` e non tocca pin nè clock. Poi
  `applySampling()`, che imposta `MODE_FORCED`, X1 su temperatura, pressione e umidità e
  `FILTER_OFF` (enum membri della classe, vanno qualificati `Adafruit_BME280::`); il
  default della libreria è normal x16 continuo.
- **Guasto I2C silenzioso**: `read8/read16/read24` ignorano l'esito di
  `write_then_read` e restituiscono il buffer non riempito (byte 0 = indirizzo del
  registro). `takeForcedMeasurement()` ritorna true a bus morto (stato letto 0xF3, bit
  0x08 a zero). La temperatura che ne esce è ~+180 °C: `poll()` scarta letture fuori
  da −40..85 °C o 300..1100 hPa e chiama `expireIfStale()`. La pressione si scrive come
  negazione dell'intervallo valido, così anche un NaN viene scartato (ogni confronto con NaN è
  falso). Umidità già limitata 0..100.
- **Reset del chip da sè**: la libreria Adafruit non rilegge mai il registro `ctrl_hum`
  (0xF2) e tiene la configurazione in una copia software. Un reset del sensore riporta a zero
  il sovracampionamento dell'umidità (umidità saltata), ma la libreria dà NaN sull'umidità solo
  per la propria configurazione, quindi non se ne accorge e il chip risponde con un valore
  plausibile ma falso. `poll()` rilegge `ctrl_hum` via Wire a ogni poll, all'indirizzo con cui
  il sensore è stato avviato: una lettura I2C fallita scarta la lettura; i bit 2..0 diversi da
  X1 (reset avvenuto) riapplicano il campionamento con `applySampling()` e scartano la lettura,
  che riparte al ciclo dopo. In entrambi i casi chiama `expireIfStale()`.
- Forced X1: misura ~10 ms, nessun autoriscaldamento a 1/300 Hz, temperatura non
  compensata. Datasheet: T ±0,5 °C a 25 °C, RH ±3 %RH, P assoluta ±1 hPa e relativa
  ±0,12 hPa; risoluzione a x1: T 0,005 °C, P 2,62 Pa.
- Col ciclo interno le letture distano esattamente `CYCLE_S`: `INDOOR_SAMPLE_MAX_AGE_MS`
  (un periodo e mezzo, 450 s) tollera una lettura persa e fa scadere il campione alla seconda,
  come col BME680, il cui poll cade a un periodo più il margine.
- Rischio residuo noto: se il bit "measuring" non è ancora alto quando
  `takeForcedMeasurement()` lo legge, si leggono i dati della misura precedente (al più un
  ciclo di ritardo, mai un valore inventato).

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
uint16_t iaq;           // 0-500, solo con INDOOR_SENSOR_BME680
uint8_t  iaqAccuracy;   // 0-3,   solo con INDOOR_SENSOR_BME680
```

La struct misura **20 byte** col BME680 (verificato con `static_assert`), 16 col BME280, contro i 28 che
occuperebbe con quattro float. Il guadagno di memoria è simbolico, perchè
l'istanza è una sola. I tipi interi fissano la risoluzione che serve (1 %RH, 1 mBar,
1 punto IAQ), non quella che il sensore supporta: per umidità e pressione scartano
cifre che sarebbero misura.

Due conseguenze pratiche:

- la conversione a intero avviene in `applyOutputs()`, sulle uscite lette con
  `getOutputs()` dopo `run()`, e passa da un helper che
  **scarta il NaN**, perchè `lroundf()` su NaN ha comportamento indefinito e
  dentro un campo intero un valore arbitrario non è più distinguibile da una
  misura vera, mentre in un float resterebbe riconoscibile;
- **1 hPa = 1 mBar**, quindi la conversione da Pa è una sola divisione e
  l'unità mostrata è una scelta di etichetta, non di calcolo.

**Perchè questi tipi sono i minimi.** La pressione sta in 16 bit perchè il campo di
misura del BME680 è 300-1100 hPa, cioè 800 valori a passo di 1 mBar, che non entrano in 8
bit; un `uint8_t` con offset coprirebbe solo 256 mBar attorno a un valore fisso e
dipenderebbe da altitudine e meteo. L'IAQ sta in 16 bit perchè 500 non entra in 8 (a 8 bit
darebbe passi da 2). L'umidità 0-100 e l'accuratezza 0-3 stanno in 8.

## Cosa si mostra sul display

Il campione resta ai tipi sopra, e il display lo arrotonda ulteriormente, solo nel disegno
(`Indoor::shownTemperature()`, `shownPressure()`, `shownIaq()`): temperatura 0,5 °C
(lo zero esce sempre positivo: fra -0,25 e 0 °C `roundf()` darebbe -0.0, che `%.1f` stampa
come "-0.0"), umidità intera senza riduzione, pressione 2 mBar, IAQ 10 con l'etichetta calcolata sul
valore arrotondato, perchè le soglie 50, 100, 150, 200, 250 e 350 sono multiple di 10. Lo
scopo è che le oscillazioni sotto la risoluzione utile non cambino il frame, vedi
[[frame_skip_unchanged]]. Il README ha la tabella con risoluzione massima, accuratezza,
tipo memorizzato, risoluzione memorizzata e valore mostrato per le grandezze del BME680 e
per quelle di OpenWeather.

Correlate: [[bsec_modi_campionamento]] per come BSEC viene pilotato nel tempo,
[[tuya_module]] per la mappatura di questi tipi sul device model Tuya.

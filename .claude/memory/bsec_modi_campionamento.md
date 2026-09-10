---
name: BSEC2 - modi di campionamento, warning positivi, e perche' la misura on-demand non si usa
description: 300 s e' un modo non un intervallo; run() ritorna true quando non c'e' niente da leggere; perche' ULP plus e' stata valutata e scartata
type: reference
---

**I 300 s del BME680 non sono un intervallo configurabile: sono un modo.** `bsec.updateSubscription(..., BSEC_SAMPLE_RATE_ULP)` in `Indoor.h` sottoscrive gli output a 1/300 Hz. Gli altri modi (`bsec_datatypes.h`) sono `LP` 3 s, `CONT` 1 s, `SCAN` 18 s, `DISABLED`. Non esiste un valore intermedio, e non ha senso esporre la cadenza del sensore fra i tempi configurabili: LP consuma ~10× e impone un polling ogni 3 s, incompatibile col light sleep di questo firmware.

## Due comportamenti del wrapper che non si vedono dall'API

1. **`Bsec2::run()` ritorna `true` anche quando non c'e' niente da leggere.** Il corpo è tutto dentro `if (currTimeNs >= bmeConf.next_call)` e la funzione cade sul `return true` finale (`bsec2.cpp:146-209`); ritorna `false` solo su errore vero. Quindi "nessun dato" e "tutto bene" sono indistinguibili dal valore di ritorno: chi deve sapere se è arrivato un campione guarda `hasNewSample`, non `run()`.
2. **`bmeConf` è privato** (`bsec2.h:239`) e gatea `run()`. Nessuna API pubblica permette di sapere quando cadrà il prossimo campione: per questo `scadenzaIndoor()` nel `.ino` la deriva da `Indoor::sample().lastUpdateMs + 300 s`, che è l'unica informazione disponibile.

## I warning di BSEC sono positivi, e vanno filtrati con `!=`

`Indoor::refresh()` logga quando `bsec.status != BSEC_OK`, **non** `< BSEC_OK`. I codici positivi sono warning — fra cui `BSEC_W_CALL_TIMING_VIOLATION` (100), che segnala un poll fuori tempo — e con un filtro sui soli negativi resterebbero invisibili. `detail::descriviStatoBsec()` li traduce in testo, altrimenti sul seriale comparirebbe un numero muto.

## Perche' la misura on-demand ("ULP plus") non e' implementata

`BSEC_SAMPLE_RATE_ULP_MEASUREMENT_ON_DEMAND` (0.0f) chiederebbe un campione extra fuori cadenza, per mostrare un valore fresco invece di uno vecchio fino a 5 minuti. È stata valutata a fondo e **scartata**. Non reintrodurla senza aver risolto tutti e quattro i punti:

1. **Il wrapper non la espone in modo utilizzabile.** `updateSubscription()` non tocca `bmeConf`, quindi la richiesta resterebbe in coda fino allo slot ULP regolare — cioè non servirebbe a niente. L'unico modo per farla rivalutare subito è un giro `getState()`/`setState()`, perché `setState` fa `memset(&bmeConf, 0)` (`bsec2.cpp:236`). È un effetto collaterale non documentato, usato come meccanismo.
2. **Il verdetto non arriva dalla richiesta** ma dal `run()` successivo, che in caso di rifiuto ritorna `false` con `status` 101 (`MODEXCEEDULPTIMELIMIT`: una misura ULP è appena avvenuta o sta per avvenire) o 102 (`MODINSUFFICIENTWAITTIME`: troppo presto dalla precedente extra).
3. **Bosch non documenta le finestre temporali** di quei rifiuti: né il README della libreria, né l'esempio ufficiale `basic_config_state_ulp_plus` (che la innesca da un pulsante), né i thread del forum danno numeri. L'esempio dice solo *"It will be rejected if the sensor is not already in ULP mode, or if the time difference between requests is too short"*.
4. **Non compone bene con uno scheduler periodico.** Il task `bsec` ha una scadenza calcolata su `lastUpdateMs + 300 s`, mentre una misura extra produce un campione **fuori scadenza**: senza un flag che leghi le due cose il task non risulterebbe dovuto nel giro in cui la misura è stata chiesta, e il campione verrebbe raccolto fino a 5 minuti dopo. E un flag del genere ha bisogno del proprio tetto, perché "nessuna risposta" non è osservabile (punto 1 della sezione precedente): senza scadenza il sensore resterebbe dovuto a ogni giro e il dispositivo si sveglierebbe a `SLEEP_MIN_S` invece che ogni 300 s, dieci volte più spesso.

Il beneficio, anche funzionando tutto, sarebbe stato "a volte più fresco" e non "sempre fresco": se BSEC ha bisogno di più di un `run()` per il ciclo dello heater, il campione arriva comunque al giro dopo. Sproporzionato rispetto al rischio.

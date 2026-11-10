---
name: BSEC2 - modi di campionamento, warning positivi, e perchè la misura on-demand non si usa
description: 300 s è un modo non un intervallo; tolleranza del 6,25% sul poll (guida AN011) e conseguenze su sleep e notte; configurazione ULP esplicita e salvataggio dello stato ad accuratezza 3; taratura di fondo; run() ritorna true quando non c'è niente da leggere, quindi le uscite si leggono con getOutputs() e il campione nuovo dal time_stamp; callback del wrapper non usata; perchè ULP plus e BSEC3 sono scartati
type: reference
---

**I 300 s del BME680 non sono un intervallo configurabile: sono un modo.** `bsec.updateSubscription(..., BSEC_SAMPLE_RATE_ULP)` in `Indoor.h` sottoscrive gli output a 1/300 Hz. Gli altri modi (`bsec_datatypes.h`) sono `LP` 3 s, `CONT` 1 s, `SCAN` 18 s, `DISABLED`. Non esiste un valore intermedio, e non ha senso esporre la cadenza del sensore fra i tempi configurabili: LP consuma ~10× e impone un polling ogni 3 s, incompatibile col light sleep di questo firmware.

## Due comportamenti del wrapper che non si vedono dall'API

1. **`Bsec2::run()` ritorna `true` anche quando non c'è niente da leggere.** Il corpo è tutto dentro `if (currTimeNs >= bmeConf.next_call)` e la funzione cade sul `return true` finale (`bsec2.cpp:146-209`); ritorna `false` solo su errore vero. Quindi "nessun dato" e "tutto bene" sono indistinguibili dal valore di ritorno, e le uscite restano nella libreria fra un `run()` e l'altro: `Indoor::refresh()` le legge con `getOutputs()` (puntatore costante, `nullptr` senza uscite) e riconosce un campione nuovo dal `time_stamp` in ns delle uscite, diverso dall'ultimo applicato (`lastOutputTimestampNs`).
3. **La callback del wrapper copia tutto per valore.** La firma imposta da `bsec2.h:80` riceve `bsecOutputs` (23 uscite da 16 B) e l'intero `Bsec2` per valore, e la copia la fa la libreria al punto di chiamata (`bsec2.cpp:379`): circa 1 KB di stack per campione, più una copia superficiale del puntatore `bsecInstance` allocato con `new[]`, innocua solo perchè né `Bsec2` né `Bme68x` hanno distruttore. Per questo `Indoor.h` non registra `attachCallback()`, che il wrapper tratta come facoltativa (`bsec2.cpp:378` controlla il `nullptr`), e la libreria non si modifica.
2. **`bmeConf` è privato** (`bsec2.h:239`) e gatea `run()`: prima di `next_call` la chiamata non fa nulla, e al primo `run()` dopo il boot `next_call` vale 0, quindi la misura parte subito. Il trigger e la lettura avvengono nella stessa chiamata: `bme68x_get_data()` attende il dato fino a 5 × 10 ms (`bme68x.c`).

## Cadenza delle interrogazioni e scadenza del campione

**La cadenza si conta dall'ultima interrogazione, non dal campione.** `Indoor::refresh()` registra l'istante di ogni chiamata e `Indoor::msUntilNextPoll()` restituisce 0 se il sensore non è mai stato interrogato, altrimenti il tempo a ultima chiamata + 300 s + `BSEC_MARGINE_POLL_MS`. È la scadenza custom del task `bsec`. Interrogare più spesso non anticipa nessun campione, per il punto 2; contare dal campione invece bloccherebbe il task quando il campione non c'è, perchè nello scheduler la scadenza custom precede `neverRun` (vedi [[scheduler_task_table]]), e farebbe svegliare il dispositivo a ogni giro con il sensore guasto.

**Il campione scade al secondo campione perso.** Se una chiamata non porta un campione nuovo e l'ultimo è più vecchio di `INDOOR_SAMPLE_MAX_AGE_MS` (2 × 300 s + 1 s), `valid` torna falso: il riquadro mostra "--" e Tuya si sospende, perchè `readyToPublish()` richiede `valid`. `refresh()` restituisce `true` anche in quel caso, perchè il dato da mostrare è cambiato e serve un ridisegno.

**Senza sensore** il task è sospeso: il suo `preconditionsMet` è `Indoor::attivo()`, falso se `bsec.begin()` è fallito.

## I warning di BSEC sono positivi, e vanno filtrati con `!=`

`Indoor::refresh()` logga quando `bsec.status != BSEC_OK`, **non** `< BSEC_OK`. I codici positivi sono warning — fra cui `BSEC_W_CALL_TIMING_VIOLATION` (100), che segnala un poll fuori tempo — e con un filtro sui soli negativi resterebbero invisibili. `detail::descriviStatoBsec()` li traduce in testo, altrimenti sul seriale comparirebbe un numero muto.

## Perchè la misura on-demand ("ULP plus") non è implementata

`BSEC_SAMPLE_RATE_ULP_MEASUREMENT_ON_DEMAND` (0.0f) chiederebbe un campione extra fuori cadenza, per mostrare un valore fresco invece di uno vecchio fino a 5 minuti. È stata valutata a fondo e **scartata**. Non reintrodurla senza aver risolto tutti e quattro i punti:

1. **Il wrapper non la espone in modo utilizzabile.** `updateSubscription()` non tocca `bmeConf`, quindi la richiesta resterebbe in coda fino allo slot ULP regolare — cioè non servirebbe a niente. L'unico modo per farla rivalutare subito è un giro `getState()`/`setState()`, perché `setState` fa `memset(&bmeConf, 0)` (`bsec2.cpp:236`). È un effetto collaterale non documentato, usato come meccanismo.
2. **Il verdetto non arriva dalla richiesta** ma dal `run()` successivo, che in caso di rifiuto ritorna `false` con `status` 101 (`MODEXCEEDULPTIMELIMIT`: una misura ULP è appena avvenuta o sta per avvenire) o 102 (`MODINSUFFICIENTWAITTIME`: troppo presto dalla precedente extra).
3. **Bosch non documenta le finestre temporali** di quei rifiuti: né il README della libreria, né l'esempio ufficiale `basic_config_state_ulp_plus` (che la innesca da un pulsante), né i thread del forum danno numeri. L'esempio dice solo *"It will be rejected if the sensor is not already in ULP mode, or if the time difference between requests is too short"*.
4. **Non compone bene con uno scheduler periodico.** Il task `bsec` ha una scadenza calcolata sull'ultima interrogazione + 300 s, mentre una misura extra produce un campione **fuori scadenza**: senza un flag che leghi le due cose il task non risulterebbe dovuto nel giro in cui la misura è stata chiesta, e il campione verrebbe raccolto fino a 5 minuti dopo. E un flag del genere ha bisogno del proprio tetto, perchè "nessuna risposta" non è osservabile (punto 1 della sezione precedente): senza scadenza il sensore resterebbe dovuto a ogni giro e il dispositivo si sveglierebbe a `SLEEP_MIN_S` invece che ogni 300 s, dieci volte più spesso.

Il beneficio, anche funzionando tutto, sarebbe stato "a volte più fresco" e non "sempre fresco": se BSEC ha bisogno di più di un `run()` per il ciclo dello heater, il campione arriva comunque al giro dopo. Sproporzionato rispetto al rischio.

## Tolleranza del poll e conseguenze sul firmware

**La guida Bosch `BST-BME-Integration-Guide-AN011-321000` (2025, BSEC 3.x, copre il BME680)** fissa al **6,25%** sia il warning `BSEC_W_SC_CALL_TIMING_VIOLATION` (§3.3.4.1, "a maximum jitter of 6.25% is allowed") sia la soglia oltre cui l'accuratezza IAQ resta a 0 (§3.2): ±18,75 s in ULP. La vecchia AN008-47 (2018, BSEC 1.x) metteva la seconda soglia al 50%. La guida di BSEC 2.x (AN011-61) sta solo nel pacchetto Bosch dietro licenza; una trascrizione della AN011-321000 è in `github.com/sonu27/polverine`, `docs/bsec/integration-guide.md`. Conseguenze, tutte nello scheduler ([[scheduler_task_table]]):
- il sensore è il primo passo del ciclo e la sua scadenza è l'orologio del firmware; lo sleep dura fino a lei esattamente, senza pavimento;
- lo scheduler lo interroga anche fra un fetch e l'altro, prima del frame e con un client sulle pagine di manutenzione;
- di notte restano i soli risvegli del sensore: un buco di ore torna ad accuratezza 0 (stabilizzazione ~20 min in ULP), `setState()` serve a un riavvio e non a saltare misure, e non esiste un modo più lento dei 300 s.

Non verificabile: se `next_call` si calcola dall'istante della chiamata o su una griglia fissa (libreria binaria, `bmeConf` privato senza getter). Con una griglia fissa il secondo di `BSEC_MARGINE_POLL_MS` si accumulerebbe e dopo ~19 cicli comparirebbe il warning 100; la correzione sarebbe contare la scadenza dal `time_stamp` dell'ultima uscita.

## Configurazione e stato

- `Indoor::begin()` carica con `setConfig()` il blob `bme680_iaq_33v_300s_4d` della libreria (breakout alimentata a 3,3 V, ULP, storia di 4 giorni) **prima** di `restoreBsecState()`, come l'esempio Bosch `basic_config_state`. Il blob (1943 byte, `BSEC_MAX_PROPERTY_BLOB_SIZE`) sta a livello di namespace, `inline const`, non sullo stack di `begin()`. Senza `setConfig()` BSEC gira con una configurazione di default non dichiarata.
- Uno stato salvato con un'altra configurazione viene rifiutato da `setState()` e la taratura riparte una volta.
- Lo stato si salva la prima volta che l'accuratezza IAQ arriva a 3 nel boot (flag `savedAtFullAccuracy`), poi ogni `BSEC_STATE_SAVE_INTERVAL_MS` con accuratezza almeno 1. L'esempio Bosch invece salva al primo campione qualunque sia l'accuratezza, poi ogni 360 minuti.
- **Taratura di fondo** automatica: accuratezza 0 = stabilizzazione o run-in, 1 = esporre una volta ad aria buona (esterna) e cattiva (scatola con aria espirata), 2 = auto-trimming, 3 = tarato; circa 30 minuti per tipo d'aria (AN011 §5.3.2.1), di più in ULP dove 30 minuti sono 6 campioni. Un erase della flash cancella il namespace `bme680` e la taratura riparte.

**BSEC3 scartato per il BME680**: Bosch offre per questo sensore BSEC 2.6.1.0 (quella in uso, libreria bsec2 1.10.2610); BSEC 3.3.0.1 è pubblicato per BME688 e BME690, non ha un wrapper Arduino ufficiale (l'unico port, `mauringo/Bsec3-ArduinoLibrary-Unofficial`, supporta solo il BME690), cambia API (AN011 §2.3) e non cambia i vincoli dell'ULP. La licenza BSEC copre BME680, BME688 e BME690.

Correlate: [[bme680_precisione_e_tipi]] per il dato che questi campioni
producono, la precisione di ogni grandezza e i tipi in cui viene rappresentato.

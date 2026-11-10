---
name: Scheduler a ciclo del sensore
description: Scheduler.h possiede tutto il timing attorno a un solo orologio, il ciclo BSEC da ~301 s: passi sensore, rete, frame; cadenze contate in cicli con slot ancorato; tabella dei soli task di rete nel .ino e hook per radio, sensore e frame; notte con il solo sensore; regole di sleep e di manutenzione
metadata:
  type: project
---

`Scheduler.h` possiede **tutto** il timing. I moduli espongono solo un `runFetch()` che fa I/O e dice com'è andata; sensore, radio e pannello li usa attraverso lo struct `Scheduler::Hooks` passato a `begin(table, count, hooks)`: `radioOn`, `radioOff`, `sensorActive`, `msUntilSensorPoll`, `pollSensor`, `drawFrame`.

**Il ciclo.** Un solo orologio: la scadenza del sensore (`Indoor::msUntilNextPoll()`, ultima chiamata + 300 s + `BSEC_MARGINE_POLL_MS`), perchè BSEC tollera solo il 6,25% di scarto e non si lascia anticipare (vedi [[bsec_modi_campionamento]]). Senza sensore il ciclo è interno, `CYCLE_S` dall'inizio del precedente. Ogni ciclo, in quest'ordine: 1) `pollSensor`; 2) rete: una sola accensione della radio per i task dovuti, prima tutti gli `onRoundStart()` poi i `run()` in ordine di tabella; 3) frame, sempre, a ogni ciclo dentro la fascia WiFi. Nessun flag di dati nuovi: il frame legge da sè i dati, badge Tuya compreso.

**Cadenze in cicli.** `intervalMinutes` è multiplo di `CYCLE_MIN` (vincolo del campo `step` di `Timings::Campo`, vedi [[timings_nvs_config]]); un periodico è dovuto quando `cycle - slotCycle >= interval / CYCLE_MIN`. Al più un tentativo per ciclo (`attemptCycle`); il ritento cade al ciclo successivo e lo slot si chiude con `slotCycle = openCycle`, il ciclo in cui si era aperto, così la cadenza non cambia fase. DAILY: orologio valido, ora >= target, giorno diverso dall'ultimo slot e distanza minima `GIORNALIERA_DISTANZA_MIN_MS` in ms, perchè senza sensore di notte i cicli si fermano. Il primo ciclo di un nuovo giorno dentro la fascia rimette `neverRun` su tutti i PERIODIC, mai sui DAILY.

**Tabella** (`TASK_TABLE` nel `.ino`, solo task di rete, ordine = ordine di esecuzione): meteo, mail (prima di Google: cache del token OAuth condivisa), google, outlook (sospesi senza orologio), tuya (`maxAttempts` 0, pubblica il campione appena letto), route (`routeReady()` richiede `isRouteSourceActive() && isInsideRouteHours()` e rimette `neverRun` all'ingresso nella fascia, vedi [[routefinder_morning_route]]), cinema DAILY (ultimo: copre il cold start di render). Ping di sveglia come `onRoundStart` di route e cinema.

**La scadenza del sensore ha la precedenza.** `sleepUntilNextEvent()` dorme esattamente fino a `msUntilNextCycle()`, senza pavimento (con 0 non dorme); `SLEEP_MAX_S` (24 h) è solo una guardia. Fra un task e l'altro e prima del frame `serviceSensorDeadline()` apre il ciclo successivo se la scadenza è passata. Il vecchio clamp [30, 300] s faceva interrogare il sensore ogni ~330 s, fuori dalla tolleranza di BSEC.

**Notte.** Fuori dalla fascia WiFi (orologio valido) il ciclo fa solo il passo del sensore: niente radio, frame, Tuya o SNTP. Senza sensore si dorme fino a `wifi_h_ini`:00 con `msUntilLocalHour()`. `isInsideRadioHours()` è fail-open a orologio non valido. In modalità scheduler l'ora si rilegge dopo `radioOn()`: se cade fuori fascia, nessun fetch.

**Finestra di manutenzione** (`RadioMode::MANAGED_BY_MAINTENANCE`): la radio si usa, non si tocca; giro al più ogni `MAINTENANCE_SCHEDULER_PERIOD_MS`; i fetch aspettano la STA e, con orologio non valido, l'ora per al più `PRIMO_FRAME_ATTESA_MS` dal boot; il frame del ciclo aspetta i fetch dovuti per al più `PRIMO_FRAME_ATTESA_MS` dall'inizio del ciclo. Con `clientAttivo()` il `.ino` chiama `Scheduler::serviceSensor()`, che fa solo il passo del sensore. Al boot con orologio valido fuori fascia `setup()` non chiama `Maintenance::begin()`.

**Diagnostica.** `printStatus()` (pagina `/`): ciclo, prossimo evento, per ogni task cadenza in minuti e cicli, cicli mancanti o "sospeso"/"ora"/"ritento". Ogni esito è loggato con durata e prossimo slot.

**Aggiungere un task di rete** = un campo in `Timings::Valori`, una riga in `CAMPI` con `step` `CYCLE_MIN`, una riga di tabella; lo scheduler non si tocca.

**Verificato per esecuzione**: `test/test_scheduler.cpp` con sensore, radio e pannello finti che registrano la sequenza dei passi; copre ordine dei passi, alternanza di una cadenza da 2 cicli per 20 cicli con fetch di durata variabile, sleep esatto senza pavimento, fase di rete più lunga del ciclo, ritento con slot ancorato, radio giù, notte con e senza sensore anche attraverso il DST, primo ciclo del mattino, giornaliero senza doppioni con orologio non valido, manutenzione con STA in ritardo o assente e client attivo, ciclo interno, rollover di `millis()`.

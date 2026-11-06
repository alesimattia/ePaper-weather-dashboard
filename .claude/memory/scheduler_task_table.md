---
name: Scheduler centralizzato dei flussi temporizzati
description: Scheduler.h possiede tutto il timing; nomi degli hook di Task, struttura della tabella, le due fasi del giro, coalescing, proprietà della radio, giro di manutenzione al più ogni secondo, ora riletta dopo l'accensione della radio
type: project
---

`Scheduler.h` possiede **tutto** il timing: intervallo, istante dell'ultimo slot, contatore dei fallimenti e politica di ritento. I moduli espongono solo un `runFetch()` che fa I/O e dice com'è andata: nessuno di loro sa quando tocca a lui.

**Vocabolario.** `Scheduler::Task` ha gli hook `run()` (esegue, torna `Outcome {OK, FAILED, SKIPPED}`), `preconditionsMet()` (precondizione non temporale), `hasIncompleteData()`, `onRoundStart()` (a inizio giro per tutti i dovuti), `onOutcome(Outcome)`, `customMsUntilDue()`; i campi `cadence` (`Cadence::PERIODIC`/`DAILY`), `needsRadio`, `runsOutsideRadioHours`, `intervalMinutes`, `dailyLocalHour`, `retryIntervalMs`, `maxAttempts`; lo stato `state` con `neverRun`, `lastSlotMs`, `retrying`, `nextAttemptMs`, `failedAttempts`, `lastSlotDayOfYear`. API: `runRound(RadioMode)`, `sleepUntilNextEvent()`, `isDue()`, `msUntilDue()`, `msUntilDueWithRadioLimits()`, `msUntilLocalHour()`, `recordOutcome()`, `isInsideRadioHours()`, `isFirstFrameAllowed()`, `printStatus()`, `NEVER`. Gli adattatori del `.ino` seguono lo stesso schema (`runWeatherFetch()`, `tuyaReady()`, `onIndoorOutcome()`, `markDirtyOnOk()`...).

**La tabella sta nel `.ino`** (`TASK_TABLE`), non in `Scheduler.h`, perché nomina anche il cinema, che è fatto di statici dello sketch. L'ordine delle righe **è** l'ordine di esecuzione:

| # | task | radio | cadenza | perché lì |
|---|---|---|---|---|
| 0 | meteo | sì | `owm_min` | `preconditionsMet` = `weatherReady()`; `hasIncompleteData` = corrente senza previsioni ⇒ ritenta invece di chiudere lo slot |
| 1 | mail | sì | `mail_min` | `preconditionsMet` = `mailReady()`; prima di Google: condividono la cache del token OAuth, chi gira per primo paga il refresh |
| 2 | google | sì | `goog_min` | `preconditionsMet` = `googleReady()` (orologio + credenziali); il token è quello appena rinfrescato da mail |
| 3 | outlook | sì | `outl_min` | `preconditionsMet` = `outlookReady()` (orologio + credenziali) |
| 4 | tuya | sì | `tuya_min` | `preconditionsMet` = credenziali + campione BSEC valido + orologio sincronizzato; `maxAttempts = 0`; `runsOutsideRadioHours = TUYA_IGNORE_ACTIVE_HOUR`; prima del cinema, così il suo handshake TLS è altra copertura del boot di render |
| 5 | cinema | sì | DAILY `cine_h` | ultimo: è l'unico che può pagare il cold start, e il tempo degli altri è la copertura. `preconditionsMet` = `cinemaReady()` (partizione del wallpaper presente); il suo `onRoundStart()` è il ping di sveglia |
| 6 | bsec | no | custom, `Indoor::msUntilNextPoll()` | `preconditionsMet` = `Indoor::attivo` (senza sensore il task è sospeso); `onOutcome` = `onIndoorOutcome()`, che chiede il ridisegno solo dentro la fascia WiFi; **prima del display**, così un campione appena prodotto entra nel frame dello stesso giro |
| 7 | display | no | `disp_min` come **rate limit** | `preconditionsMet` = `Weather::sporco() && isFirstFrameAllowed()` |

**Il giro ha due fasi**, e in ciascuna gli `onRoundStart()` di *tutti* i task dovuti girano prima di *qualunque* `run()`. È questo che dà al ping del cinema la sua copertura: parte all'inizio, e render fa boot mentre l'ESP32 è occupato con gli altri fetch. Con `preconditionsMet()` falso il task non è dovuto, quindi nemmeno il suo `onRoundStart()` parte.

**Coalescing.** Il gate di accensione della radio valuta la scadenza **stretta** (anticipo 0); l'esecuzione, a radio già accesa, usa l'anticipo `coal_min`. Così la finestra si apre quando serve davvero, ma una volta aperta ci si infila anche chi scadrebbe fra poco: un task può partire in anticipo, mai in ritardo. È il meccanismo che tiene i task sulla stessa finestra radio, e a differenza di un aggancio esplicito fra due task non dipende dall'allineamento manuale delle cadenze.

**La radio non è dello scheduler.** `wifiOn`/`wifiOff` gli vengono passate in `begin()` e restano nel `.ino` (leggono `Env.h`). Durante la finestra di manutenzione lo scheduler gira in `RadioMode::MANAGED_BY_MAINTENANCE`: usa la STA se c'è, non la accende né la spegne, e valuta il giro al più ogni `MAINTENANCE_SCHEDULER_PERIOD_MS` (1 s) anche se il `loop()` lo chiama ogni 10 ms per il web server, perché nessun task ha granularità più fine del secondo. `sleepUntilNextEvent()` ha una rete di sicurezza che logga se trova la radio ancora accesa.

**L'ora si rilegge dopo `wifiOn()`.** Il "giorno di oggi" passato alle valutazioni viene da un unico helper (`detail::localNow()`), chiamato per il gate della radio, di nuovo dopo l'accensione e di nuovo per la fase locale: la prima sincronizzazione SNTP avviene dentro `wifiOn()`, e con l'ora presa prima un task giornaliero non registrerebbe `lastSlotDayOfYear`. `msUntilLocalHour()` calcola la distanza in interi su `time_t`, senza il `double` di `difftime()`.

**L'orologio non è dello scheduler, e non è di nessun ramo del `loop()`.** Sta in `Clock.h`, e lo fa partire chi constata di avere la rete: `wifiOn()` nella forma bloccante, la macchina a stati di `Maintenance` in quella non bloccante quando la STA sale. Con la radio della manutenzione lo scheduler non chiama `wifiOn()`, quindi senza quel secondo aggancio nella finestra di manutenzione nessuno sincronizzerebbe.

**Chi dipende dall'ora assoluta lo dichiara con `preconditionsMet()`.** I due task calendario la ricavano da `isClockValidForCalendars()`, cioè `Clock::valido()`: senza orologio le loro query filtrano da `1970-01-01` e, ordinando per data, riportano i **primi** eventi del calendario invece dei prossimi. Una risposta così è HTTP 200, quindi senza la precondizione lo scheduler la registrerebbe come successo e chiuderebbe lo slot per la cadenza piena. Con la precondizione falsa il task vale `NEVER`: sospeso, slot intatto, nessuna radio accesa per lui, e dovuto da sé appena l'ora diventa vera. Non può andare in stallo perchè meteo e mail non dipendono dall'orologio e continuano ad aprire la finestra radio, dove la sincronizzazione avviene; se anche loro sono sospesi per credenziali assenti, la sincronizzazione la fa la finestra di manutenzione al boot.

**La scadenza custom precede ogni altra regola** di `msUntilDue()`, `neverRun` compreso, e un task locale gira solo con scadenza 0. Una scadenza custom deve quindi valere 0 quando il task va eseguito la prima volta: se dipendesse da un dato che solo l'esecuzione produce, il task non partirebbe mai. `test_scheduler.cpp` fissa la regola.

**Credenziali.** Un produttore con i valori di `Env.h` lasciati al template è sospeso dal suo `preconditionsMet()`, che consulta `isConfigured()` del modulo (`Net::isCredentialSet()`: vuoto o prefisso `paste_`/`0000000`). Una chiave presente ma rifiutata invece fa fallire il task alla sua cadenza, come qualunque guasto; gli altri proseguono. Con tutti i produttori di rete sospesi l'orologio si sincronizza comunque nella finestra di manutenzione al boot.

**Il ridisegno non è dello scheduler**: il flag vive in `Weather::detail::needsRefresh` (unico), e i task lo chiedono nel proprio `onOutcome(Outcome)`. Il display ha un `onDisplayOutcome()` vuoto apposta, altrimenti si auto-sporcherebbe a ogni giro.

**Il primo frame** non aspetta dati validi: `isFirstFrameAllowed()` è vero appena un task di rete ha prodotto un esito **qualsiasi**, anche un fallimento, oppure sono passati `PRIMO_FRAME_ATTESA_MS` dal boot. Con la rete a posto il frame arriva in pochi secondi coi dati veri; senza rete arriva comunque, coi placeholder `--`. Il gate vive interamente nello scheduler: Weather disegna e basta.

**Diagnostica.** "Perché questo task non parte" è la combinazione di più campi: per questo `printStatus(Print&)` non è decorativa, ed è esposta su `/` (pagina di stato) della finestra di manutenzione. Ogni esito è loggato con durata, tentativo e prossima scadenza.

**Aggiungere un task** = una riga di tabella più gli adattatori, ed è tutto: nessun modulo e nessun ramo di `loop()` si toccano. Tuya è l'esempio già in tabella, e l'unico che usa due campi altrimenti dormienti, `maxAttempts = 0` e `runsOutsideRadioHours`, più un `onOutcome()` che non è `markDirtyOnOk`: chiede il ridisegno solo quando `hasFailed()` **cambia** rispetto al badge già disegnato, non su successo (vedi [[tuya_module]]).

**Verificato per esecuzione**: `test/test_scheduler.cpp` compila `Scheduler.h` su host sostituendo con macro `time()`, `localtime_r()` e `mktime()`; le ultime due passano dalle regole POSIX di `test/stub/fuso_posix.h` invece che dalla libc, così il test gira identico su macOS e su Windows, dove il CRT non sa leggere i campi di transizione di una stringa TZ. Copre cadenze, coalescing, ritenti e soglia, `SKIPPED` che non spende il tentativo, cadenza giornaliera col recupero in giornata, fascia oraria, orologio non sincronizzato, cambio dell'ora legale, rollover di `millis()` a 49,7 giorni, clamp del light sleep, il giro di manutenzione al più ogni secondo e il giorno registrato quando l'orologio si sincronizza dentro `wifiOn()`. `./test/esegui.sh` o `test\esegui.ps1`.

---
name: Scheduler centralizzato dei flussi temporizzati
description: Scheduler.h possiede tutto il timing; struttura della tabella dei task, le due fasi del giro, coalescing, proprieta' della radio
type: project
---

`Scheduler.h` possiede **tutto** il timing: intervallo, istante dell'ultimo slot, contatore dei fallimenti e politica di ritento. I moduli espongono solo un `runFetch()` che fa I/O e dice com'è andata: nessuno di loro sa quando tocca a lui.

**La tabella sta nel `.ino`** (`TABELLA_TASK`), non in `Scheduler.h`, perché nomina anche il cinema, che è fatto di statici dello sketch. L'ordine delle righe **è** l'ordine di esecuzione:

| # | task | radio | cadenza | perché lì |
|---|---|---|---|---|
| 0 | meteo | sì | `owm_min` | `datiIncompleti` = corrente senza previsioni ⇒ ritenta invece di chiudere lo slot |
| 1 | mail | sì | `mail_min` | prima di Google: condividono la cache del token OAuth, chi gira per primo paga il refresh |
| 2 | google | sì | `goog_min` | il token è quello appena rinfrescato da mail |
| 3 | outlook | sì | `outl_min` | |
| 4 | tuya | sì | `tuya_min` | `pronto` = credenziali + campione BSEC valido + orologio sincronizzato; `maxTentativi = 0`; `ignoraFascia = TUYA_IGNORE_ACTIVE_HOUR`; prima del cinema, così il suo handshake TLS è altra copertura del boot di render |
| 5 | cinema | sì | GIORNALIERA `cine_h` | ultimo: è l'unico che può pagare il cold start, e il tempo degli altri è la copertura. Il suo `prima()` è il ping di sveglia |
| 6 | bsec | no | custom, dal sensore | **prima del display**, così un campione appena prodotto entra nel frame dello stesso giro |
| 7 | display | no | `disp_min` come **rate limit** | `pronto` = `Weather::sporco() && primoFrameConsentito()` |

**Il giro ha due fasi**, e in ciascuna i `prima()` di *tutti* i task dovuti girano prima di *qualunque* `esegui()`. È questo che dà al ping del cinema la sua copertura: parte all'inizio, e render fa boot mentre l'ESP32 è occupato con gli altri fetch.

**Coalescing.** Il gate di accensione della radio valuta la scadenza **stretta** (anticipo 0); l'esecuzione, a radio già accesa, usa l'anticipo `coal_min`. Così la finestra si apre quando serve davvero, ma una volta aperta ci si infila anche chi scadrebbe fra poco: un task può partire in anticipo, mai in ritardo. È il meccanismo che tiene i task sulla stessa finestra radio, e a differenza di un aggancio esplicito fra due task non dipende dall'allineamento manuale delle cadenze.

**La radio non è dello scheduler.** `wifiOn`/`wifiOff` gli vengono passate in `begin()` e restano nel `.ino` (leggono `Env.h`). Durante la finestra di manutenzione lo scheduler gira in modalità `ESTERNA`: usa la STA se c'è, non la accende né la spegne. `dormi()` ha una rete di sicurezza che logga se trova la radio ancora accesa.

**L'orologio non è dello scheduler, e non è di nessun ramo del `loop()`.** Sta in `Clock.h`, e lo fa partire chi constata di avere la rete: `wifiOn()` nella forma bloccante, la macchina a stati di `Maintenance` in quella non bloccante quando la STA sale. In modalità `ESTERNA` lo scheduler non chiama `wifiOn()`, quindi senza quel secondo aggancio nella finestra di manutenzione nessuno sincronizzerebbe: è il difetto che quella scelta ripara.

**Chi dipende dall'ora assoluta lo dichiara con `pronto()`.** I due task calendario hanno `pronto = calendarioPronto`, cioè `Clock::valido()`: senza orologio le loro query filtrano da `1970-01-01` e, ordinando per data, riportano i **primi** eventi del calendario invece dei prossimi. Una risposta così è HTTP 200, quindi senza la precondizione lo scheduler la registrerebbe come successo e chiuderebbe lo slot per la cadenza piena. Con `pronto()` falso il task vale `MAI`: sospeso, slot intatto, nessuna radio accesa per lui, e dovuto da sé appena l'ora diventa vera. Non può andare in stallo perchè meteo e mail non hanno quella precondizione e continuano ad aprire la finestra radio, dove la sincronizzazione avviene.

**Il ridisegno non è dello scheduler**: il flag vive in `Weather::detail::needsRefresh` (unico), e i task lo chiedono nel proprio `dopo(Esito)`. Il display ha un `dopo` vuoto apposta, altrimenti si auto-sporcherebbe a ogni giro.

**Il primo frame** non aspetta dati validi: `primoFrameConsentito()` è vero appena un task di rete ha prodotto un esito **qualsiasi**, anche un fallimento, oppure sono passati `PRIMO_FRAME_ATTESA_MS` dal boot. Con la rete a posto il frame arriva in pochi secondi coi dati veri; senza rete arriva comunque, coi placeholder `--`. Il gate vive interamente nello scheduler: Weather disegna e basta.

**Diagnostica.** Con lo scheduler "perché questo task non parte" non è più una riga di codice ma la combinazione di sei campi: per questo `stampaStato(Print&)` non è decorativa, ed è esposta su `/status` della finestra di manutenzione. Ogni esito è loggato con durata, tentativo e prossima scadenza.

**Aggiungere un task** = una riga di tabella più gli adattatori, ed è tutto: nessun modulo e nessun ramo di `loop()` si toccano. Tuya è l'esempio già in tabella, e l'unico che usa due campi altrimenti dormienti — `maxTentativi = 0` e `ignoraFascia` — più un `dopo()` che non è `marcaSuOk`: chiede il ridisegno solo quando `hasFailed()` **cambia** rispetto al badge già disegnato, non su successo (vedi [[tuya_module]]).

**Verificato per esecuzione**: `test/test_scheduler.cpp` compila `Scheduler.h` su host sostituendo con macro `time()`, `localtime_r()` e `mktime()`; le ultime due passano dalle regole POSIX di `test/stub/fuso_posix.h` invece che dalla libc, cosi' il test gira identico su macOS e su Windows, dove il CRT non sa leggere i campi di transizione di una stringa TZ. Copre cadenze, coalescing, ritenti e soglia, `SALTATO` che non spende il tentativo, cadenza giornaliera col recupero in giornata, fascia oraria, orologio non sincronizzato, cambio dell'ora legale, rollover di `millis()` a 49,7 giorni e clamp del light sleep. `./test/esegui.sh`.

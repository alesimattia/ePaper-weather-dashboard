---
name: Finestra di manutenzione (Maintenance.h)
description: Macchina a stati STA con fallback AP, pagine /config e /update, e le quattro trappole che rompono l'upload
type: project
---

`Maintenance.h` è il modulo della finestra di servizio: l'aggiornamento del firmware è una delle sue pagine, non il suo scopo.

**Stati:** `Inattiva → AttesaSta → ServerSta | ServerAp → Inattiva`. A boot la STA sale in background; se ottiene un indirizzo entro `MAINT_STA_TIMEOUT_S` il server è sulla rete di casa (IP o `epd-dashboard.local` via mDNS), altrimenti si accende l'AP di emergenza. Il fallback usa `WIFI_AP_STA` e **non** `WIFI_AP`: la STA continua a tentare, così un router più lento del dispositivo (tipico dopo un blackout) non condanna il giro alla sola rete di emergenza.

`server.begin()` è chiamato **una volta sola**: `WebServer` ascolta su `0.0.0.0`, quindi serve qualunque interfaccia abbia un indirizzo. La macchina a stati governa solo radio, mDNS e scadenza — nessun rebinding.

**La scadenza parte quando il server diventa raggiungibile**, non a boot. `proroga()` (chiamata da ogni handler) la riporta a `millis() + maint_min`: **nessun tetto assoluto**, la finestra si chiude `maint_min` dopo l'ultima richiesta, quindi una pagina aperta a ridosso della fine non porta a un upload rifiutato a metà. Nei POST di `/config` e `/config/reset` `proroga()` gira **dopo** il salvataggio, così la nuova durata vale già per la finestra in corso. `secondiResidui()` arrotonda per eccesso, quindi subito dopo una richiesta mostra la durata piena (180 s, non 179).

**Pagine:** ogni handler apre con `apriPagina(titolo)` (risposta chunked, testa comune con `<title>` = `<h2>`) e chiude con `chiudiPagina()` (piede `PIEDE` con i link a `/` "Stato", `/config`, `/route`, `/update`, più il chunk terminale). `/route` gestisce le impostazioni del percorso ([[routefinder_morning_route]]) con lo stesso schema di `/config`: candidato completo applicato in blocco, campo malformato segnalato e ignorato, `proroga()` dopo il salvataggio; `handleConfigPost()` rifiuta una fascia WiFi che non contiene più la fascia percorso. Il testo HTML resta ASCII perchè le pagine non dichiarano un charset. `/` è la pagina di stato (`handleStato`), servita direttamente senza redirect; non esiste `/status` nè una home con via/indirizzo, rimosse su richiesta dell'utente perchè dati inutili, e niente riga hostname (giudicata incomprensibile). È `text/html` con uptime, heap, secondi residui e dump dello scheduler in `<pre>` (solo stringhe del firmware, nessuna sanificazione). Unica pagina fuori dallo schema: l'esito dell'upload, generato da `HTTPUpdateServer` (successo = meta refresh di 15 s verso `/`; errore = testo senza link), non modificabile senza reimplementare l'handler POST. Le risposte 401/403 restano `text/plain`.

**Chiusura:** stessa sequenza di `wifiOff()` (`disconnect(true, false)`, cache BSSID conservata) e radio consegnata **spenta** allo scheduler. Non riapribile senza riavvio.

## Le quattro trappole, tutte verificate sul core

Ognuna rompe l'upload se ignorata, e nessuna dà un errore chiaro:

1. **Mai chiamare `server.collectHeaders()`.** La chiama `updater.setup()` con `Origin` e `Host` per il controllo CSRF; una seconda chiamata ne *sostituisce* la lista e ogni upload fallisce con "Wrong origin received". I due header restano comunque disponibili ai nostri handler, e infatti li riusiamo per il controllo di origine sulle POST di `/config`.
2. **L'`action` del form deve restare relativa** (`action=/update`), per lo stesso controllo CSRF.
3. **Le credenziali della Basic Auth vanno passate a `updater.setup()`.** L'upload è atomico dentro una sola `handleClient()`: parsing multipart → `Update.end(true)` → `ESP.restart()`. Un controllo a valle arriverebbe dopo che la partizione è già scritta. Per lo stesso motivo la deadline della finestra **non può** interrompere un upload iniziato.
4. **La tabella delle partizioni deve avere due slot applicative**: quella di `partitions.csv` ne ha due da 1920 KB (vedi [[build_toolchain_arduino]]). Con `Huge APP (3MB No OTA)` c'è una sola slot e `Update.begin()` fallisce. L'OTA non riscrive la tabella delle partizioni.

**La finestra fa partire anche l'orologio.** `apri(Stato::ServerSta)` e il ramo `ServerAp` quando la STA sale chiamano `Clock::sincronizza(0)`, non bloccante. Sta lì e non altrove perchè sono i due punti in cui la macchina a stati **sa** di avere la rete di casa con un indirizzo, e perchè in questa finestra la radio non è dello scheduler: `wifiOn()` non viene mai chiamata e senza quell'aggancio nessuno sincronizzerebbe. Bloccare è escluso: il loop gira ogni ~10 ms e l'attesa congelerebbe web server e access point durante un upload.

## Lo scheduler aspetta mentre un client è collegato

Il web server è servito dallo stesso `loop()` che esegue fetch e refresh: un fetch (fino a 45 s per il cinema) o un refresh (24 s) lo lasciano senza risposta. Per questo `loop()` chiama `Scheduler::runRound(RadioMode::MANAGED_BY_MAINTENANCE)` solo se `Maintenance::clientAttivo()` è falsa, e lo scheduler valuta comunque il giro al più ogni `MAINTENANCE_SCHEDULER_PERIOD_MS` (1 s), non a ogni passata da 10 ms. È vera con almeno un dispositivo associato all'AP di emergenza (`WiFi.softAPgetStationNum()`), oppure con una richiesta servita da meno di `MAINT_CLIENT_IDLE_MS` (30 s). Sulla rete di casa il PC è collegato al router, non all'ESP, e il web server chiude la connessione dopo ogni richiesta, quindi la richiesta recente è l'unico segnale disponibile; l'istante lo registra `proroga()`, che ogni handler chiama. Senza tetto assoluto, richieste più fitte di 30 s (es. un client che interroga `/` in polling) tengono fermo lo scheduler, e quindi il pannello, finchè continuano. Una richiesta che arriva mentre un fetch è già partito aspetta comunque la fine di quel fetch.

## Diagnostica e rollback

La pagina di stato (`/`) mostra anche heap minimo dal boot (`ESP.getMinFreeHeap()`), blocco contiguo massimo (`ESP.getMaxAllocHeap()`) e stack minimo residuo del loop (`uxTaskGetStackHighWaterMark`, l'handler gira nel task del loop). Sono le misure che confermano i margini stimati: picco circa 93 KB nel giorno del cinema (piani 46,8 KB + sessione TLS circa 33 KB + JSON + String) contro 150-180 KB di heap libero stimato, stack del loop da 8192 byte. La stessa terna esce in `LOGV` prima del light sleep nei giri che hanno eseguito qualcosa.

**Rollback dell'app: il bootloader lo supporta, il core lo annulla.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, ma `initArduino()` conferma la nuova immagine all'avvio, prima di `setup()`, tramite gli hook deboli `verifyRollbackLater()` (default false) e `verifyOta()` (default true) in `esp32-hal-misc.c`. Un firmware caricato da `/update` che va in crash dopo l'avvio quindi non torna alla versione precedente e richiede il cavo seriale. Per scelta dell'utente resta così; per attivarlo basterebbe ridefinire `verifyRollbackLater()` a true e chiamare `esp_ota_mark_app_valid_cancel_rollback()` quando il firmware ha dimostrato di funzionare, accettando che una mancanza di corrente prima di quel punto riporti alla versione precedente.

## Costi misurati

mDNS pesa **~34 KB di flash** (spegnibile con `MAINT_MDNS 0`, resta l'IP). Basic Auth ~344 byte. Con tutto attivo il firmware sta al 67% (9.7") e 69% (12.2") dello slot.

## Sicurezza, detta chiaramente

Sulla rete di casa la pagina è raggiungibile da chiunque sia sulla LAN per la durata della finestra, e `/update` accetta un firmware qualunque — che conterrebbe le credenziali di `Env.h`. Le mitigazioni sono proporzionate a un dispositivo domestico: finestra breve solo a boot che si chiude `maint_min` dopo l'ultima richiesta, Basic Auth opzionale (`MAINT_HTTP_PASSWORD` in `Env.h`, **da definire**), controllo dell'origine sulle POST. Niente HTTPS, niente token.

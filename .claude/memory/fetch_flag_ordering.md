---
name: Politica di ritento e consumo dello slot
description: Un tentativo per task per ciclo, ritento solo al ciclo successivo e mai nella stessa accensione (scelta dell'utente: il frame non aspetta mai); maxAttempts e consumo dello slot (0 = un tentativo per slot, solo Tuya); restartSlot sul fronte di salita della precondizione e al primo ciclo del giorno contro i ritenti ereditati; radio caduta = SKIPPED con slot intatto; DEFERRED = rinvio di un ciclo che apre lo slot senza spendere tentativi
type: project
---

**Regola, voluta dall'utente:** un task fallito ritenta **una sola volta, al ciclo successivo** (il frame dopo), **mai nella stessa accensione della radio**: nessuna seconda passata, nessuna attesa dentro il passo, il frame del ciclo non aspetta mai un ritento. Al più un tentativo per task per ciclo (`attemptCycle`), che è anche il freno anti retry-storm: nella finestra di manutenzione `loop()` gira ogni ~10 ms, e senza freno un fallimento ripetuto colpirebbe un endpoint già in difficoltà.

Il ritento nella stessa accensione non va reintrodotto: pochi secondi dopo il primo tentativo cade prima che lo spin-up di render.com (circa un minuto) finisca, e il secondo fallimento chiude lo slot.

**Consumo dello slot.** `maxAttempts` è il numero di fallimenti consecutivi dopo cui lo slot si chiude.
- `FETCH_MAX_TENTATIVI` (2) per meteo, calendari, percorso e cinema: primo fallimento → `retrying`, dovuto al ciclo dopo; secondo → `closeSlot()`, con `slotCycle = openCycle` (il ciclo in cui lo slot si era aperto), quindi la cadenza non cambia fase, e si attende la cadenza piena.
- Un'esecuzione riuscita ma con dati incompleti (`hasIncompleteData()`, il meteo con la corrente e senza previsioni) conta come fallimento.
- **`0` = un tentativo per slot comunque vada**: solo Tuya, i cui guasti (credenziali, region, device model) sono permanenti sulla scala dei minuti.

**Slot che riparte pulito: `Scheduler::restartSlot()`** azzera `neverRun` (quindi dovuto subito), `retrying` e `failedAttempts`. Scatta in due casi:
- sul **fronte di salita della precondizione**, rilevato da `isDue()` con il campo `State::eligible` (per questo `isDue()` non è const; è idempotente fra le valutazioni dello stesso giro, `printStatus()` compreso);
- al **primo ciclo in fascia di ogni giorno**, per tutti i task.

Causa che risolve: senza una transizione per la sospensione, un task che usciva dalla fascia, da una forzatura o dalla notte con un ritento pendente lo ritrovava alla prossima attivazione, e il primo fallimento chiudeva lo slot senza ritento. Copre ingresso nelle fasce di percorso e cinema, inizio e fine di una forzatura, ritorno dell'ora. Il ritento abbandonato quando la precondizione cade non basta: non copre il ritorno ad Automatica allo scadere di una forzatura, con il buffer che contiene l'altra immagine e la cadenza che dice "non dovuto". Le sole precondizioni che oscillano sono di Tuya, a cadenza di un ciclo: effetto nullo.

**Radio caduta ⇒ `Outcome::SKIPPED`**, non fallimento: lo stato resta intatto e il tentativo non si spende, perchè il controllo `WL_CONNECTED` precede l'esecuzione. Radio dello scheduler che non si connette o STA mai salita in manutenzione: passo chiuso senza eseguire niente, ritenti intatti per il ciclo con la radio su.

**Errore da evitare:** non spostare il consumo dello slot prima del controllo della radio "per simmetria": un dispositivo senza rete brucerebbe gli slot.

**Rinvio ⇒ `Outcome::DEFERRED`** (percorso e cinema con il server render.com in avvio, [[esp32_cinema_consumer]]): in `recordOutcome()` apre lo slot (`openCycle` = ciclo del rinvio, se non in ritento nè già in rinvio), segna `attemptCycle` (una sola esecuzione per ciclo), NON spende tentativi nè conta negli esiti, `deferred` = true; log `rinviato al prossimo ciclo`. Il task resta dovuto al ciclo dopo senza nessun controllo in `isDue()`, perchè il rinvio non tocca `neverRun`, `retrying` e `slotCycle` (un `|| deferred` in `isDue()` è ridondante: mutante equivalente). L'esito successivo non riscrive `openCycle` e azzera `deferred`; `restartSlot()` e `closeSlot()` lo azzerano; `printStatus()` mostra "rinvio". Perchè non `SKIPPED`: lo slot si ancorerebbe al ciclo del download e ogni slot orario del cinema slitterebbe di un ciclo (18:05, 19:10, 20:15). Il limite al numero di rinvii non sta nello scheduler ma in `RenderServer`: una sveglia vale 15 min, quindi al più un rinvio ogni 15 min per server.

Verificato in `test/test_scheduler.cpp`, anche con prove di mutazione: togliendo l'azzeramento del mattino o il fronte di salita i test relativi falliscono; nel blocco del rinvio falliscono togliendo l'ancoraggio di `openCycle`, l'azzeramento in `restartSlot()` o trattando `DEFERRED` come esito ordinario. Vedi [[scheduler_task_table]].

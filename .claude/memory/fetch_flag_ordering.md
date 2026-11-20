---
name: Politica di ritento e consumo dello slot
description: Al più due tentativi per task per ciclo come freno anti retry-storm, il secondo nella stessa accensione solo per i fallimenti rapidi; maxAttempts e consumo dello slot (0 = un tentativo per slot, solo Tuya); nel primo passo dopo il boot si ritenta ogni fallimento; radio caduta = SKIPPED con slot intatto
type: project
---

Il vincolo che conta è **anti retry-storm**: nella finestra di manutenzione `loop()` gira ogni ~10 ms e lo scheduler valuta un giro al secondo, quindi un tentativo fallito ripetuto senza freno colpirebbe un endpoint magari già in difficoltà. Il freno è la regola dello scheduler di **al più due tentativi per task per ciclo**: il secondo solo per un fallimento rapido, dopo `SESSION_RETRY_MS` e con la radio ancora su; in ogni altro caso un tentativo per ciclo (`attemptCycle`). Vale per tutti i task e non dipende dalla frequenza del loop.

**Ritento e consumo dello slot.** `maxAttempts` è il numero di fallimenti consecutivi dopo cui lo slot si chiude.
- `FETCH_MAX_TENTATIVI` (2) per meteo, calendari, percorso e cinema: al primo fallimento il task resta in ritento. Il secondo tentativo si fa **nella stessa accensione della radio** se il primo è durato meno di `FAST_FAILURE_MAX_MS` (guasto di connessione o di protocollo: il tetto della connessione TCP è 5 s) e la STA è ancora su; altrimenti torna dovuto al ciclo successivo, perchè un timeout ripetuto subito raddoppierebbe il tempo di radio e di norma fallirebbe uguale. Lo slot resta ancorato al ciclo in cui si era aperto, così la cadenza non cambia fase; al secondo fallimento si chiude e si attende la cadenza piena.
- Un'esecuzione riuscita ma con dati incompleti (`hasIncompleteData()`, il meteo con la corrente e senza previsioni) conta come fallimento, ed essendo durata poco è un fallimento rapido.
- **`0` = un tentativo per slot comunque vada**: lo slot si chiude dopo l'esecuzione, riuscita o no. Lo usa solo Tuya, i cui guasti (credenziali, region, device model) sono permanenti sulla scala dei minuti. Sensore e pannello non sono task: li scandisce il ciclo.

**Come si attende fra le due passate.** Con la radio dello scheduler un `delay()` unico dentro il passo, perchè non si può uscire a metà passo: il loop spegnerebbe la radio prima del light sleep. Nella finestra di manutenzione il giro riprende dopo `SESSION_RETRY_MS` senza bloccare il web server. Se la STA cade fra le due passate non si attende e il ritento resta al ciclo successivo. Le passate sono al massimo due per passo.

**Eccezione: il primo passo di rete dopo il boot, in manutenzione.** La soglia dei fallimenti rapidi non vale e si ritenta ogni fallimento con `maxAttempts` maggiore di 0, perchè il primo frame aspetta e il secondo tentativo, per esempio a un cold start di render.com ormai concluso, di norma riesce. Vedi [[scheduler_task_table]].

**Radio caduta ⇒ `Outcome::SKIPPED`**, non fallimento: è un guasto a monte, non del task. Lo stato resta intatto e il tentativo non si spende, perchè il controllo `WL_CONNECTED` precede l'esecuzione. Con la radio dello scheduler che non si connette il passo si chiude senza eseguire niente: nessun task brucia il suo slot, il giornaliero del cinema compreso. Lo stesso vale con la STA mai salita nella finestra di manutenzione.

**Errore da evitare:** non spostare il consumo dello slot prima del controllo della radio "per simmetria": romperebbe la garanzia che un dispositivo senza rete non bruci lo slot giornaliero del cinema.

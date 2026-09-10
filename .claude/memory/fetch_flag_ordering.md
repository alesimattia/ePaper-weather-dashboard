---
name: Politica di ritento e consumo dello slot
description: Come lo scheduler evita il retry-storm, e perche' il cinema usa la politica pessimista
type: project
---

Il vincolo che conta è **anti retry-storm**: durante la finestra di manutenzione `loop()` gira ogni ~10 ms, quindi un tentativo appena fallito, se non frenato, si ripeterebbe cento volte al secondo contro un endpoint che magari è già in difficoltà. Due meccanismi di `Scheduler.h` lo impediscono, e valgono per **tutti** i task.

**1. `FETCH_RITENTO_MS` (30 s) — distanza minima fra due tentativi dello stesso slot.** È l'unico freno che agisce anche quando lo slot resta aperto. Senza di esso, un task con `datiIncompleti()` vero o in ritento martellerebbe.

**2. `maxTentativi` — quanti fallimenti consecutivi prima di chiudere lo slot.**
- `FETCH_MAX_TENTATIVI` (2) per meteo, mail, calendari, cinema: dopo due fallimenti lo slot si consuma e si attende la cadenza piena.
- **`0` = nessun ritento**: lo slot è consumato comunque vada, quindi un endpoint irraggiungibile costa esattamente un tentativo per intervallo. La usano `bsec` e `display`, che non hanno senso da ritentare.

Il consumo dello slot avviene **dopo** il pre-check `WL_CONNECTED` del runner, e l'ordine conta: se la radio non sale il task esce come `SALTATO` prima di eseguire, lo stato resta intatto e il tentativo non viene speso. È la garanzia che un dispositivo senza rete non bruci lo slot giornaliero del cinema.

**Radio caduta a giro iniziato ⇒ `Esito::SALTATO`**, non fallimento: è un guasto a monte, non del task. Lo slot resta intatto e nessun tentativo viene consumato, così il fetch avviene davvero al primo giro con la radio su. È un miglioramento rispetto al comportamento precedente, dove Weather e i calendari bruciavano tentativi di backoff in quella situazione.

**Errore da evitare:** non spostare il consumo dello slot pessimista prima del pre-check WiFi "per simmetria". Romperebbe la garanzia che un dispositivo senza rete non bruci lo slot giornaliero del cinema.

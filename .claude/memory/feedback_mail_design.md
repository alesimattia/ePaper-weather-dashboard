---
name: Mail.h design preferences
description: regole esplicite dell'utente sul modulo Mail.h (best-effort, ordering, cap stringhe)
type: feedback
---

Vincoli di design dichiarati dall'utente durante l'implementazione di
`Mail.h` (2026-05). Da rispettare se si refactora o si estende il modulo.

**1. `Mail::runFetch()` deve essere best-effort, MAI bloccante per gli altri fetch.**

**Why:** l'utente ha detto esplicitamente "se non trova mail o se non c'e'
connessione wifi il flusso del software deve continuare regolarmente
senza interruzioni". Le mail sono utili ma secondarie rispetto a
meteo/calendario.

**How to apply:** nel `.ino`, il return value di `Mail::runFetch()` va
usato SOLO per decidere `Weather::markDirty()` (la UI mail va ridisegnata
solo se la cache e' cambiata). NON propagare l'errore al flusso:
nessun `return`/`goto` su fail. Mail::runFetch() puo' fallire silenziosamente:
la cache resta com'era e i fetch calendario partono comunque.

**UPDATE (2026-05): La UI mail ora ESISTE.** Originariamente l'utente disse
"per ora non voglio modificare l'interfaccia grafica ma devo solo scaricare
le mail". Successivamente ha richiesto e approvato il rendering: griglia
2x2 (097c, 4 mail) / 2x3 (122c, 6 mail) nell'area y=CINEMA_H..BANNER_Y,
icona busta `INDOOR_ICON_MAIL` 20x20 in alto a sinistra, font come gli
eventi calendario, solo nero. `Mail::draw()` viene chiamato dentro
`Weather::renderFrame()` paged loop e il `.ino` chiama `Weather::markDirty()`
dopo un `Mail::runFetch()` riuscito.

**2. Mail va eseguito SUBITO PRIMA di Outlook + Google Calendar.**

**Why:** richiesta esplicita ("lo scaricamento delle mail deve essere
fatto subito prima dei calendari"). Le mail vengono percepite come
informazione a piu' alta priorita' del calendario.

**How to apply:** in entrambi i rami del `loop()` (OTA aperta + on-demand),
`Mail::pendingFetch()` / `Mail::runFetch()` va piazzato dopo
`fetchCinemaImage()` e PRIMA dei due `Calendar::*::runFetch()`. Se si
inverte l'ordine va validato con l'utente.

**3. Subject delle mail cap a 60 caratteri (`MAIL_SUBJECT_LEN = 60`).**

**Why:** richiesta esplicita "limita anche la lunghezza dell'oggetto a
60 caratteri massimo".

**How to apply:** se si cambia `MAIL_SUBJECT_LEN` ricontrollare con l'utente.
Il cap a 60 e' una scelta di prodotto, non vincolato all'API Gmail.

**4. `MAIL_GOOGLE_FETCH_MIN` deve essere un `#define` SEPARATO da
`CAL_GOOGLE_FETCH_MIN`, anche se attualmente l'utente lo tiene allineato.**

**Why:** richiesta esplicita "voglio che il tempo di refresh
MAIL_GOOGLE_FETCH_MIN sia configurabile separatamente rispetto al tempo
di refresh del calendario". Anche se nel `.ino` il valore corrente e'
identico (10), il knob deve restare indipendente per future regolazioni.

**How to apply:** non collassare i due `#define` in un alias, anche se
sembra DRY. L'indipendenza concettuale e' un requisito.

**5. Numero minimo di secret in `Env_template.h`.**

**Why:** "Tieni solo il minimo dei secrets necessari in Env_template.h".

**How to apply:** Mail.h NON aggiunge nuovi `#define` in Env_template.h.
Riusa `GOOGLE_CLIENT_ID/SECRET/REFRESH_TOKEN` con scope unificati.
Tutto cio' che e' configurazione (host, scope, cadenza, cap) vive in
Mail.h come `#define` override-abile.

---
name: Timings.h - default, pavimenti, override su NVS
description: Dove vivono i tempi, come si validano, chiavi NVS e politica di clamp
type: project
---

`Timings.h` ha tre sezioni con criteri diversi, e la distinzione è il punto del file:

1. **Modi hardware** — `BSEC_PERIODO_ULP_S` (300), `DISPLAY_REFRESH_PIENO_S` (24), `SLEEP_MIN_S`/`SLEEP_MAX_S`. Imposti da libreria o fisica, non si configurano.
2. **Cadenze** — default `#ifndef` + **pavimento** `#define` accanto. Modificabili a runtime, persistite su NVS.
3. **Compile-time** — timeout, budget, ritenti: dipendono da protocollo o hardware, non da una preferenza.

**Politica sotto soglia, su tre livelli:**
- `static_assert` sui default e sui vincoli incrociati (fascia coerente, ora cinema dentro la fascia, coalescing minore di ogni cadenza, refresh ≥ durata di un refresh);
- la pagina `/config` **rifiuta** un valore fuori dai limiti e ne dice il motivo;
- al boot `Timings::begin()` **clampa** e logga: serve a recuperare valori diventati illegali perché un aggiornamento firmware ha alzato un pavimento. Il valore corretto viene riscritto in NVS, così `/config` mostra quello davvero in uso e non un fantasma. Se la combinazione è incoerente si torna interamente ai default, che per costruzione sono validi.

**Chiavi NVS: massimo 15 caratteri.** `Preferences` tronca in silenzio oltre quella soglia, quindi una chiave troppo lunga sarebbe un bug muto: c'è una `static_assert` con un `constexpr` sulla lunghezza che lo impedisce. Namespace `"timings"`, distinto da `"bme680"` di Indoor.

**`schema`** si alza solo quando una chiave cambia significato o unità: in quel caso il namespace viene azzerato. Una chiave *assente* ricade sul proprio default, quindi aggiungere un campo in un firmware successivo non richiede migrazione.

**`save()` è differenziale**: riscrive solo ciò che è cambiato, per non consumare cicli di flash.

**Lo scheduler legge `Timings::get()` a ogni valutazione** e ricalcola le scadenze da `ultimo + intervallo`, senza mai memorizzare una scadenza assoluta. È questo che fa avere effetto a una modifica dal giro successivo, senza riavvio.

**Verificato per esecuzione**, non solo per lettura: `test/test_timings.cpp` compila l'header vero con stub minimi di piattaforma e controlla pavimenti, vincoli incrociati, applicazione transazionale, round-trip su NVS, clamp al boot, NVS assente e schema mancante. Si lancia con `./test/esegui.sh`.

**`applica()` e' transazionale per necessita', non per eleganza.** I vincoli legano piu' campi fra loro: allargare la fascia WiFi e spostare l'ora del cinema sono legittimi insieme e illegittimi presi uno alla volta. Applicandoli in sequenza, una richiesta valida verrebbe respinta a meta' a seconda dell'ordine dei campi.

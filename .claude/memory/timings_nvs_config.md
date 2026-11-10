---
name: Timings.h - default, pavimenti, passo, override su NVS
description: Dove vivono i tempi, il ciclo CYCLE_S come unità delle cadenze, il campo step del descrittore su cui poggia la validazione, chiavi NVS, schema e politica di clamp
type: project
---

`Timings.h` ha tre sezioni con criteri diversi, e la distinzione è il punto del file:

1. **Modi hardware** — `BSEC_PERIODO_ULP_S` (300), `CYCLE_S` = periodo ULP e `CYCLE_MIN` = `CYCLE_S / 60` (con `static_assert(CYCLE_S % 60 == 0)`: in modo LP varrebbe 0 e il controllo del passo dividerebbe per zero), `DISPLAY_REFRESH_PIENO_S` (24, con `CYCLE_S >= DISPLAY_REFRESH_PIENO_S` perchè a ogni ciclo si disegna), `SLEEP_MAX_S` (24 h, solo guardia). Imposti da libreria o fisica, non si configurano.
2. **Cadenze** — default `#ifndef` + **pavimento** `#define` accanto. Tutte le cadenze dei task di rete sono multipli di `CYCLE_MIN`, perchè lo scheduler le conta in cicli (vedi [[scheduler_task_table]]); i pavimenti di mail e calendari valgono `CYCLE_MIN`. Il display non ha una cadenza propria e coalescing e ritenti a ms non esistono più come chiavi. Modificabili a runtime, persistite su NVS.
3. **Compile-time** — timeout, budget, `RENDER_SPINDOWN_MS`, `BSEC_MARGINE_POLL_MS`: dipendono da protocollo o hardware, non da una preferenza.

**Il passo è un dato del descrittore.** `Timings::Campo` ha `minimo`, `massimo`, `predefinito` e `step` (`CYCLE_MIN` per le cadenze, 1 per ore e finestra); tutta la validazione per campo poggia su questi, senza controlli scritti a mano:
- `passiValidi()` è uno `static_assert` generico su `CAMPI`: step > 0, default, pavimento e tetto multipli del passo, default dentro i limiti;
- `applica()` rifiuta un valore fuori limiti o fuori passo con il motivo di `campoNonValido()`, che nomina chiave, limiti o passo;
- `begin()` clampa ai limiti e porta un valore fuori passo al multiplo successivo, che resta entro il tetto perchè anche il tetto è multiplo; logga e riscrive in NVS, così `/config` mostra il valore in uso;
- `/config` scrive `step` nell'`<input type=number>`.
`incoerenza()` resta per i soli vincoli fra campi di `Timings` (fascia WiFi coerente, ora del cinema dentro la fascia); con una combinazione incoerente al boot si torna interamente ai default. Il vincolo "fascia percorso dentro la fascia WiFi" sta in `Routefinder::windowOutsideRadioHours()`, chiamato dalla POST di `/config`.

**Aggiungere una cadenza** = un campo in `Valori`, una riga in `CAMPI` con `step`, una riga nella tabella dei task: scheduler e validazione non si toccano.

**Chiavi NVS: massimo 15 caratteri.** `Preferences` tronca in silenzio oltre quella soglia: lo impedisce una `static_assert` con un `constexpr` sulla lunghezza. Namespace `"timings"`, distinto da `"bme680"` di Indoor.

**`schema`** si alza solo quando una chiave cambia significato o unità: in quel caso il namespace viene azzerato. Vale 2 da quando le cadenze sono multipli del ciclo. Una chiave *assente* ricade sul proprio default, quindi aggiungere un campo in un firmware successivo non richiede migrazione. Al primo boot (schema assente) `begin()` chiama `save()`, che scrive **ogni** campo (una chiave assente vale 0xFFFF, diverso da qualunque valore): da lì tutte le cadenze sono persistite, e un nuovo default non si applica da sè senza uno schema nuovo o un erase della flash.

**`save()` è differenziale**: riscrive solo ciò che è cambiato, per non consumare cicli di flash.

**Lo scheduler legge `Timings::get()` a ogni valutazione** e ricalcola le scadenze dal ciclo dello slot precedente, senza mai memorizzare una scadenza assoluta: una modifica ha effetto dal ciclo successivo, senza riavvio.

**`applica()` è transazionale per necessità, non per eleganza.** I vincoli legano più campi fra loro: allargare la fascia WiFi e spostare l'ora del cinema sono legittimi insieme e illegittimi presi uno alla volta. Applicandoli in sequenza, una richiesta valida verrebbe respinta a metà a seconda dell'ordine dei campi.

**Verificato per esecuzione**: `test/test_timings.cpp` compila l'header vero con stub minimi e controlla pavimenti, vincoli incrociati, applicazione transazionale, rifiuto fuori passo con il motivo, arrotondamento al boot, schema precedente azzerato, round-trip su NVS, clamp al boot, NVS assente e schema mancante.

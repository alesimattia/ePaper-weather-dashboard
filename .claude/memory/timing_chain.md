---
name: Catena dei tempi (scheduler, sleep dinamico, pavimenti)
description: Chi possiede il timing, come si calcola il risveglio, quali sono i limiti inferiori di ogni flusso e i timeout incrociati del fetch cinema
type: project
---

Tutti i tempi stanno in `Timings.h`; il **quando** lo decide `Scheduler.h`, mai i moduli. Vedi [[scheduler_task_table]] per la struttura della tabella e [[timings_nvs_config]] per gli override a runtime.

**Pavimenti — sotto questi valori interrogare non produce informazione nuova:**

| Flusso | Pavimento | Natura |
|---|---|---|
| BME680 | 300 s (`BSEC_SAMPLE_RATE_ULP`) | hard: è un *modo* della libreria, non un intervallo. Gli altri sono LP 3 s e CONT 1 s |
| Meteo OWM | 10 min (cadenza dei dati); quota 1000/giorno = 1,44 min | soft sui dati, hard sulla quota |
| Mail, Google, Outlook | nessuno sui dati | il costo è la radio: 5-10 s per fetch |
| Cinema | `CACHE_TTL_SECONDS=3600` in produzione; render dorme a 15 min; ETag/304 | hard lato server. Il palinsesto cambia a giorni ⇒ cadenza **giornaliera**, non periodica |
| Display | 24 s (refresh pieno) | hard fisico |
| Radio | 2-15 s per accensione | costo |

**Sleep dinamico:** `Scheduler::dormi()` dorme fino al minimo delle scadenze effettive, clampato in `[SLEEP_MIN_S 30, SLEEP_MAX_S 300]`. Il tetto **è** il periodo ULP: più alto perderebbe campioni, più basso sveglierebbe a vuoto. Il pavimento impedisce che un errore di calcolo produca un ciclo di risvegli.

**Timeout del fetch cinema, che è il percorso più lungo:**
- `WIFI_CONNECT_TIMEOUT_MS` 15 s (attesa di `WL_CONNECTED` in `wifiOn()`).
- `CINEMA_HTTP_TIMEOUT_MS` 45 s, sia sul GET sia sulla lettura per piano.
- `CINEMA_PREWARM_TIMEOUT_MS` 1500 ms: è l'attesa della **risposta** al ping `/health`, non della connessione. L'handshake TLS ha il suo timeout separato (default HTTPClient, 5 s) e **non va accorciato**, altrimenti su rete lenta la richiesta non parte affatto.
- Cold start render.com **misurato 22,4 s**; dopo il boot `/cinema/arduino` risponde in **0,54 s**, perché le cache su disco sopravvivono al suspend. È il motivo per cui pingare `/health` basta: a costare è il boot del processo, non la pipeline di rendering.
- Ordine del giro: ping → meteo → mail → Google → Outlook → cinema. I fetch intermedi valgono 8-20 s e sono la copertura di quel boot.

**Worst case:** 15 s (WiFi) + fino a 22 s (cold start) + 2-3 s (download) ≈ 40 s, dentro i 45 s del timeout. Il pre-warm riduce l'attesa, non elimina il bisogno di margine.

**Durante la finestra di manutenzione** `loop()` gira con `delay(10)` per `WebServer::handleClient()`: un fetch cinema bloccante congela il server per la sua durata. Accettato, ma è il motivo per cui esiste `FETCH_RITENTO_MS` (30 s), che impedisce a un tentativo appena fallito di ripetersi cento volte al secondo.

**Se sposti il fetch giornaliero del cinema** basta cambiare `cine_h` (da `/config` o in `Timings.h`): una `static_assert` e il controllo a runtime impongono che resti dentro la fascia `wifi_h_ini..wifi_h_fin`. Non c'è più nessuno scheduler esterno da tenere allineato — vedi [[github_cron_inaffidabile]].

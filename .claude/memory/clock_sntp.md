---
name: Orologio di sistema e SNTP
description: Clock.h possiede fuso e sincronizzazione; due agganci, uno per proprietario della radio; chi dipende dall'ora assoluta e cosa succede senza
type: project
---

`Clock.h` possiede tutto quello che riguarda l'ora assoluta: il fuso POSIX e la
sincronizzazione SNTP. Tre funzioni:

- `Clock::begin()` — `setenv("TZ", CLOCK_POSIX_TZ, 1)` + `tzset()`, nessuna
  rete. Va in `setup()` prima di qualunque modulo che formatti orari locali;
- `Clock::valido()` — `time(nullptr) >= TIME_VALID_EPOCH_MIN`;
- `Clock::sincronizza(timeout_ms = TIME_SYNC_TIMEOUT_MS)` — no-op se l'ora è
  già valida, altrimenti `configTzTime(CLOCK_POSIX_TZ, NTP_SERVER_1,
  NTP_SERVER_2)` più attesa, con backoff `TIME_SYNC_RETRY_MS` fra due
  tentativi falliti.

Soglie e server stanno in `Timings.h` con tutti gli altri tempi. `valido()` è
l'**unica** definizione di "ora valida" del firmware: ci poggiano lo scheduler
(`inFascia`, la cadenza giornaliera, `stampaStato`), la precondizione dei due
task calendario e la guardia di `Tuya::runPublish()`.

## I due agganci, uno per proprietario della radio

È il punto che spiega perchè il modulo esiste invece di stare dentro `wifiOn()`:
**in questo firmware la radio ha due proprietari**, e la sincronizzazione non
appartiene a nessuno dei due.

- **Ciclo normale**: `wifiOn()` nel `.ino` chiama `Clock::sincronizza()` nella
  forma **bloccante**, fino a `TIME_SYNC_TIMEOUT_MS`. Lì si può attendere: i
  fetch di quel giro stanno tutti dentro quella accensione.
- **Finestra di manutenzione**: la radio è di `Maintenance`, lo scheduler gira
  in `Radio::ESTERNA` e `wifiOn()` non viene mai chiamata. La macchina a stati
  chiama `Clock::sincronizza(0)`, **non bloccante**, nei due punti in cui
  constata di avere la rete di casa con un indirizzo: `apri(Stato::ServerSta)`
  e la risalita della STA nel ramo `ServerAp`. Bloccare lì congelerebbe web
  server e access point durante un possibile upload.

## Perchè proprio così

- **`configTzTime` e non `configTime`**: la seconda deriva la stringa TZ dagli
  offset e **sovrascriverebbe** il fuso, perdendo il DST automatico di
  Europe/Rome e rompendo tutti i `localtime_r()` di Weather, Calendar e Mail.
  Passando `CLOCK_POSIX_TZ` a `configTzTime` si riapplica in modo idempotente
  lo stesso fuso di `begin()`.
- **Timeout mai sotto i 10 s** (default 12000): il core è compilato con
  `CONFIG_LWIP_SNTP_STARTUP_DELAY=y` e `MAXIMUM_STARTUP_DELAY=5000`, quindi la
  prima richiesta parte con un ritardo casuale fra 0 e 5 s. Un timeout di 5 s
  scade prima che il pacchetto sia uscito, dando un fallimento sistematico e
  inspiegabile.
- **Una sync riuscita per boot basta**: `millis()` è `esp_timer_get_time()/1000`
  e `time()` poggia sulla stessa base più l'offset di `settimeofday`; quella
  base avanza in light sleep. `sntp` **non va mai fermato**: resta in polling
  con `CONFIG_LWIP_SNTP_UPDATE_DELAY=10800000` e corregge il drift, che non è
  trascurabile perchè il WROOM-32E non ha cristallo da 32 kHz e il core è su
  `CONFIG_RTC_CLK_SRC_INT_RC=y`.
- **Soglia 1700000000 e non 100000**: quest'ultima corrisponde a 27,7 h di
  uptime, oltre le quali un clock mai sincronizzato diventa indistinguibile da
  uno valido.

## Chi dipende dall'ora assoluta

- `Scheduler::inFascia()` e la cadenza `GIORNALIERA` del cinema: senza orologio
  la fascia è **fail-open** e la giornaliera resta sospesa, perchè
  `consumaSlot()` non registra `tm_yday` con un'ora finta.
- **I due task calendario**, che lo dichiarano con `pronto = calendarioPronto`,
  cioè `Clock::valido()`. `Calendar::detail::nowUtcEpoch()` ritorna 0 a
  orologio non valido e le query filtrerebbero da `1970-01-01T00:00:00Z`:
  ordinando per data il device riceverebbe i **primi** eventi del calendario
  invece dei prossimi, con una risposta HTTP 200 che lo scheduler
  registrerebbe come successo, chiudendo lo slot per la cadenza piena. Con la
  precondizione il task resta sospeso, lo slot intatto, e diventa dovuto da sè
  appena l'ora è vera.
- `Tuya::readyToPublish()` e `Tuya::runPublish()`: TuyaLink firma un timestamp
  Unix, quindi senza ora vera il publish non parte (vedi [[tuya_module]]).
- `Mail::draw()` usa l'ora solo per formattare, e la sua soglia è la stessa
  `TIME_VALID_EPOCH_MIN`.

**Non può andare in stallo**: meteo e mail non dipendono dall'ora e continuano
ad aprire la finestra radio, dove la sincronizzazione avviene. È la stessa
ragione per cui `inFascia()` è fail-open.

**Modo di fallire su una rete che blocca NTP**: l'orologio non diventa mai
valido, i due calendari non girano affatto e la lista eventi resta ai
placeholder. È voluto: è meglio di appuntamenti vecchi presentati come
prossimi. Meteo, mail e cinema restano operativi, perchè `wifiOn()` ignora il
ritorno della sincronizzazione e la connessione riesce lo stesso.

## Perchè il salto d'ora non rompe niente

Nessun modulo usa `time()` per misurare intervalli: le cadenze girano su
`millis()` con sottrazione signed, nello `Scheduler` e dentro `Indoor.h`, e i
consumer sopra leggono l'ora solo come data e ora del giorno. È la premessa che
rende sicuro sincronizzare a metà vita del dispositivo.

La data sul pannello non dipende da questa sincronizzazione:
`Weather::detail::renderFrame()` passa a `Calendar::draw()` l'epoch del campo
`dt` di OpenWeather, non `time(nullptr)`, e `recordHistory()` usa la stessa
sorgente.

## How to apply

Se serve un'ora reale in un modulo nuovo, non aggiungere una seconda
sincronizzazione e non riscrivere il confronto con la soglia: chiamare
`Clock::valido()`. Se il flusso è un task, la precondizione va nell'hook
`pronto()` della sua riga di tabella, che è il modo in cui il difetto è stato
reso inesprimibile; una guardia dentro l'esecuzione resta come difesa, non
come meccanismo. Non chiamare `configTime()` da nessuna parte, per il motivo
sul TZ spiegato sopra.

Correlate: [[scheduler_task_table]], [[maintenance_window]], [[tuya_module]],
[[timings_nvs_config]], [[timing_chain]].

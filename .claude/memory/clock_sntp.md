---
name: Orologio di sistema e SNTP
description: timeIsValid/ensureTimeSynced nel .ino con configTzTime; soglia 1700000000, timeout >= 10 s per lo startup delay lwIP, una sync per boot; ripara finestra WiFi, daily cinema e filtro temporale dei calendari
type: project
---

Il `.ino` sincronizza l'orologio via SNTP con due funzioni accanto a
`wifiOn()`/`wifiOff()`/`isActiveHour()`:

- `timeIsValid()`: `time(nullptr) >= TIME_VALID_EPOCH_MIN` (1700000000, nov 2023);
- `ensureTimeSynced(uint32_t timeout_ms = TIME_SYNC_TIMEOUT_MS)`: no-op se l'ora
  è già valida, altrimenti `configTzTime(CAL_POSIX_TZ, NTP_SERVER_1, NTP_SERVER_2)`
  più attesa; backoff `TIME_SYNC_RETRY_MS` fra tentativi falliti.

Chiamata in due punti: dentro `wifiOn()` a connessione riuscita (bloccante), e
nel ramo della finestra OTA con `ensureTimeSynced(0)`, cioè **non bloccante**.

## Perchè proprio così

- **`configTzTime` e non `configTime`**: la seconda deriva la stringa TZ dagli
  offset e **sovrascriverebbe `CAL_POSIX_TZ`**, perdendo il DST automatico di
  Europe/Rome e rompendo tutti i `localtime_r()` di Weather, Calendar e Mail.
  Passando `CAL_POSIX_TZ` a `configTzTime` si riapplica in modo idempotente lo
  stesso fuso di `Calendar::initTimezone()`.
- **Timeout mai sotto i 10 s** (default 12000): il core è compilato con
  `CONFIG_LWIP_SNTP_STARTUP_DELAY=y` e `MAXIMUM_STARTUP_DELAY=5000`, quindi la
  prima richiesta parte con un ritardo casuale fra 0 e 5 s. Un timeout di 5 s
  scade prima che il pacchetto sia uscito, dando un fallimento sistematico e
  inspiegabile.
- **Non bloccante in finestra OTA**: lì il loop gira ogni ~10 ms e attendere
  congelerebbe AP e web server proprio mentre un upload firmware potrebbe essere
  in corso; con SNTP fallito il backoff da 60 s produrrebbe più blocchi dentro i
  tre minuti di finestra. Con timeout 0 la richiesta parte e il polling di lwIP
  la porta a termine. Il prezzo è che i fetch di quella prima passata possono
  girare ancora senza ora valida, e il ramo normale li rifà allineati poco dopo.
- **Sincronizzazione dentro `wifiOn()` e non altrove**: nel ramo normale tutti i
  fetch stanno dentro `if (wifiOn())`, quindi da lì in avanti vedono l'ora vera,
  compreso `fetchCinemaImage()` che memorizza `tm_yday`.
- **Una sync riuscita per boot basta**: `millis()` è `esp_timer_get_time()/1000`
  e `time()` poggia sulla stessa base più l'offset di `settimeofday`; che quella
  base avanzi in light sleep lo dimostra il firmware in produzione, dove cadenze
  da 10 min scattano attraverso sleep da 5. `sntp` **non va mai fermato**: resta
  in polling con `CONFIG_LWIP_SNTP_UPDATE_DELAY=10800000` e corregge il drift,
  che non è trascurabile perchè il WROOM-32E non ha cristallo da 32 kHz e il
  core è su `CONFIG_RTC_CLK_SRC_INT_RC=y`.
- **Soglia 1700000000 e non 100000**: quest'ultima corrisponde a 27,7 h di
  uptime, oltre le quali un clock mai sincronizzato diventa indistinguibile da
  uno valido.

## Cosa ripara, cioè cosa era rotto

Il firmware non aveva alcuna sorgente di ora assoluta: nessun `configTime`,
`configTzTime` o `settimeofday` in nessun file; `Calendar::initTimezone()`
applica solo il fuso; il core chiama `sntp_init()` esclusivamente dentro
`configTime`/`configTzTime` e non invoca `esp_sntp_servermode_dhcp`. Quindi
`time(nullptr)` era l'uptime contato dal 1970. Tre conseguenze, tutte ora
risolte:

1. **`isActiveHour()`**: con la vecchia soglia lasciava passare tutto per le
   prime 27,7 h, poi ricavava `tm_hour` da un'ora finta che cicla con l'uptime,
   bloccando i fetch per circa 7 "ore" su 24. Ora la finestra
   `WIFI_ACTIVE_HOUR_START..END` è effettiva, e **di notte la radio non si
   accende più**: per la telemetria Tuya esiste `TUYA_IGNORE_ACTIVE_HOUR`.
2. **`shouldFetchCinema()`**: il trigger giornaliero su `CINEMA_DAILY_FETCH_HOUR`
   scattava a un'ora arbitraria o mai, rendendo inutile il keep-warm alle 06:55
   (vedi [[timing_chain]]).
3. **`Calendar::detail::nowUtcEpoch()`** (soglia `1000000000`) ritornava sempre
   0, quindi le query Outlook e Google filtravano da `1970-01-01T00:00:00Z`:
   ordinando per `start/dateTime` il device riceveva i **primi** `MAX_EVENTS`
   eventi del calendario, non i prossimi. Ora il riquadro eventi mostra gli
   appuntamenti futuri. Stessa soglia in `Mail::draw()`, che teneva `today`
   azzerato.

## Perchè il salto d'ora non rompe niente

Nessun modulo usa `time()` per misurare intervalli: tutte le cadenze girano su
`millis()` con sottrazione signed (`Weather.h`, `Mail.h`, `Indoor.h`,
`Tuya.h`), e i consumer sopra leggono l'ora solo come data e ora del giorno. È
la premessa che rende sicuro sincronizzare a metà vita del dispositivo.

`Calendar.h` e `Mail.h` **non sono stati toccati**: le loro soglie erano già
corrette e si sistemano da sè.

La data sul pannello non dipende da questa correzione: `Weather::detail::renderFrame()`
passa a `Calendar::draw()` l'epoch del campo `dt` di OpenWeather, non
`time(nullptr)`, e `recordHistory()` usa la stessa sorgente.

## How to apply

Se serve un'ora reale in un modulo nuovo, non aggiungere una seconda
sincronizzazione: chiamare `timeIsValid()` e affidarsi a quella di `wifiOn()`,
oppure replicare la guardia difensiva di `Tuya::detail::clockIsValid()`, che
salta l'operazione senza ritentare. Non chiamare `configTime()` da nessuna
parte, per il motivo sul TZ spiegato sopra.

Correlate: [[tuya_module]] (l'autenticazione TuyaLink firma un timestamp Unix ed
è il motivo per cui questa sincronizzazione esiste), [[timing_chain]],
[[layout_invariants]].

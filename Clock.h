#ifndef CLOCK_H
#define CLOCK_H

#include <Arduino.h>
#include <time.h>
// Assente solo sul toolset MSVC dei test su host, dove lo stub di esp_sntp.h
// fornisce timeval e gettimeofday.
#if __has_include(<sys/time.h>)
  #include <sys/time.h>
#endif
#include <stdint.h>
#include <esp_sntp.h>

#include "Timings.h"
#include "Log.h"

/**
 * Orologio di sistema: fuso orario e sincronizzazione SNTP.
 *
 * Serve un'ora assoluta alla fascia oraria della radio, al trigger giornaliero
 * del fetch cinema e alle query dei calendari, che filtrano gli eventi da
 * "adesso". Senza sincronizzazione time() conta dal 1970 partendo dal boot,
 * cioè restituisce l'uptime.
 *
 * Il modulo esiste perchè la sincronizzazione non appartiene a nessuno dei
 * suoi chiamanti: la radio in questo firmware ha due proprietari, lo scheduler
 * nel ciclo normale e la finestra di manutenzione al boot, e ciascuno deve
 * poter dire "adesso sono in rete" senza che l'orologio viva dentro il codice
 * dell'altro.
 *
 * API:
 *   - Clock::begin()        : applica il fuso. Nessuna rete, va in setup();
 *   - Clock::valido()       : vero se l'ora è stata sincronizzata;
 *   - Clock::sincronizza()  : avvia SNTP, presuppone la rete su.
 *
 * Le soglie e i server (TIME_*, NTP_SERVER_*) stanno in Timings.h con tutti
 * gli altri tempi del firmware.
 */

/**
 * POSIX TZ string per Europe/Rome con gestione automatica del DST.
 *  - Standard: CET (UTC+1), da ultima domenica di ottobre alle 03:00 locali
 *    a ultima domenica di marzo.
 *  - Estiva:   CEST (UTC+2), da ultima domenica di marzo alle 02:00 UTC
 *    a ultima domenica di ottobre alle 03:00 locali.
 *
 * Applicata da Clock::begin() via setenv("TZ", ...) + tzset(). Dopo l'init,
 * tutte le chiamate a localtime_r() del progetto ritornano componenti locali
 * gia' corretti per il DST in corso.
 *
 * Non sta in Env.h perchè non è una credenziale: è un parametro di dominio.
 */
#ifndef CLOCK_POSIX_TZ
  #define CLOCK_POSIX_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
#endif

namespace Clock
{
  namespace detail
  {
    /** Scritti dalla callback di SNTP, che gira nel task di lwIP, e letti dal
     *  loop: valori a 32 bit e bool, quindi load e store atomici. */
    inline volatile bool     everSynced = false;
    inline volatile uint32_t lastSyncMs = 0;

    /** Base della misura di deriva, scritta dal loop prima di sntp_restart() e
     *  letta dalla callback dopo: le due cose sono sequenziali. */
    inline volatile bool measuring      = false;
    inline int64_t       expectedBaseUs = 0;
    inline uint32_t      expectedBaseMs = 0;
    inline int64_t       prevSyncUs     = 0;

    /** Microsecondi Unix di una struct timeval. */
    inline int64_t toUs(const struct timeval& tv)
    {
      return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
    }

    /**
     * Callback di sincronizzazione avvenuta, invocata da lwIP dopo aver
     * impostato l'ora. Se la sincronizzazione l'ha chiesta resyncIfStale(),
     * confronta l'ora ricevuta con quella attesa, cioè con l'ora di sistema
     * prima della correzione: base della richiesta più il tempo trascorso, che
     * millis() conta sulla stessa base dei tempi. La differenza è la deriva
     * accumulata dall'RC in light sleep dalla sincronizzazione precedente.
     */
    inline void onTimeSync(struct timeval* tv)
    {
      const uint32_t nowMs = millis();
      const int64_t receivedUs = toUs(*tv);
      if (measuring && everSynced)
      {
        const int64_t expectedUs = expectedBaseUs + (int64_t)(uint32_t)(nowMs - expectedBaseMs) * 1000LL;
        const int64_t correctionUs = receivedUs - expectedUs;
        const int64_t spanUs = expectedUs - prevSyncUs;
        const long ppm = spanUs > 0 ? (long)(correctionUs * 1000000LL / spanUs) : 0;
        LOG("Time", "SNTP: correzione %+lld ms dopo %lld min (%+ld ppm)",
            (long long)(correctionUs / 1000LL), (long long)(spanUs / 60000000LL), ppm);
      }
      else
      {
        LOGV("Time", "SNTP: sincronizzazione periodica");
      }
      measuring  = false;
      prevSyncUs = receivedUs;
      lastSyncMs = nowMs;
      everSynced = true;
    }
  }

  /**
   * Applica la stringa POSIX TZ al processo: da qui in avanti localtime_r()
   * ritorna componenti in Europe/Rome con DST automatico. Registra anche la
   * callback di sincronizzazione, che deve esistere prima del primo SNTP.
   *
   * Va chiamata in setup() PRIMA di qualsiasi modulo che formatti orari locali
   * (Weather::begin, Calendar::Outlook::begin, ecc.). Non tocca la rete.
   */
  inline void begin()
  {
    setenv("TZ", CLOCK_POSIX_TZ, 1);
    tzset();
    sntp_set_time_sync_notification_cb(detail::onTimeSync);
  }

  /**
   * Vero se l'orologio di sistema è stato sincronizzato, cioè se time()
   * supera TIME_VALID_EPOCH_MIN. Prima della sincronizzazione time()
   * restituisce l'uptime contato dal 1970, che dopo 27,7 h di accensione
   * diventa indistinguibile da un'ora reale.
   *
   * È l'unica definizione di "ora valida" del firmware: ci poggiano lo
   * scheduler, la precondizione dei task calendario e le guardie dei moduli.
   */
  inline bool valido()
  {
    return time(nullptr) >= TIME_VALID_EPOCH_MIN;
  }

  /**
   * Sincronizza l'orologio via SNTP se non lo è già. Presuppone la rete su:
   * va chiamata da chi ha appena constatato di averla, cioè wifiOn() nel ciclo
   * normale e la macchina a stati della finestra di manutenzione quando la STA
   * sale. Quando l'ora è già valida costa un solo confronto.
   *
   * Usa configTzTime() e non configTime(): la prima applica la stringa POSIX
   * che le viene passata, quindi ripassando CLOCK_POSIX_TZ riapplica lo stesso
   * fuso di begin(); la seconda deriverebbe il TZ dagli offset e lo
   * sovrascriverebbe, perdendo il DST automatico di Europe/Rome.
   *
   * Una sincronizzazione riuscita per boot basta: il light sleep mantiene la
   * base temporale su cui poggiano time() e millis(), quindi l'ora sopravvive
   * al sonno. Il drift dell'RTC, che su questo modulo gira sull'oscillatore RC
   * interno perchè non c'è il cristallo da 32 kHz, viene corretto dal polling
   * SNTP di lwIP: riprova ogni 3 h e va a buon fine appena cade in una
   * finestra con radio accesa, per questo sntp non viene mai fermato.
   *
   * @param timeout_ms attesa massima della prima sincronizzazione. A 0 avvia
   *        SNTP e ritorna subito, lasciando che il polling di lwIP allinei
   *        l'ora entro pochi secondi: serve alla finestra di manutenzione,
   *        dove bloccare congelerebbe web server e access point.
   * @return true se l'ora è valida al ritorno.
   */
  inline bool sincronizza(uint32_t timeout_ms = TIME_SYNC_TIMEOUT_MS)
  {
    static uint32_t ultimoTentativoMs = 0;
    static bool tentato = false;

    if (valido())
      return true;
    // Backoff fra tentativi falliti: senza, la finestra di manutenzione
    // riavvierebbe SNTP a ogni giro da 10 ms.
    if (tentato && (int32_t)(millis() - ultimoTentativoMs) < (int32_t)TIME_SYNC_RETRY_MS)
      return false;

    ultimoTentativoMs = millis();
    tentato = true;

    configTzTime(CLOCK_POSIX_TZ, NTP_SERVER_1, NTP_SERVER_2);

    uint32_t t0 = millis();
    while (!valido() && (millis() - t0) < timeout_ms)
    {
      delay(WIFI_ATTESA_POLL_MS);
    }

    if (!valido())
    {
      // Con timeout_ms a 0 non è un errore: la richiesta è partita e il
      // polling SNTP di lwIP la porta a termine senza bloccare il chiamante.
      if (timeout_ms == 0)
        LOG("Time", "SNTP avviato, sincronizzazione in corso");
      else
        LOG("Time", "SNTP timeout");
      return false;
    }

    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    LOG("Time", "SNTP ok: %04d-%02d-%02d %02d:%02d:%02d locale",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec);
    return true;
  }

  /**
   * Forza una nuova sincronizzazione se l'ultima è più vecchia di
   * CLOCK_RESYNC_INTERVAL_MS. Va chiamata da chi ha appena portato su la radio,
   * cioè wifiOn() nel .ino: il polling di lwIP ogni 3 h riesce solo se cade
   * dentro una finestra radio, mentre qui la richiesta parte quando la rete
   * c'è. Non blocca: lwIP manda la richiesta entro pochi secondi e la risposta
   * arriva mentre girano i fetch.
   */
  inline void resyncIfStale()
  {
    using namespace detail;
    if (!valido() || !everSynced) return;
    // Una richiesta precedente rimasta senza risposta non blocca questa: la
    // base si aggiorna e la misura resta corretta.
    if ((uint32_t)(millis() - lastSyncMs) < CLOCK_RESYNC_INTERVAL_MS) return;

    struct timeval now;
    gettimeofday(&now, nullptr);
    expectedBaseUs = toUs(now);
    expectedBaseMs = millis();
    measuring = true;
    if (!sntp_restart())
    {
      measuring = false;
      LOG("Time", "SNTP non attivo: risincronizzazione non avviata");
      return;
    }
    LOGV("Time", "SNTP: risincronizzazione avviata");
  }
}

#endif // CLOCK_H

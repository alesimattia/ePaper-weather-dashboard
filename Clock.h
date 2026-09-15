#ifndef CLOCK_H
#define CLOCK_H

#include <Arduino.h>
#include <time.h>
#include <stdint.h>

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
  /**
   * Applica la stringa POSIX TZ al processo: da qui in avanti localtime_r()
   * ritorna componenti in Europe/Rome con DST automatico. Idempotente.
   *
   * Va chiamata in setup() PRIMA di qualsiasi modulo che formatti orari locali
   * (Weather::begin, Calendar::Outlook::begin, ecc.). Non tocca la rete.
   */
  inline void begin()
  {
    setenv("TZ", CLOCK_POSIX_TZ, 1);
    tzset();
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
}

#endif // CLOCK_H

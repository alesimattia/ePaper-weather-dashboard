#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <time.h>
#include <stdint.h>

#include "Timings.h"
#include "Log.h"
#include "Clock.h"

/**
 * Scheduler centralizzato di tutti i flussi temporizzati, costruito attorno a
 * un solo orologio: il ciclo del sensore.
 *
 * BSEC accetta un'interrogazione del BME680 ogni BSEC_PERIODO_ULP_S, con uno
 * scarto massimo del 6,25% (guida Bosch AN011), e non si lascia nè anticipare
 * nè rimandare: è quindi lui a scandire il tempo, e tutto il resto si conta in
 * cicli. Senza sensore il ciclo è interno e dura lo stesso CYCLE_S. Lo è anche
 * con un sensore senza scadenza propria, come il BME280, che non registra
 * msUntilSensorPoll: si legge all'inizio di ogni ciclo dentro la fascia WiFi.
 * Ogni ciclo esegue tre passi, sempre in quest'ordine:
 *
 *   1. sensore: l'interrogazione, così ciò che segue usa il campione nuovo;
 *   2. rete: il passo di rete, vedi sotto, con i task dovuti in questo ciclo
 *      nell'ordine della tabella e gli onRoundStart() prima dei run();
 *   3. frame: il ridisegno del pannello, che quindi mostra sempre i dati
 *      appena prodotti.
 *
 * Il passo di rete ha quattro fasi: Idle (decide se serve), Acquiring (radio
 * connessa e ora assestata), Running (i task) e Done. Il frame parte solo a
 * passo concluso: la regola è una sola per entrambi i proprietari della radio,
 * e nessun budget è ancorato al boot ma all'evento che limita (l'inizio del
 * passo, la STA vista). "Radio acquisita" vuol dire connessa e con l'ora valida
 * oppure con il budget dell'ora scaduto, così l'insieme dei task dovuti si
 * calcola con l'ora già nota e i task che la richiedono non restano fuori per
 * aver gareggiato con SNTP.
 *
 * Le cadenze dei task sono multipli del ciclo e si contano in cicli dallo slot
 * precedente, mai in millisecondi, quindi non accumulano deriva: un task da 10
 * minuti cade un frame sì e uno no, per sempre. Un task fallito ritenta una
 * sola volta, al ciclo successivo e mai nella stessa accensione, così nessun
 * fallimento ritarda il frame. Lo slot riparte pulito a ogni nuova attivazione
 * del task, cioè quando la sua precondizione diventa vera e al primo ciclo
 * della giornata: un ritento lasciato in sospeso da una fascia, da un
 * forzamento o dalla notte non si trascina in quella dopo. Un task che ha
 * avviato qualcosa che matura solo fra un ciclo, come il risveglio di un
 * server, si rinvia al ciclo successivo: lo slot si apre lì e nessun
 * tentativo si spende.
 *
 * Fuori dalla fascia WiFi resta solo il passo del sensore, il minimo che
 * conserva l'accuratezza di BSEC: niente radio e niente frame fino
 * all'apertura della fascia. Senza un sensore con scadenza propria non resta
 * nemmeno quello, e si dorme fino all'apertura.
 *
 * La scadenza del sensore ha la precedenza su tutto: lo sleep dura fino a lei
 * esattamente, e la si controlla anche fra un task di rete e l'altro e prima
 * del frame, perchè la fase di rete nel caso peggiore si avvicina al ciclo.
 *
 * Non possiede la radio nè il sensore nè il pannello: li usa attraverso gli
 * hook passati a begin(). Durante la finestra di manutenzione non accende nè
 * spegne la radio: attende la STA e l'ora, poi la usa.
 */
namespace Scheduler
{
  /**
   * Esito di un task nel giro corrente.
   *
   *   OK            eseguito con successo.
   *   FAILED        guasto imputabile al task: consuma un tentativo.
   *   SKIPPED       niente da fare, o guasto a monte (radio giù): lo stato
   *                 resta intatto e il tentativo non viene speso.
   *   DEFERRED      avviato qualcosa che matura solo al ciclo successivo:
   *                 lo slot si apre qui, il tentativo non viene speso e il
   *                 task resta dovuto al ciclo dopo.
   */
  enum class Outcome : uint8_t { OK, FAILED, SKIPPED, DEFERRED };

  /** Di chi è la radio durante il giro. */
  enum class RadioMode : uint8_t
  {
    MANAGED_BY_SCHEDULER,    // la accende e la spegne lo scheduler
    MANAGED_BY_MAINTENANCE   // è della finestra di manutenzione: si usa se c'è, non si tocca
  };

  /** Scadenza non calcolabile o non applicabile. */
  inline constexpr uint32_t NEVER = UINT32_MAX;

  /**
   * Descrittore di un task di rete. La parte immutabile è la configurazione,
   * `state` è la sola parte che lo scheduler modifica.
   *
   * Gli hook opzionali (nullptr = assente):
   *   preconditionsMet()  precondizione non temporale; falso sospende il task,
   *                       e il ritorno a vero apre uno slot nuovo e subito dovuto.
   *   hasIncompleteData() vero quando l'esecuzione è riuscita ma i dati non
   *                       bastano ancora: lo slot non si chiude e si ritenta.
   *   onRoundStart()      eseguito per TUTTI i task dovuti prima di qualunque
   *                       run(): serve a chi deve avviare qualcosa e lasciarlo
   *                       maturare mentre gli altri girano.
   */
  struct Task
  {
    const char* tag;

    uint16_t Timings::Valori::*intervalMinutes;     // multiplo di CYCLE_MIN

    uint8_t  maxAttempts;                           // 0 = un tentativo per slot, comunque vada

    Outcome  (*run)();
    bool     (*preconditionsMet)();
    bool     (*hasIncompleteData)();
    void     (*onRoundStart)();

    struct State
    {
      bool     neverRun          = true;   // esplicito: nessuna sentinella sui cicli
      uint32_t slotCycle         = 0;      // ciclo in cui si è aperto l'ultimo slot chiuso
      uint32_t openCycle         = 0;      // ciclo in cui si è aperto lo slot in corso
      bool     attempted         = false;  // vero se attemptCycle è significativo
      uint32_t attemptCycle      = 0;      // ciclo dell'ultimo tentativo
      bool     retrying          = false;
      bool     deferred          = false;  // rinviato al ciclo successivo da un esito DEFERRED
      uint8_t  failedAttempts    = 0;      // falliti consecutivi dentro lo slot corrente
      bool     eligible          = false;  // esito dell'ultima valutazione della precondizione
      uint32_t outcomeCount = 0, okCount = 0, failedCount = 0;
    } state;
  };

  /**
   * Hook verso la piattaforma e i moduli, tutti obbligatori salvo indicazione.
   * Il sensore è opzionale: con sensorActive() falso o assente il ciclo è
   * interno, ogni CYCLE_S dall'inizio del precedente.
   * Anche msUntilSensorPoll è opzionale e dice se il sensore ha una scadenza
   * propria. Se c'è (BSEC) è l'orologio del ciclo e pollSensor() si chiama a
   * ogni ciclo, anche di notte. Se manca (BME280) il ciclo è interno e
   * pollSensor() si chiama solo all'inizio dei cicli dentro la fascia WiFi.
   */
  struct Hooks
  {
    bool     (*radioOn)()           = nullptr;
    void     (*radioOff)()          = nullptr;
    bool     (*sensorActive)()      = nullptr;
    uint32_t (*msUntilSensorPoll)() = nullptr;
    void     (*pollSensor)()        = nullptr;
    void     (*drawFrame)()         = nullptr;
  };

  namespace detail
  {
    inline Task*    tasks     = nullptr;
    inline uint8_t  taskCount = 0;
    inline Hooks    hooks     = {};

    inline uint32_t cycle            = 0;      // numero del ciclo corrente, dal primo dopo il boot
    inline bool     cycleStarted     = false;
    inline uint32_t cycleStartMs     = 0;
    inline bool     framePending     = false;  // il ciclo corrente non ha ancora il suo frame
    inline int16_t  lastActiveDay    = -1;     // tm_yday dell'ultimo ciclo dentro la fascia
    inline bool     ranTaskThisRound = false;
    inline uint32_t radioRounds      = 0;

    /** Fasi del passo di rete di un ciclo. */
    enum class NetPhase : uint8_t { IDLE, ACQUIRING, RUNNING, DONE };

    inline NetPhase netPhase       = NetPhase::IDLE;
    inline bool     frameGate      = false;  // un passo di rete è concluso e il frame può partire
    inline uint32_t acquireStartMs = 0;      // inizio dell'acquisizione della radio nel passo corrente
    inline bool     staSeen        = false;  // la STA è stata vista connessa in questo passo
    inline uint32_t staSeenMs      = 0;
    inline bool     runStarted     = false;  // gli onRoundStart() del passo sono già partiti
    inline bool     stepRanTask    = false;  // nel passo corrente almeno un task ha eseguito
    inline bool     clockAttempted = false;  // un passo di rete ha già acceso la radio con l'ora non valida
    inline uint32_t clockAttemptCycle = 0;   // ciclo di quel passo

    // Diagnostica del primo frame: istanti assoluti di millis(), -1 se non ancora avvenuti.
    inline int32_t     diagStaMs        = -1;
    inline int32_t     diagClockMs      = -1;
    inline const char* stepOutcome      = "in corso";
    inline bool        firstFrameLogged = false;

    // Ultimo giro valutato durante la finestra di manutenzione, vedi runRound().
    inline bool     maintenanceRoundDone   = false;
    inline uint32_t lastMaintenanceRoundMs = 0;

    /** Confronto rollover-safe sulla differenza signed di millis(). */
    inline bool hasElapsed(uint32_t nowMs, uint32_t sinceMs, uint32_t thresholdMs)
    {
      return (int32_t)(nowMs - sinceMs) >= (int32_t)thresholdMs;
    }

    /** Millisecondi mancanti a `atMs`, 0 se già passato. */
    inline uint32_t msRemaining(uint32_t nowMs, uint32_t atMs)
    {
      const int32_t d = (int32_t)(atMs - nowMs);
      return d <= 0 ? 0 : (uint32_t)d;
    }

    /**
     * Ora locale corrente scritta in `buffer`, oppure nullptr con l'orologio non
     * sincronizzato. Unico punto in cui lo scheduler ricava il "giorno di oggi"
     * che passa alle valutazioni delle scadenze.
     */
    inline const struct tm* localNow(struct tm& buffer)
    {
      if (!Clock::valido()) return nullptr;
      const time_t now = time(nullptr);
      localtime_r(&now, &buffer);
      return &buffer;
    }

    inline bool sensorIsActive() { return hooks.sensorActive && hooks.sensorActive(); }

    /** Vero se il sensore è attivo e ha una scadenza propria, che diventa
     *  l'orologio del ciclo al posto di CYCLE_S. */
    inline bool sensorSetsCycle() { return sensorIsActive() && hooks.msUntilSensorPoll; }

    /** Vero se l'ora non è valida e da CLOCK_RECOVERY_CYCLES cicli nessun passo
     *  ha acceso la radio: SNTP si sincronizza solo con la radio accesa, e i
     *  task che chiedono l'ora non diventerebbero mai dovuti. */
    inline bool isClockRecoveryDue()
    {
      return !Clock::valido() &&
             (!clockAttempted || cycle - clockAttemptCycle >= (uint32_t)CLOCK_RECOVERY_CYCLES);
    }

    /** Numero di cicli fra due slot di un task periodico, almeno uno. */
    inline uint32_t cyclesPerSlot(const Task& t)
    {
      const uint32_t n = (uint32_t)(Timings::get().*t.intervalMinutes) / (uint32_t)CYCLE_MIN;
      return n == 0 ? 1 : n;
    }
  }

  /**
   * Vero dentro la fascia oraria in cui la radio si accende e il pannello si
   * ridisegna. Fail-open a orologio non sincronizzato: senza questa eccezione
   * il primo SNTP non potrebbe mai avvenire, perchè richiede la radio.
   */
  inline bool isInsideRadioHours()
  {
    struct tm buffer;
    const struct tm* local = detail::localNow(buffer);
    if (!local) return true;
    const auto& v = Timings::get();
    return local->tm_hour >= v.wifiOraInizio && local->tm_hour <= v.wifiOraFine;
  }

  /**
   * Millisecondi fino alla prossima occorrenza locale di `hour`:00.
   * mktime() con tm_isdst = -1 normalizza il giorno e assorbe il cambio
   * dell'ora legale. La differenza si calcola in interi su time_t, senza il
   * double di difftime() che sull'ESP32 è emulato in software. Presuppone
   * l'orologio sincronizzato.
   */
  inline uint32_t msUntilLocalHour(uint8_t hour, bool tomorrow)
  {
    const time_t now = time(nullptr);
    struct tm current;
    localtime_r(&now, &current);
    struct tm target = current;
    target.tm_hour = hour;
    target.tm_min = 0;
    target.tm_sec = 0;
    target.tm_isdst = -1;
    if (tomorrow || hour <= current.tm_hour) target.tm_mday += 1;
    const time_t at = mktime(&target);
    if (at == (time_t)-1 || at <= now) return 0;
    const int64_t ms = (int64_t)(at - now) * 1000;
    return ms >= (int64_t)NEVER ? NEVER - 1 : (uint32_t)ms;
  }

  /**
   * Millisecondi all'inizio del prossimo ciclo: la scadenza del sensore se è
   * attivo e ne ha una, altrimenti CYCLE_S dall'inizio del ciclo corrente. 0
   * prima del primo ciclo dopo il boot.
   */
  inline uint32_t msUntilNextCycle()
  {
    using namespace detail;
    if (!cycleStarted) return 0;
    if (sensorSetsCycle()) return hooks.msUntilSensorPoll();
    return msRemaining(millis(), cycleStartMs + (uint32_t)CYCLE_S * 1000UL);
  }

  /** Numero del ciclo corrente, per i log e per i test. */
  inline uint32_t currentCycle() { return detail::cycle; }

  /**
   * Riapre lo slot del task da zero: dovuto subito, senza ritenti, rinvii nè
   * fallimenti ereditati. Lo chiamano il fronte di salita della precondizione e
   * il primo ciclo della giornata, cioè ogni nuova attivazione del task.
   */
  inline void restartSlot(Task& t)
  {
    t.state.neverRun = true;
    t.state.retrying = false;
    t.state.deferred = false;
    t.state.failedAttempts = 0;
  }

  /**
   * Vero se il task va eseguito nel ciclo corrente: precondizione vera, al più
   * un tentativo per ciclo, e i cicli della cadenza trascorsi dallo slot
   * precedente oppure un ritento in sospeso. Un rinvio non tocca nessuna di
   * queste condizioni, quindi il task rinviato resta dovuto al ciclo dopo.
   * Registra anche l'esito della precondizione e, sul suo fronte di salita,
   * riapre lo slot: per questo non è const, e resta idempotente se chiamata più
   * volte nello stesso giro.
   */
  inline bool isDue(Task& t)
  {
    Task::State& s = t.state;
    const bool eligible = !t.preconditionsMet || t.preconditionsMet();
    if (eligible && !s.eligible) restartSlot(t);
    s.eligible = eligible;
    if (!eligible) return false;
    if (s.attempted && s.attemptCycle == detail::cycle) return false;
    if (s.retrying) return true;
    return s.neverRun || detail::cycle - s.slotCycle >= detail::cyclesPerSlot(t);
  }

  namespace detail
  {
    /** Vero se almeno un task di rete è dovuto nel ciclo corrente. */
    inline bool anyTaskDue()
    {
      for (uint8_t i = 0; i < taskCount; ++i)
        if (isDue(tasks[i])) return true;
      return false;
    }

    /** Chiude lo slot corrente: da qui il task non ritenta e la cadenza riparte
     *  dal ciclo in cui lo slot si era aperto. */
    inline void closeSlot(Task& t)
    {
      t.state.slotCycle = t.state.openCycle;
      t.state.neverRun = false;
      t.state.retrying = false;
      t.state.deferred = false;
      t.state.failedAttempts = 0;
    }

    /**
     * Apre un nuovo ciclo se la sua scadenza è arrivata: interroga il sensore
     * (sempre se ha una scadenza propria, altrimenti solo dentro la fascia) e,
     * dentro la fascia, prenota il frame. Il primo ciclo di un giorno dentro la
     * fascia riapre lo slot di tutti i task, così il primo frame del mattino ha
     * dati nuovi e nessun ritento della sera prima si trascina.
     * @return true se il ciclo è stato aperto.
     */
    inline bool startCycleIfDue(const struct tm* today)
    {
      if (msUntilNextCycle() != 0) return false;
      ++cycle;
      cycleStarted = true;
      cycleStartMs = millis();
      netPhase = NetPhase::IDLE;
      frameGate = false;
      // Un sensore senza scadenza propria si legge solo per i cicli che la
      // useranno: di notte niente frame nè Tuya, quindi niente lettura.
      if (sensorIsActive() && hooks.pollSensor && (sensorSetsCycle() || isInsideRadioHours()))
        hooks.pollSensor();
      if (isInsideRadioHours())
      {
        framePending = true;
        if (today)
        {
          if (lastActiveDay >= 0 && today->tm_yday != lastActiveDay)
          {
            for (uint8_t i = 0; i < taskCount; ++i) restartSlot(tasks[i]);
            LOG("sched", "ciclo %lu: primo della giornata, tutti i task dovuti", (unsigned long)cycle);
          }
          lastActiveDay = (int16_t)today->tm_yday;
        }
      }
      else
      {
        framePending = false;
      }
      LOGV("sched", "ciclo %lu", (unsigned long)cycle);
      return true;
    }

    /** Il passo del sensore fuori sequenza, se la sua scadenza è arrivata
     *  mentre il giro faceva altro. */
    inline void serviceSensorDeadline()
    {
      struct tm buffer;
      startCycleIfDue(localNow(buffer));
    }

    /** Descrizione leggibile della prossima scadenza, per i log. */
    inline const char* describeNextSlot(const Task& t)
    {
      static char buf[40];
      snprintf(buf, sizeof buf, "fra %lu cicli", (unsigned long)cyclesPerSlot(t));
      return buf;
    }
  }

  /**
   * Registra l'esito di un task e decide se lo slot si chiude o si ritenta al
   * ciclo successivo. Un'esecuzione riuscita ma con dati ancora incompleti
   * conta come fallimento ai fini dello slot: è il caso del meteo che ha la
   * corrente ma non le previsioni. Un rinvio apre lo slot nel proprio ciclo, che
   * resta l'ancora della cadenza anche quando l'esito arriva al ciclo dopo.
   */
  inline void recordOutcome(Task& t, Outcome outcome, uint32_t durationMs)
  {
    if (outcome == Outcome::SKIPPED)
    {
      LOGV("sched", "%s: saltato", t.tag);
      return;
    }

    detail::ranTaskThisRound = true;
    if (!t.state.retrying && !t.state.deferred) t.state.openCycle = detail::cycle;
    t.state.attempted = true;
    t.state.attemptCycle = detail::cycle;

    if (outcome == Outcome::DEFERRED)
    {
      t.state.deferred = true;
      LOG("sched", "%s: rinviato al prossimo ciclo dopo %lu ms", t.tag, (unsigned long)durationMs);
      return;
    }
    t.state.deferred = false;
    t.state.outcomeCount++;

    const bool incomplete = (outcome != Outcome::FAILED) && t.hasIncompleteData && t.hasIncompleteData();
    const bool countsAsFailure = (outcome == Outcome::FAILED) || incomplete;

    if (outcome == Outcome::OK) t.state.okCount++;
    if (outcome == Outcome::FAILED) t.state.failedCount++;

    if (countsAsFailure && t.maxAttempts > 0 && t.state.failedAttempts + 1 < t.maxAttempts)
    {
      t.state.failedAttempts++;
      t.state.retrying = true;
      LOG("sched", "%s: %s in %lu ms (tentativo %u/%u), ritento al prossimo ciclo",
          t.tag, incomplete ? "dati incompleti" : "fallito",
          (unsigned long)durationMs, (unsigned)t.state.failedAttempts, (unsigned)t.maxAttempts);
      return;
    }

    const bool thresholdReached = countsAsFailure && t.maxAttempts > 0;
    detail::closeSlot(t);
    if (countsAsFailure)
      LOG("sched", "%s: %s in %lu ms%s, prossimo %s", t.tag,
          incomplete ? "dati incompleti" : "fallito", (unsigned long)durationMs,
          thresholdReached ? ", soglia raggiunta" : "", detail::describeNextSlot(t));
    else
      LOG("sched", "%s: ok in %lu ms, prossimo %s", t.tag,
          (unsigned long)durationMs, detail::describeNextSlot(t));
  }

  namespace detail
  {
    /** Esegue gli onRoundStart() dei task dovuti, tutti prima di qualunque
     *  run(): chi deve avviare qualcosa lo lascia maturare mentre gli altri girano. */
    inline void startDueTasks()
    {
      for (uint8_t i = 0; i < taskCount; ++i)
        if (isDue(tasks[i]) && tasks[i].onRoundStart) tasks[i].onRoundStart();
    }

    /**
     * Esegue i run() dei task dovuti nell'ordine della tabella, con la radio già
     * connessa. Fra un task e l'altro serve la scadenza del sensore, che una
     * fase di rete lunga potrebbe superare.
     * @return true se almeno un task ha eseguito davvero, cioè non è stato saltato.
     */
    inline bool runDueTasks()
    {
      bool ranAny = false;
      for (uint8_t i = 0; i < taskCount; ++i)
      {
        serviceSensorDeadline();
        Task& t = tasks[i];
        if (!isDue(t)) continue;

        // Radio caduta a giro iniziato: guasto a monte, non del task. Lo slot
        // resta intatto e il tentativo si spende al primo giro con la radio su.
        if (WiFi.status() != WL_CONNECTED)
        {
          LOG("sched", "%s: saltato, radio non connessa (slot intatto)", t.tag);
          recordOutcome(t, Outcome::SKIPPED, 0);
          continue;
        }

        const uint32_t startMs = millis();
        const Outcome outcome = t.run();
        recordOutcome(t, outcome, millis() - startMs);
        if (outcome != Outcome::SKIPPED) ranAny = true;
      }
      return ranAny;
    }

    /** Registra gli istanti in cui STA e ora diventano disponibili per la
     *  riga diagnostica del primo frame. */
    inline void noteBootDiagnostics(uint32_t nowMs)
    {
      if (diagStaMs < 0 && WiFi.status() == WL_CONNECTED) diagStaMs = (int32_t)nowMs;
      if (diagClockMs < 0 && Clock::valido()) diagClockMs = (int32_t)nowMs;
    }

    /** Conclude il passo di rete: da qui il frame in sospeso può partire. */
    inline void finishNetworkStep(const char* outcome)
    {
      netPhase = NetPhase::DONE;
      frameGate = true;
      stepOutcome = outcome;
      noteBootDiagnostics(millis());
    }

    /** Spegne la radio se è dello scheduler; nella finestra di manutenzione non si tocca. */
    inline void releaseRadio(RadioMode mode)
    {
      if (mode == RadioMode::MANAGED_BY_SCHEDULER && hooks.radioOff) hooks.radioOff();
    }

    /**
     * Fa avanzare il passo di rete del ciclo, a fasi, e conclude con
     * finishNetworkStep(). Con la radio dello scheduler il passo si chiude
     * dentro la stessa chiamata; nella finestra di manutenzione, dove la radio
     * non si possiede e il loop deve servire il web server, ogni chiamata
     * avanza quanto può e ritorna.
     *
     *   Idle       serve un passo solo dentro la fascia, se un task è dovuto
     *              oppure se l'ora non è valida e da CLOCK_RECOVERY_CYCLES
     *              cicli nessun passo ha acceso la radio, dove SNTP si
     *              sincronizza: senza ora sarebbe dovuto il solo meteo.
     *   Acquiring  radio dello scheduler: radioOn(), già bloccante su
     *              connessione e SNTP. Manutenzione: la STA entro
     *              RADIO_STA_ATTESA_MS dall'inizio, poi l'ora entro
     *              TIME_SYNC_TIMEOUT_MS dalla STA vista.
     *   Running    i task valutano qui le precondizioni, quindi quelli che
     *              chiedono l'ora non restano fuori per aver gareggiato con
     *              SNTP, e se l'ora cade fuori fascia un boot notturno non fa
     *              fetch. Poi una sola passata dei task: un fallimento ritenta al
     *              ciclo successivo, così il frame non aspetta mai.
     */
    inline void advanceNetworkStep(RadioMode mode)
    {
      for (;;)
      {
        const uint32_t nowMs = millis();
        noteBootDiagnostics(nowMs);

        switch (netPhase)
        {
          case NetPhase::IDLE:
          {
            if (!isInsideRadioHours())
            {
              finishNetworkStep("fuori fascia");
              return;
            }
            if (!anyTaskDue())
            {
              if (!isClockRecoveryDue())
              {
                finishNetworkStep("nessun task dovuto");
                return;
              }
              LOG("sched", "ora non valida e nessun task dovuto: radio accesa per sincronizzare l'ora");
            }
            if (!Clock::valido())
            {
              clockAttempted = true;
              clockAttemptCycle = cycle;
            }
            netPhase = NetPhase::ACQUIRING;
            acquireStartMs = nowMs;
            staSeen = false;
            runStarted = false;
            stepRanTask = false;
            continue;
          }

          case NetPhase::ACQUIRING:
          {
            if (mode == RadioMode::MANAGED_BY_SCHEDULER)
            {
              ++radioRounds;
              if (hooks.radioOn && hooks.radioOn())
              {
                netPhase = NetPhase::RUNNING;
                continue;
              }
              LOG("sched", "connessione fallita: i task di rete riprovano al prossimo ciclo");
              releaseRadio(mode);
              finishNetworkStep("radio non connessa");
              return;
            }

            if (WiFi.status() != WL_CONNECTED)
            {
              if (hasElapsed(nowMs, acquireStartMs, RADIO_STA_ATTESA_MS))
              {
                LOG("sched", "STA non connessa entro %lu s: frame senza dati di rete, i task riprovano al ciclo successivo",
                    (unsigned long)(RADIO_STA_ATTESA_MS / 1000UL));
                finishNetworkStep("STA non connessa");
              }
              return;
            }
            if (!staSeen)
            {
              staSeen = true;
              staSeenMs = nowMs;
            }
            if (!Clock::valido())
            {
              if (!hasElapsed(nowMs, staSeenMs, TIME_SYNC_TIMEOUT_MS)) return;
              LOG("sched", "ora non valida dopo %lu s dalla STA: i task che la richiedono restano al ciclo successivo",
                  (unsigned long)(TIME_SYNC_TIMEOUT_MS / 1000UL));
            }
            netPhase = NetPhase::RUNNING;
            continue;
          }

          case NetPhase::RUNNING:
          {
            const uint32_t cycleAtStart = cycle;
            if (!runStarted)
            {
              runStarted = true;
              if (!isInsideRadioHours())
              {
                LOG("sched", "l'ora sincronizzata cade fuori dalla fascia WiFi: nessun fetch");
                releaseRadio(mode);
                finishNetworkStep("fuori fascia");
                return;
              }
              startDueTasks();
            }
            stepRanTask = runDueTasks();

            // Un fetch lungo ha attraversato la scadenza del sensore: il ciclo
            // nuovo riparte da Idle, e questo passo serve comunque il frame in
            // sospeso, come prima del passo a fasi.
            if (cycle != cycleAtStart)
            {
              releaseRadio(mode);
              frameGate = true;
              stepOutcome = "completo, ciclo attraversato";
              return;
            }
            releaseRadio(mode);
            finishNetworkStep(stepRanTask ? "completo" : "nessun task eseguito");
            return;
          }

          case NetPhase::DONE:
            return;
        }
      }
    }

    /** Una riga con i tempi del primo frame dopo il boot: la prova, al boot
     *  successivo, di dove sono finiti i secondi fra STA, ora e task. */
    inline void logFirstFrame()
    {
      if (firstFrameLogged) return;
      firstFrameLogged = true;
      LOG("sched", "primo frame a %lu ms dal boot: STA vista a %ld ms, ora valida a %ld ms (-1 = mai), passo di rete %s",
          (unsigned long)millis(), (long)diagStaMs, (long)diagClockMs, stepOutcome);
    }
  }

  /**
   * Un giro: apre il ciclo se è scaduto, fa avanzare il passo di rete e disegna
   * il frame del ciclo quando il passo è concluso.
   *
   * Nella finestra di manutenzione il loop chiama questa funzione ogni pochi
   * millisecondi per servire il web server, e il giro si valuta al più ogni
   * MAINTENANCE_SCHEDULER_PERIOD_MS, perchè nessun passo ha una granularità
   * più fine del secondo.
   */
  inline void runRound(RadioMode mode)
  {
    using namespace detail;
    ranTaskThisRound = false;

    if (mode == RadioMode::MANAGED_BY_MAINTENANCE)
    {
      const uint32_t nowMs = millis();
      if (maintenanceRoundDone && !hasElapsed(nowMs, lastMaintenanceRoundMs, MAINTENANCE_SCHEDULER_PERIOD_MS))
        return;
      maintenanceRoundDone = true;
      lastMaintenanceRoundMs = nowMs;
    }

    serviceSensorDeadline();
    advanceNetworkStep(mode);

    if (!framePending) return;
    if (!isInsideRadioHours())
    {
      framePending = false;
      return;
    }
    if (!frameGate) return;
    serviceSensorDeadline();
    framePending = false;
    frameGate = false;
    ranTaskThisRound = true;
    logFirstFrame();
    if (hooks.drawFrame) hooks.drawFrame();
  }

  /**
   * Solo il passo del sensore, per quando il giro completo non si può fare:
   * nella finestra di manutenzione, finchè un client usa le pagine, un fetch o
   * un refresh lascerebbero il web server muto, ma un'interrogazione da ~50 ms
   * no, e senza BSEC uscirebbe dalla sua tolleranza.
   */
  inline void serviceSensor() { detail::serviceSensorDeadline(); }

  /** Vero se nell'ultimo giro un task o il frame hanno lavorato: il .ino lo
   *  usa per scrivere la diagnostica di memoria solo nei giri utili. */
  inline bool ranTasksThisRound() { return detail::ranTaskThisRound; }

  /**
   * Millisecondi al prossimo evento: la scadenza esatta del sensore quando è
   * attivo e ne ha una, senza pavimento, perchè un poll in ritardo oltre il
   * 6,25% blocca l'accuratezza di BSEC; altrimenti (nessun sensore, o un
   * sensore senza scadenza come il BME280) la fine del ciclo dentro la fascia
   * e l'apertura della fascia fuori. Il tetto SLEEP_MAX_S è solo una guardia.
   */
  inline uint32_t msUntilNextEvent(const char** who)
  {
    uint32_t ms;
    if (detail::sensorSetsCycle() || !detail::cycleStarted)
    {
      ms = msUntilNextCycle();
      *who = "sensore";
    }
    else if (Clock::valido() && !isInsideRadioHours())
    {
      ms = msUntilLocalHour((uint8_t)Timings::get().wifiOraInizio, false);
      *who = "apertura fascia";
    }
    else
    {
      ms = msUntilNextCycle();
      *who = "ciclo";
    }
    const uint32_t cap = (uint32_t)SLEEP_MAX_S * 1000UL;
    return ms > cap ? cap : ms;
  }

  /**
   * Light sleep fino al prossimo evento. Prima di dormire verifica che la
   * radio sia davvero giù: nel percorso nominale l'ha già spenta il giro, e
   * se questa riga logga significa che l'invariante è stata rotta altrove.
   * Con l'evento già scaduto non dorme: il giro successivo parte subito.
   */
  inline void sleepUntilNextEvent()
  {
    if (WiFi.getMode() != WIFI_OFF)
    {
      LOG("sched", "radio ancora accesa prima del light sleep: la spengo");
      if (detail::hooks.radioOff) detail::hooks.radioOff();
    }
    const char* who = nullptr;
    const uint32_t ms = msUntilNextEvent(&who);
    if (ms == 0) return;
    if (detail::ranTaskThisRound) LOG("sched", "sleep %lu ms (prossimo: %s)", (unsigned long)ms, who);
    else LOGV("sched", "sleep %lu ms (prossimo: %s)", (unsigned long)ms, who);
    Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
    const esp_err_t e = esp_light_sleep_start();
    if (e != ESP_OK) LOG("sched", "light sleep rifiutato: %d", (int)e);
  }

  /**
   * Registra la tabella dei task di rete e gli hook verso radio, sensore e
   * pannello. Da chiamare in setup() dopo Timings::begin() e dopo i begin()
   * dei moduli.
   */
  inline void begin(Task* table, uint8_t count, const Hooks& hooks)
  {
    detail::tasks = table;
    detail::taskCount = count;
    detail::hooks = hooks;
    detail::cycle = 0;
    detail::cycleStarted = false;
    detail::framePending = false;
    detail::lastActiveDay = -1;
    detail::maintenanceRoundDone = false;
    detail::clockAttempted = false;
    LOG("sched", "%u task di rete, ciclo di %u s, sensore %s", (unsigned)count, (unsigned)CYCLE_S,
        detail::sensorIsActive() ? "attivo" : "assente (ciclo interno)");
  }

  /**
   * Dump dello stato, per rispondere alla domanda "perchè questo task non
   * parte". Il motivo è la combinazione di più campi, quindi serve poterla
   * stampare: per ogni task la cadenza in minuti e in cicli, i cicli che
   * mancano allo slot e i contatori degli esiti.
   */
  inline void printStatus(Print& out)
  {
    const char* who = nullptr;
    const uint32_t next = msUntilNextEvent(&who);
    out.printf("ciclo: %lu  prossimo evento: %s fra %lu s  giri radio: %lu\n",
               (unsigned long)detail::cycle, who, (unsigned long)(next / 1000UL),
               (unsigned long)detail::radioRounds);
    out.printf("orologio: %s  fascia: %s  sensore: %s  passo di rete: %s\n",
               Clock::valido() ? "sincronizzato" : "non sincronizzato",
               isInsideRadioHours() ? "aperta" : "chiusa",
               detail::sensorIsActive() ? "attivo" : "assente",
               detail::stepOutcome);
    // Separa le righe generali dalla tabella dei task, larga quanto la tabella.
    out.printf("-----------------------------------------------------------\n");
    out.printf("%-8s %-11s %-10s %7s %7s %7s\n", "task", "cadenza", "prossimo", "esiti", "ok", "falliti");
    for (uint8_t i = 0; i < detail::taskCount; ++i)
    {
      Task& t = detail::tasks[i];
      char cadence[16];
      char when[16];
      snprintf(cadence, sizeof cadence, "%u'=%lu cicli", (unsigned)(Timings::get().*t.intervalMinutes),
               (unsigned long)detail::cyclesPerSlot(t));

      if (t.preconditionsMet && !t.preconditionsMet()) snprintf(when, sizeof when, "sospeso");
      else if (isDue(t)) snprintf(when, sizeof when, "ora");
      else if (t.state.retrying) snprintf(when, sizeof when, "ritento");
      else if (t.state.deferred) snprintf(when, sizeof when, "rinvio");
      else
      {
        const uint32_t elapsed = detail::cycle - t.state.slotCycle;
        const uint32_t per = detail::cyclesPerSlot(t);
        snprintf(when, sizeof when, "%lu cicli", (unsigned long)(elapsed >= per ? 0 : per - elapsed));
      }
      out.printf("%-8s %-11s %-10s %7lu %7lu %7lu\n", t.tag, cadence, when,
                 (unsigned long)t.state.outcomeCount, (unsigned long)t.state.okCount,
                 (unsigned long)t.state.failedCount);
    }
  }
}

#endif // SCHEDULER_H

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
 * cicli. Senza sensore il ciclo è interno e dura lo stesso CYCLE_S. Ogni ciclo
 * esegue tre passi, sempre in quest'ordine:
 *
 *   1. sensore: l'interrogazione, così ciò che segue usa il campione nuovo;
 *   2. rete: una sola accensione della radio per i task dovuti in questo
 *      ciclo, nell'ordine della tabella, con gli onRoundStart() prima dei run();
 *   3. frame: il ridisegno del pannello, che quindi mostra sempre i dati
 *      appena prodotti.
 *
 * Le cadenze dei task sono multipli del ciclo e si contano in cicli dallo slot
 * precedente, mai in millisecondi, quindi non accumulano deriva: un task da 10
 * minuti cade un frame sì e uno no, per sempre. Fuori dalla fascia WiFi resta
 * solo il passo del sensore, il minimo che conserva l'accuratezza di BSEC: niente
 * radio e niente frame fino all'apertura della fascia.
 *
 * La scadenza del sensore ha la precedenza su tutto: lo sleep dura fino a lei
 * esattamente, e la si controlla anche fra un task di rete e l'altro e prima
 * del frame, perchè la fase di rete nel caso peggiore si avvicina al ciclo.
 *
 * Non possiede la radio nè il sensore nè il pannello: li usa attraverso gli
 * hook passati a begin(). Durante la finestra di manutenzione non accende nè
 * spegne la radio, la usa se la STA è connessa.
 */
namespace Scheduler
{
  /**
   * Esito di un task nel giro corrente.
   *
   *   OK            eseguito, con dati nuovi da mostrare.
   *   FAILED        guasto imputabile al task: consuma un tentativo.
   *   SKIPPED       niente da fare, o guasto a monte (radio giù): lo stato
   *                 resta intatto e il tentativo non viene speso.
   */
  enum class Outcome : uint8_t { OK, FAILED, SKIPPED };

  /** Come si apre lo slot di un task. */
  enum class Cadence : uint8_t
  {
    PERIODIC,  // ogni intervalMinutes / CYCLE_MIN cicli dallo slot precedente
    DAILY      // una volta al giorno, dal primo ciclo con l'ora locale indicata
  };

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
   *   preconditionsMet()  precondizione non temporale; falso sospende il task.
   *   hasIncompleteData() vero quando l'esecuzione è riuscita ma i dati non
   *                       bastano ancora: lo slot non si chiude e si ritenta.
   *   onRoundStart()      eseguito per TUTTI i task dovuti prima di qualunque
   *                       run(): serve a chi deve avviare qualcosa e lasciarlo
   *                       maturare mentre gli altri girano.
   */
  struct Task
  {
    const char* tag;
    Cadence     cadence;

    uint16_t Timings::Valori::*intervalMinutes;     // PERIODIC, multiplo di CYCLE_MIN
    uint16_t Timings::Valori::*dailyLocalHour;      // DAILY

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
      uint32_t attemptCycle      = 0;      // ciclo dell'ultimo tentativo: uno per ciclo
      bool     retrying          = false;
      uint8_t  failedAttempts    = 0;      // falliti consecutivi dentro lo slot corrente
      int16_t  lastSlotDayOfYear = -1;     // tm_yday dell'ultimo slot giornaliero
      uint32_t lastDailyMs       = 0;      // istante dell'ultimo slot giornaliero
      uint32_t outcomeCount = 0, okCount = 0, failedCount = 0;
    } state;
  };

  /**
   * Hook verso la piattaforma e i moduli, tutti obbligatori salvo indicazione.
   * Il sensore è opzionale: con sensorActive() falso o assente il ciclo è
   * interno, ogni CYCLE_S dall'inizio del precedente.
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

    inline uint32_t bootMs           = 0;
    inline uint32_t cycle            = 0;      // numero del ciclo corrente, dal primo dopo il boot
    inline bool     cycleStarted     = false;
    inline uint32_t cycleStartMs     = 0;
    inline bool     framePending     = false;  // il ciclo corrente non ha ancora il suo frame
    inline int16_t  lastActiveDay    = -1;     // tm_yday dell'ultimo ciclo dentro la fascia
    inline bool     ranTaskThisRound = false;
    inline uint32_t radioRounds      = 0;

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
   * attivo, altrimenti CYCLE_S dall'inizio del ciclo corrente. 0 prima del
   * primo ciclo dopo il boot.
   */
  inline uint32_t msUntilNextCycle()
  {
    using namespace detail;
    if (!cycleStarted) return 0;
    if (sensorIsActive()) return hooks.msUntilSensorPoll ? hooks.msUntilSensorPoll() : 0;
    return msRemaining(millis(), cycleStartMs + (uint32_t)CYCLE_S * 1000UL);
  }

  /** Numero del ciclo corrente, per i log e per i test. */
  inline uint32_t currentCycle() { return detail::cycle; }

  /**
   * Vero se il task va eseguito nel ciclo corrente: precondizioni vere, al più
   * un tentativo per ciclo, e per un periodico i cicli della cadenza trascorsi
   * dallo slot precedente. Un giornaliero chiede l'orologio, l'ora raggiunta,
   * un giorno diverso dall'ultimo slot e la distanza minima dal precedente,
   * contata in millisecondi perchè senza sensore di notte i cicli si fermano.
   */
  inline bool isDue(const Task& t, const struct tm* today)
  {
    const Task::State& s = t.state;
    if (t.preconditionsMet && !t.preconditionsMet()) return false;
    if (s.attempted && s.attemptCycle == detail::cycle) return false;
    if (s.retrying) return true;
    if (t.cadence == Cadence::PERIODIC)
      return s.neverRun || detail::cycle - s.slotCycle >= detail::cyclesPerSlot(t);

    if (!today) return false;
    if (!s.neverRun && !detail::hasElapsed(millis(), s.lastDailyMs, GIORNALIERA_DISTANZA_MIN_MS))
      return false;
    return today->tm_yday != s.lastSlotDayOfYear &&
           today->tm_hour >= (int)(Timings::get().*t.dailyLocalHour);
  }

  namespace detail
  {
    /** Vero se almeno un task di rete è dovuto nel ciclo corrente. */
    inline bool anyTaskDue(const struct tm* today)
    {
      for (uint8_t i = 0; i < taskCount; ++i)
        if (isDue(tasks[i], today)) return true;
      return false;
    }

    /**
     * Chiude lo slot corrente: da qui il task non ritenta e la cadenza riparte
     * dal ciclo in cui lo slot si era aperto. Il giorno di un task giornaliero
     * si registra solo con l'orologio valido, altrimenti si memorizzerebbe il
     * 1 gennaio 1970 bruciando uno slot futuro.
     */
    inline void closeSlot(Task& t, const struct tm* today)
    {
      t.state.slotCycle = t.state.openCycle;
      t.state.neverRun = false;
      t.state.retrying = false;
      t.state.failedAttempts = 0;
      if (t.cadence == Cadence::DAILY)
      {
        t.state.lastDailyMs = millis();
        if (today) t.state.lastSlotDayOfYear = (int16_t)today->tm_yday;
      }
    }

    /**
     * Apre un nuovo ciclo se la sua scadenza è arrivata: interroga il sensore e,
     * dentro la fascia, prenota il frame. Il primo ciclo di un giorno dentro la
     * fascia rende dovuti tutti i periodici, così il primo frame del mattino ha
     * dati nuovi; i giornalieri seguono solo la propria ora.
     * @return true se il ciclo è stato aperto.
     */
    inline bool startCycleIfDue(const struct tm* today)
    {
      if (msUntilNextCycle() != 0) return false;
      ++cycle;
      cycleStarted = true;
      cycleStartMs = millis();
      if (sensorIsActive() && hooks.pollSensor) hooks.pollSensor();
      if (isInsideRadioHours())
      {
        framePending = true;
        if (today)
        {
          if (lastActiveDay >= 0 && today->tm_yday != lastActiveDay)
          {
            for (uint8_t i = 0; i < taskCount; ++i)
              if (tasks[i].cadence == Cadence::PERIODIC) tasks[i].state.neverRun = true;
            LOG("sched", "ciclo %lu: primo della giornata, tutti i periodici dovuti", (unsigned long)cycle);
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
      if (t.cadence == Cadence::DAILY)
        snprintf(buf, sizeof buf, "domani dalle %02u", (unsigned)(Timings::get().*t.dailyLocalHour));
      else
        snprintf(buf, sizeof buf, "fra %lu cicli", (unsigned long)cyclesPerSlot(t));
      return buf;
    }
  }

  /**
   * Registra l'esito di un task e decide se lo slot si chiude o si ritenta al
   * ciclo successivo. Un'esecuzione riuscita ma con dati ancora incompleti
   * conta come fallimento ai fini dello slot: è il caso del meteo che ha la
   * corrente ma non le previsioni.
   */
  inline void recordOutcome(Task& t, Outcome outcome, uint32_t durationMs, const struct tm* today)
  {
    if (outcome == Outcome::SKIPPED)
    {
      LOGV("sched", "%s: saltato", t.tag);
      return;
    }

    detail::ranTaskThisRound = true;
    if (!t.state.retrying) t.state.openCycle = detail::cycle;
    t.state.attempted = true;
    t.state.attemptCycle = detail::cycle;
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
    detail::closeSlot(t, today);
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
    /**
     * Esegue i task dovuti con la radio già connessa: prima tutti gli
     * onRoundStart(), poi i run() nell'ordine della tabella. Fra un task e
     * l'altro serve la scadenza del sensore, che una fase di rete lunga
     * potrebbe superare.
     */
    inline void runDueTasks(const struct tm* today)
    {
      for (uint8_t i = 0; i < taskCount; ++i)
        if (isDue(tasks[i], today) && tasks[i].onRoundStart) tasks[i].onRoundStart();

      for (uint8_t i = 0; i < taskCount; ++i)
      {
        serviceSensorDeadline();
        Task& t = tasks[i];
        if (!isDue(t, today)) continue;

        // Radio caduta a giro iniziato: guasto a monte, non del task. Lo slot
        // resta intatto e il tentativo si spende al primo giro con la radio su.
        if (WiFi.status() != WL_CONNECTED)
        {
          LOG("sched", "%s: saltato, radio non connessa (slot intatto)", t.tag);
          recordOutcome(t, Outcome::SKIPPED, 0, today);
          continue;
        }

        const uint32_t startMs = millis();
        const Outcome outcome = t.run();
        recordOutcome(t, outcome, millis() - startMs, today);
      }
    }

    /**
     * Passo di rete del giro. Con la radio dello scheduler la accende solo se
     * un task è dovuto, rilegge l'ora dopo l'accensione (la prima sincronizzazione
     * SNTP avviene dentro radioOn()) e rinuncia se l'ora vera cade fuori fascia.
     * Nella finestra di manutenzione usa la STA se c'è, e al boot aspetta l'ora
     * valida per al più PRIMO_FRAME_ATTESA_MS, così un boot notturno non fa
     * fetch prima di sapere che è notte.
     */
    inline void runNetworkStep(RadioMode mode)
    {
      struct tm buffer;
      const struct tm* today = localNow(buffer);
      if (!isInsideRadioHours() || !anyTaskDue(today)) return;

      if (mode == RadioMode::MANAGED_BY_MAINTENANCE)
      {
        if (WiFi.status() != WL_CONNECTED) return;
        if (!Clock::valido() && !hasElapsed(millis(), bootMs, PRIMO_FRAME_ATTESA_MS)) return;
        runDueTasks(localNow(buffer));
        return;
      }

      ++radioRounds;
      if (hooks.radioOn && hooks.radioOn())
      {
        // L'ora si rilegge qui: la prima sincronizzazione SNTP avviene dentro
        // radioOn(), e senza un'ora valida un giornaliero non registrerebbe il
        // giorno del proprio slot.
        today = localNow(buffer);
        if (isInsideRadioHours()) runDueTasks(today);
        else LOG("sched", "l'ora sincronizzata cade fuori dalla fascia WiFi: nessun fetch");
      }
      else
      {
        LOG("sched", "connessione fallita: i task di rete riprovano al prossimo ciclo");
      }
      if (hooks.radioOff) hooks.radioOff();
    }

    /**
     * Vero quando il frame del ciclo può partire. Con la radio dello scheduler
     * subito dopo il passo di rete, riuscito o no. Nella finestra di
     * manutenzione i task possono aspettare la STA o l'ora: il frame li attende
     * per al più PRIMO_FRAME_ATTESA_MS dall'inizio del ciclo, poi si disegna con
     * quello che c'è, placeholder compresi.
     */
    inline bool frameReady(RadioMode mode)
    {
      if (mode == RadioMode::MANAGED_BY_SCHEDULER) return true;
      struct tm buffer;
      if (!anyTaskDue(localNow(buffer))) return true;
      return hasElapsed(millis(), cycleStartMs, PRIMO_FRAME_ATTESA_MS);
    }
  }

  /**
   * Un giro: apre il ciclo se è scaduto, esegue il passo di rete e disegna il
   * frame del ciclo quando è pronto.
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
    runNetworkStep(mode);

    if (!framePending) return;
    if (!isInsideRadioHours())
    {
      framePending = false;
      return;
    }
    if (!frameReady(mode)) return;
    serviceSensorDeadline();
    framePending = false;
    ranTaskThisRound = true;
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
   * attivo, senza pavimento, perchè un poll in ritardo oltre il 6,25% blocca
   * l'accuratezza di BSEC; senza sensore la fine del ciclo dentro la fascia e
   * l'apertura della fascia fuori. Il tetto SLEEP_MAX_S è solo una guardia.
   */
  inline uint32_t msUntilNextEvent(const char** who)
  {
    uint32_t ms;
    if (detail::sensorIsActive() || !detail::cycleStarted)
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
    detail::bootMs = millis();
    detail::cycle = 0;
    detail::cycleStarted = false;
    detail::framePending = false;
    detail::lastActiveDay = -1;
    detail::maintenanceRoundDone = false;
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
    struct tm localBuffer;
    const struct tm* today = detail::localNow(localBuffer);
    const char* who = nullptr;
    const uint32_t next = msUntilNextEvent(&who);
    out.printf("ciclo: %lu  prossimo evento: %s fra %lu s  giri radio: %lu\n",
               (unsigned long)detail::cycle, who, (unsigned long)(next / 1000UL),
               (unsigned long)detail::radioRounds);
    out.printf("orologio: %s  fascia: %s  sensore: %s\n",
               Clock::valido() ? "sincronizzato" : "non sincronizzato",
               isInsideRadioHours() ? "aperta" : "chiusa",
               detail::sensorIsActive() ? "attivo" : "assente");
    // Separa le righe generali dalla tabella dei task, larga quanto la tabella.
    out.printf("-----------------------------------------------------------\n");
    out.printf("%-8s %-11s %-10s %7s %7s %7s\n", "task", "cadenza", "prossimo", "esiti", "ok", "falliti");
    for (uint8_t i = 0; i < detail::taskCount; ++i)
    {
      const Task& t = detail::tasks[i];
      char cadence[16];
      char when[16];
      if (t.cadence == Cadence::DAILY)
        snprintf(cadence, sizeof cadence, "ore %02u", (unsigned)(Timings::get().*t.dailyLocalHour));
      else
        snprintf(cadence, sizeof cadence, "%u'=%lu cicli", (unsigned)(Timings::get().*t.intervalMinutes),
                 (unsigned long)detail::cyclesPerSlot(t));

      if (t.preconditionsMet && !t.preconditionsMet()) snprintf(when, sizeof when, "sospeso");
      else if (isDue(t, today)) snprintf(when, sizeof when, "ora");
      else if (t.cadence == Cadence::DAILY) snprintf(when, sizeof when, "giornaliero");
      else if (t.state.retrying) snprintf(when, sizeof when, "ritento");
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

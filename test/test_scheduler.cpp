/**
 * Verifica della logica temporale di Scheduler.h: cadenze, coalescing,
 * ritenti, cadenza giornaliera con recupero, fascia oraria, ora legale,
 * rollover di millis() e clamp del light sleep.
 *
 * Include l'header VERO del firmware (-I..), non una copia. Tre funzioni della
 * piattaforma sono sostituite da macro: time(), perche' Scheduler.h chiama
 * time(nullptr), piu' localtime_r() e mktime(), che passano dalle regole POSIX
 * di stub/fuso_posix.h invece che dalla libc dell'host.
 *
 * Il fuso non e' quindi quello di sistema, ed e' voluto: sull'ESP32 non esiste
 * nessun database IANA e newlib interpreta esattamente la stringa CAL_POSIX_TZ,
 * cioe' fa lo stesso lavoro dello stub. In piu' il CRT Windows non legge i
 * campi di transizione di quella stringa, quindi con la libc dell'host il test
 * non sarebbe attendibile fuori da macOS e Linux.
 *
 * Perche' la cosa non diventi circolare, lo stub e' ancorato in due modi: una
 * batteria di epoch di riferimento scritti come letterali, ricavati dal
 * calendario (ultima domenica di marzo e di ottobre, transizione alle 01:00
 * UTC per direttiva UE) e verificati contro il database dei fusi di Windows; e
 * su host POSIX il confronto diretto con la libc lungo tutto l'anno.
 */
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fuso_posix.h"

/** Stringa POSIX del fuso, la stessa di CAL_POSIX_TZ in Calendar.h. Duplicata
 *  e non inclusa perche' Calendar.h tira dentro il display e tutto il resto. */
#define TZ_TEST "CET-1CEST,M3.5.0,M10.5.0/3"

/**
 * Timings.h va incluso PRIMA delle macro: porta con se' gli stub di
 * Preferences e Arduino, e quindi <map> e <string>. Con le macro gia' attive
 * si rischia di riscrivere una dichiarazione di mktime del CRT, che su MSVC
 * arriva decorata.
 */
#include "Timings.h"

#ifndef _WIN32
/**
 * Accesso alle funzioni vere, definito PRIMA delle macro che le nascondono:
 * serve al confronto fra stub e libc, che gira solo dove la libc sa leggere le
 * regole POSIX. Due wrapper e non due puntatori a funzione, perche' glibc
 * dichiara localtime_r con __restrict sui parametri e il tipo del puntatore
 * non combacerebbe.
 */
static struct tm* libcLocaltime(const time_t* t, struct tm* out) { return localtime_r(t, out); }
static time_t libcMktime(struct tm* t) { return mktime(t); }
#endif

static time_t g_epochFinta = 0;
static inline time_t tempoFinto(time_t* p) { if (p) *p = g_epochFinta; return g_epochFinta; }
#define time(p) tempoFinto(p)
#define localtime_r(a, b) fusoLocaltime(a, b)
#define mktime(t) fusoMktime(t)
#include "Scheduler.h"

uint32_t g_msFinto = 0;
SerialeFinta Serial;
WiFiFinto WiFi;

static int falliti = 0;
static void ok(bool c, const char* nome)
{
  std::printf("%-64s %s\n", nome, c ? "ok" : "FALLITO");
  if (!c) ++falliti;
}

/** Porta l'orologio finto a una data e ora locale. */
static void impostaOra(int anno, int mese, int giorno, int h, int m)
{
  struct tm t {};
  t.tm_year = anno - 1900; t.tm_mon = mese - 1; t.tm_mday = giorno;
  t.tm_hour = h; t.tm_min = m; t.tm_sec = 0; t.tm_isdst = -1;
  g_epochFinta = mktime(&t);
}
static const struct tm* oggiLocale()
{
  static struct tm t;
  localtime_r(&g_epochFinta, &t);
  return &t;
}

/**
 * Ancora lo stub del fuso a valori indipendenti dal suo stesso codice.
 *
 * Gli epoch sono ricavati dal calendario: la direttiva UE fissa le due
 * transizioni alle 01:00 UTC dell'ultima domenica di marzo e di ottobre, che
 * nel 2026 sono il 29/03 e il 25/10. Le ore locali attese sono state
 * verificate contro il database dei fusi di Windows ("W. Europe Standard
 * Time"), cioe' una terza implementazione che non e' ne' la nostra ne' la libc
 * dell'host.
 */
static void verificaFuso()
{
  struct Atteso
  {
    time_t epoch;
    int anno, mese, giorno, ora, minuto, secondo, dst;
    const char* nome;
  };
  static const Atteso ATTESI[] = {
      {1774745999, 2026,  3, 29,  1, 59, 59, 0, "un secondo prima del cambio di marzo"},
      {1774746000, 2026,  3, 29,  3,  0,  0, 1, "  e subito dopo: le 02 non esistono"},
      {1792889999, 2026, 10, 25,  2, 59, 59, 1, "un secondo prima del cambio di ottobre"},
      {1792890000, 2026, 10, 25,  2,  0,  0, 0, "  e subito dopo: le 02 tornano indietro"},
      {1768474800, 2026,  1, 15, 12,  0,  0, 0, "mezzogiorno di gennaio in ora solare"},
      {1784109600, 2026,  7, 15, 12,  0,  0, 1, "mezzogiorno di luglio in ora legale"},
  };

  for (const Atteso& a : ATTESI)
  {
    struct tm t;
    localtime_r(&a.epoch, &t);
    const bool esatto = t.tm_year + 1900 == a.anno && t.tm_mon + 1 == a.mese &&
                        t.tm_mday == a.giorno && t.tm_hour == a.ora &&
                        t.tm_min == a.minuto && t.tm_sec == a.secondo &&
                        (t.tm_isdst > 0 ? 1 : 0) == a.dst;
    ok(esatto, a.nome);
  }

  // Andata e ritorno su un istante non ambiguo, con tm_isdst = -1 come lo usa
  // Scheduler::msUntilLocalHour().
  {
    struct tm t{};
    t.tm_year = 2026 - 1900; t.tm_mon = 6; t.tm_mday = 15;
    t.tm_hour = 12; t.tm_isdst = -1;
    ok(mktime(&t) == 1784109600, "mktime con isdst=-1 sceglie l'ora legale in luglio");
  }

  // Le due ore patologiche, che il firmware puo' raggiungere con cine_h o
  // wifi_h_ini a 2: qui si fissa la convenzione, non si spera che non accada.
  {
    struct tm t{};
    t.tm_year = 2026 - 1900; t.tm_mon = 2; t.tm_mday = 29;
    t.tm_hour = 2; t.tm_min = 30; t.tm_isdst = -1;
    const time_t e = mktime(&t);
    ok(e == 1774747800 && t.tm_hour == 3 && t.tm_min == 30,
       "ora inesistente di marzo: 02:30 diventa 03:30 legali");
  }
  {
    struct tm t{};
    t.tm_year = 2026 - 1900; t.tm_mon = 9; t.tm_mday = 25;
    t.tm_hour = 2; t.tm_min = 30; t.tm_isdst = -1;
    const time_t e = mktime(&t);
    ok(e == 1792891800 && (t.tm_isdst > 0 ? 1 : 0) == 0,
       "ora ripetuta di ottobre: vince la seconda occorrenza, in ora solare");
  }

#ifndef _WIN32
  /**
   * Dove la libc sa leggere le regole POSIX, cioe' ovunque tranne Windows, il
   * confronto diretto: se stub e sistema divergono anche solo su un istante
   * dell'anno il test lo dice, invece di lasciarlo passare.
   */
  setenv("TZ", TZ_TEST, 1);
  tzset();
  bool concordi = true;
  time_t divergente = 0;
  for (time_t t = 1767225600; t < 1798761600 && concordi; t += 20 * 60)
  {
    struct tm mio, suo;
    fusoLocaltime(&t, &mio);
    libcLocaltime(&t, &suo);
    if (mio.tm_year != suo.tm_year || mio.tm_mon != suo.tm_mon ||
        mio.tm_mday != suo.tm_mday || mio.tm_hour != suo.tm_hour ||
        mio.tm_min != suo.tm_min || mio.tm_sec != suo.tm_sec ||
        (mio.tm_isdst > 0) != (suo.tm_isdst > 0))
    {
      concordi = false;
      divergente = t;
    }
    else
    {
      /**
       * Ritorno all'epoch con l'ora legale NOTA, non con -1: sull'ora
       * ripetuta di ottobre la risoluzione di mktime con isdst = -1 non e'
       * specificata e le due implementazioni possono divergere in modo
       * legittimo, il che darebbe un falso allarme. La convenzione dello stub
       * per quel caso e' fissata a parte, dalle asserzioni sulle due ore
       * patologiche.
       */
      struct tm a = mio, b = suo;   // ognuno riparte dal proprio tm
      if (fusoMktime(&a) != t || libcMktime(&b) != t) { concordi = false; divergente = t; }
    }
  }
  if (!concordi) std::printf("   diverge su epoch %lld\n", (long long)divergente);
  ok(concordi, "stub e libc concordano su ogni istante del 2026 (host POSIX)");
#endif
}

/** Stessa precondizione che nel firmware hanno i due task calendario. */
static bool clockReady() { return Clock::valido(); }

static Scheduler::Outcome runOk() { return Scheduler::Outcome::OK; }
static Scheduler::Outcome runFailed() { return Scheduler::Outcome::FAILED; }

int main()
{
  ok(fusoImposta(TZ_TEST), "stringa POSIX del fuso interpretata");
  verificaFuso();
  Timings::begin();
  const uint32_t MIN = 60u * 1000u;

  // ---- cadenza periodica e coalescing ------------------------------------
  Scheduler::Task p {};
  p.tag = "p"; p.cadence = Scheduler::Cadence::PERIODIC; p.needsRadio = false;
  p.intervalMinutes = &Timings::Valori::owmMin;            // 10 min
  p.retryIntervalMs = FETCH_RITENTO_MS; p.maxAttempts = FETCH_MAX_TENTATIVI;
  p.run = runOk;

  impostaOra(2026, 9, 10, 12, 0);
  g_msFinto = 1000;
  ok(Scheduler::msUntilDue(p, g_msFinto, oggiLocale()) == 0,
     "periodica mai eseguita: dovuta subito");
  Scheduler::recordOutcome(p, Scheduler::Outcome::OK, g_msFinto, 5, oggiLocale());
  ok(!p.state.neverRun && p.state.lastSlotMs == 1000, "  slot consumato dopo l'esito");

  g_msFinto = 1000 + 9 * MIN;
  ok(!Scheduler::isDue(p, g_msFinto, oggiLocale(), 0), "a 9 min: non dovuta (scadenza stretta)");
  ok(Scheduler::isDue(p, g_msFinto, oggiLocale(), 2 * MIN), "a 9 min: dovuta con coalescing 2 min");
  g_msFinto = 1000 + 10 * MIN;
  ok(Scheduler::isDue(p, g_msFinto, oggiLocale(), 0), "a 10 min: dovuta");

  // ---- ritento e soglia --------------------------------------------------
  Scheduler::Task r = p; r.tag = "r"; r.run = runFailed; r.state = {};
  g_msFinto = 100000;
  Scheduler::recordOutcome(r, Scheduler::Outcome::FAILED, g_msFinto, 5, oggiLocale());
  ok(r.state.retrying && r.state.failedAttempts == 1, "1o fallimento: in ritento");
  ok(Scheduler::msUntilDue(r, g_msFinto, oggiLocale()) == FETCH_RITENTO_MS,
     "  scadenza = intervallo di ritento");
  ok(!Scheduler::isDue(r, g_msFinto + FETCH_RITENTO_MS - 1, oggiLocale(), 0),
     "  non dovuta prima del ritento");
  ok(Scheduler::isDue(r, g_msFinto + FETCH_RITENTO_MS, oggiLocale(), 0),
     "  dovuta allo scadere del ritento");
  g_msFinto += FETCH_RITENTO_MS;
  Scheduler::recordOutcome(r, Scheduler::Outcome::FAILED, g_msFinto, 5, oggiLocale());
  ok(!r.state.retrying && r.state.failedAttempts == 0,
     "2o fallimento: soglia raggiunta, slot consumato");
  ok(Scheduler::msUntilDue(r, g_msFinto, oggiLocale()) == 10 * MIN,
     "  si attende la cadenza piena");

  // ---- esito saltato: guasto a monte, non del task -----------------------
  Scheduler::Task s = p; s.tag = "s"; s.state = {};
  g_msFinto = 200000;
  Scheduler::recordOutcome(s, Scheduler::Outcome::SKIPPED, g_msFinto, 0, oggiLocale());
  ok(s.state.neverRun && s.state.failedAttempts == 0 && s.state.outcomeCount == 0,
     "saltato: slot intatto, nessun tentativo speso");

  // ---- cadenza giornaliera -----------------------------------------------
  Scheduler::Task g {};
  g.tag = "g"; g.cadence = Scheduler::Cadence::DAILY; g.needsRadio = false;
  g.dailyLocalHour = &Timings::Valori::cinemaOra;             // ora 7
  g.retryIntervalMs = FETCH_RITENTO_MS; g.maxAttempts = FETCH_MAX_TENTATIVI;
  g.run = runOk;
  g.state.neverRun = false; g.state.lastSlotDayOfYear = -1; g.state.lastSlotMs = 0;

  impostaOra(2026, 9, 10, 6, 0);
  g_msFinto = 50u * 3600u * 1000u;                        // ultimoMs lontano
  {
    uint32_t d = Scheduler::msUntilDue(g, g_msFinto, oggiLocale());
    ok(d > 55 * MIN && d < 65 * MIN, "giornaliera alle 06:00 con ora 7: manca circa un'ora");
  }
  impostaOra(2026, 9, 10, 7, 30);
  ok(Scheduler::msUntilDue(g, g_msFinto, oggiLocale()) == 0,
     "alle 07:30, mai fatta oggi: dovuta (recupero con >=)");
  impostaOra(2026, 9, 10, 11, 0);
  ok(Scheduler::msUntilDue(g, g_msFinto, oggiLocale()) == 0,
     "alle 11:00, mai fatta oggi: ancora recuperabile");
  Scheduler::recordOutcome(g, Scheduler::Outcome::OK, g_msFinto, 5, oggiLocale());
  ok(g.state.lastSlotDayOfYear == oggiLocale()->tm_yday, "  giorno registrato");
  {
    uint32_t d = Scheduler::msUntilDue(g, g_msFinto, oggiLocale());
    ok(d > 19u * 3600u * 1000u && d < 21u * 3600u * 1000u, "  poi mancano ~20 h a domani alle 07");
  }
  impostaOra(2026, 9, 11, 9, 0);
  g_msFinto += 22u * 3600u * 1000u;
  ok(Scheduler::msUntilDue(g, g_msFinto, oggiLocale()) == 0,
     "giorno dopo alle 09:00: dovuta (recupero)");

  // ---- fascia oraria -----------------------------------------------------
  Scheduler::Task n = p; n.tag = "n"; n.needsRadio = true; n.state = {};
  impostaOra(2026, 9, 10, 3, 0);
  ok(!Scheduler::isInsideRadioHours(), "alle 03:00 fuori dalla fascia 7..23");
  {
    uint32_t e = Scheduler::msUntilDueWithRadioLimits(n, g_msFinto, oggiLocale());
    ok(e > 3u * 3600u * 1000u && e < 5u * 3600u * 1000u,
       "  task di rete rimandato all'apertura della fascia");
  }
  impostaOra(2026, 9, 10, 12, 0);
  ok(Scheduler::isInsideRadioHours() && Scheduler::msUntilDueWithRadioLimits(n, g_msFinto, oggiLocale()) == 0,
     "a mezzogiorno: dentro la fascia, dovuta");

  // ---- orologio non sincronizzato ----------------------------------------
  g_epochFinta = 1000;
  ok(!Clock::valido(), "epoch bassa: orologio non sincronizzato");
  ok(Scheduler::isInsideRadioHours(), "  fascia fail-open, altrimenti il primo SNTP non avverrebbe mai");
  ok(Scheduler::msUntilDue(g, g_msFinto, nullptr) == Scheduler::NEVER,
     "  cadenza giornaliera sospesa");

  // Precondizione non temporale: e' cosi' che i due calendari evitano di
  // interrogare da 1970 e di consumare lo slot con una risposta sbagliata.
  {
    Scheduler::Task c {};
    c.tag = "c"; c.cadence = Scheduler::Cadence::PERIODIC; c.needsRadio = true;
    c.intervalMinutes = &Timings::Valori::googleMin;
    c.retryIntervalMs = FETCH_RITENTO_MS; c.maxAttempts = FETCH_MAX_TENTATIVI;
    c.run = runOk; c.preconditionsMet = clockReady;

    ok(Scheduler::msUntilDueWithRadioLimits(c, g_msFinto, nullptr) == Scheduler::NEVER,
       "preconditionsMet() falso: task sospeso anche se mai eseguito");
    ok(!Scheduler::isDue(c, g_msFinto, nullptr, 10u * 60u * 1000u),
       "  e non dovuto nemmeno con un anticipo generoso");

    impostaOra(2026, 9, 10, 12, 0);
    ok(Clock::valido(), "orologio sincronizzato: la precondizione si avvera");
    ok(Scheduler::msUntilDue(c, g_msFinto, oggiLocale()) == 0,
       "  e il task e' dovuto subito, senza aver perso lo slot");
  }

  // Scadenza custom: precede ogni altra regola, neverRun compreso. È la
  // regola per cui il sensore restava fermo quando la sua scadenza custom non
  // valeva 0 al boot: un task locale gira solo con scadenza 0.
  {
    static uint32_t scadenzaFinta = 0;
    Scheduler::Task b {};
    b.tag = "b"; b.cadence = Scheduler::Cadence::PERIODIC; b.needsRadio = false;
    b.intervalMinutes = &Timings::Valori::displayRefreshMin;
    b.retryIntervalMs = FETCH_RITENTO_MS; b.maxAttempts = 0;
    b.run = runOk;
    b.customMsUntilDue = []() { return scadenzaFinta; };

    scadenzaFinta = 300000u;
    ok(b.state.neverRun && !Scheduler::isDue(b, g_msFinto, oggiLocale(), 0),
       "scadenza custom a 300 s: non dovuto anche se mai eseguito");
    scadenzaFinta = 0;
    ok(Scheduler::isDue(b, g_msFinto, oggiLocale(), 0),
       "  a 0 dovuto subito, che e' cio' che il sensore deve dare al boot");
  }

  // ---- cambio dell'ora legale --------------------------------------------
  {
    impostaOra(2026, 10, 24, 23, 30);          // sabato prima del cambio del 25 ottobre
    struct tm b;
    localtime_r(&g_epochFinta, &b);
    b.tm_mday += 1; b.tm_hour = 7; b.tm_min = 0; b.tm_sec = 0; b.tm_isdst = -1;
    time_t atteso = mktime(&b);
    uint32_t d = Scheduler::msUntilLocalHour(7, false);
    ok((uint32_t)((atteso - g_epochFinta) * 1000) == d,
       "ora legale: msUntilLocalHour coincide con mktime sullo stesso fuso");
    ok(d > 8u * 3600u * 1000u, "  attraversando il cambio la distanza cresce di un'ora");
  }

  // ---- rollover di millis() ----------------------------------------------
  {
    Scheduler::Task w = p; w.tag = "w"; w.state = {};
    g_msFinto = 0xFFFFFF00u;                   // a un passo dal giro dei 49,7 giorni
    impostaOra(2026, 9, 10, 12, 0);
    Scheduler::recordOutcome(w, Scheduler::Outcome::OK, g_msFinto, 5, oggiLocale());
    g_msFinto = 0xFFFFFF00u + 9 * MIN;         // qui millis() ha gia' wrappato
    ok(!Scheduler::isDue(w, g_msFinto, oggiLocale(), 0), "rollover millis: a 9 min non dovuta");
    g_msFinto = 0xFFFFFF00u + 10 * MIN;
    ok(Scheduler::isDue(w, g_msFinto, oggiLocale(), 0), "rollover millis: a 10 min dovuta");
  }

  // ---- clamp del light sleep ---------------------------------------------
  {
    static Scheduler::Task tab[1];
    tab[0] = p; tab[0].tag = "solo"; tab[0].state = {};
    Scheduler::begin(tab, 1, nullptr, nullptr);
    g_msFinto = 500000;
    impostaOra(2026, 9, 10, 12, 0);
    const char* chi = nullptr;
    Scheduler::recordOutcome(tab[0], Scheduler::Outcome::OK, g_msFinto, 5, oggiLocale());
    ok(Scheduler::msUntilNextEvent(&chi) == (uint32_t)SLEEP_MAX_S * 1000u,
       "scadenza a 10 min: sleep limitato al tetto di 300 s");
    g_msFinto += 10 * MIN - 5000u;             // mancano 5 s
    ok(Scheduler::msUntilNextEvent(&chi) == (uint32_t)SLEEP_MIN_S * 1000u,
       "scadenza fra 5 s: sleep alzato al pavimento di 30 s");
    ok(std::strcmp(chi, "solo") == 0, "  e si sa quale task ha deciso il risveglio");
    tab[0].preconditionsMet = []() { return false; };
    ok(Scheduler::msUntilNextEvent(&chi) == (uint32_t)SLEEP_MAX_S * 1000u &&
           std::strcmp(chi, "nessuno") == 0,
       "nessun task pronto: sleep al tetto, log esplicito");
  }

  // ---- giro nella finestra di manutenzione: al più uno al secondo --------
  // Il loop della finestra chiama runRound() ogni pochi ms per il web server:
  // un task sempre dovuto deve girare una volta per periodo, non a ogni chiamata.
  {
    static int runCount = 0;
    static Scheduler::Task tab[1];
    tab[0] = Scheduler::Task{};
    tab[0].tag = "maint"; tab[0].cadence = Scheduler::Cadence::PERIODIC; tab[0].needsRadio = false;
    tab[0].intervalMinutes = &Timings::Valori::displayRefreshMin;
    tab[0].retryIntervalMs = FETCH_RITENTO_MS; tab[0].maxAttempts = 0;
    tab[0].run = []() { ++runCount; return Scheduler::Outcome::OK; };
    tab[0].customMsUntilDue = []() { return (uint32_t)0; };
    Scheduler::begin(tab, 1, nullptr, nullptr);
    impostaOra(2026, 9, 10, 12, 0);
    g_msFinto = 2000000;
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE);
    ok(runCount == 1, "manutenzione: il primo giro parte subito");
    g_msFinto += MAINTENANCE_SCHEDULER_PERIOD_MS / 2;
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE);
    ok(runCount == 1, "  a mezzo periodo il giro non si rivaluta");
    g_msFinto += MAINTENANCE_SCHEDULER_PERIOD_MS;
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE);
    ok(runCount == 2, "  dopo un periodo il task riparte");
  }

  // ---- orologio sincronizzato dentro l'accensione della radio ------------
  // La prima sincronizzazione SNTP avviene in wifiOn(): il task giornaliero che
  // gira in quel giro deve registrare il giorno, quindi l'ora va riletta dopo.
  {
    static Scheduler::Task tab[1];
    tab[0] = Scheduler::Task{};
    tab[0].tag = "daily"; tab[0].cadence = Scheduler::Cadence::DAILY; tab[0].needsRadio = true;
    tab[0].dailyLocalHour = &Timings::Valori::cinemaOra;
    tab[0].retryIntervalMs = FETCH_RITENTO_MS; tab[0].maxAttempts = FETCH_MAX_TENTATIVI;
    tab[0].run = runOk;
    Scheduler::begin(tab, 1,
                     []() { impostaOra(2026, 9, 10, 12, 0); return true; },
                     []() {});
    g_epochFinta = 1000;
    g_msFinto = 3000000;
    ok(!Clock::valido(), "giro con orologio da sincronizzare");
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
    ok(!tab[0].state.neverRun, "  il task giornaliero gira dopo l'accensione della radio");
    ok(tab[0].state.lastSlotDayOfYear == oggiLocale()->tm_yday,
       "  e registra il giorno con l'ora appena sincronizzata");
  }

  std::printf("\n%s\n", falliti ? "CI SONO FALLIMENTI" : "tutti i controlli superati");
  return falliti != 0;
}

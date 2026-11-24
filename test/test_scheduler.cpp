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
#include <initializer_list>

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

// ---------------------------------------------------------------------------
// Piattaforma finta: sensore, radio e pannello registrano la sequenza dei passi
// in g_eventi, così i test verificano l'ordine oltre ai conteggi.
// ---------------------------------------------------------------------------

static char     g_eventi[512];
static size_t   g_nEventi = 0;
static void evento(char c) { if (g_nEventi + 1 < sizeof g_eventi) { g_eventi[g_nEventi++] = c; g_eventi[g_nEventi] = 0; } }
static void azzeraEventi() { g_nEventi = 0; g_eventi[0] = 0; }

/** Sensore finto con la stessa regola di Indoor: scadenza a ultimo poll +
 *  periodo + margine, 0 prima del primo poll, differenza signed per il rollover. */
static bool     g_sensoreAttivo = true;
static bool     g_maiInterrogato = true;
static uint32_t g_ultimoPollMs = 0;
static uint32_t g_pollPrecedenteMs = 0;
static int      g_poll = 0;
static uint32_t msAlPoll()
{
  if (g_maiInterrogato) return 0;
  const int32_t d = (int32_t)(g_ultimoPollMs + (uint32_t)BSEC_PERIODO_ULP_S * 1000u + BSEC_MARGINE_POLL_MS - g_msFinto);
  return d <= 0 ? 0 : (uint32_t)d;
}
static void poll()
{
  g_pollPrecedenteMs = g_ultimoPollMs;
  g_ultimoPollMs = g_msFinto;
  g_maiInterrogato = false;
  ++g_poll;
  evento('S');
}

static bool g_radioOk = true;
static int  g_frame = 0;

/** Durata finta di un run(), che fa avanzare l'orologio come un fetch vero. */
static uint32_t g_durataRunMs = 0;
static bool     g_fallisciA = false;
static int      g_runA = 0, g_runB = 0;

/** Fallimenti a comando: A fallisce le prime g_fallisciPrimi volte oltre che con g_fallisciA;
 *  B ha una durata sua (-1 = quella di A) e un proprio fallimento; A può far cadere la radio. */
static int      g_fallisciPrimi = 0;
static bool     g_fallisciB = false;
static int64_t  g_durataBMs = -1;
static bool     g_radioCadeAlFallimento = false;

static Scheduler::Outcome runA()
{
  evento('a'); ++g_runA; g_msFinto += g_durataRunMs;
  const bool fallisce = g_fallisciA || g_fallisciPrimi > 0;
  if (g_fallisciPrimi > 0) --g_fallisciPrimi;
  if (fallisce && g_radioCadeAlFallimento) WiFi.stato = 0;
  return fallisce ? Scheduler::Outcome::FAILED : Scheduler::Outcome::OK;
}
static Scheduler::Outcome runB()
{
  evento('b'); ++g_runB;
  g_msFinto += g_durataBMs >= 0 ? (uint32_t)g_durataBMs : g_durataRunMs;
  return g_fallisciB ? Scheduler::Outcome::FAILED : Scheduler::Outcome::OK;
}

/** Task a esiti programmati, come il download di cinema e percorso che si
 *  rinvia mentre il server render.com si avvia: restituisce gli esiti di
 *  sequenza() nell'ordine, poi OK. */
static Scheduler::Outcome g_esiti[8];
static int g_nEsiti = 0, g_iEsiti = 0, g_runD = 0;
static Scheduler::Outcome runD()
{
  evento('d'); ++g_runD;
  return g_iEsiti < g_nEsiti ? g_esiti[g_iEsiti++] : Scheduler::Outcome::OK;
}
static void sequenza(std::initializer_list<Scheduler::Outcome> esiti)
{
  g_nEsiti = g_iEsiti = g_runD = 0;
  for (Scheduler::Outcome e : esiti) g_esiti[g_nEsiti++] = e;
}

static Scheduler::Hooks hooksFinti()
{
  Scheduler::Hooks h;
  h.radioOn = []() { evento('R'); return g_radioOk; };
  h.radioOff = []() { evento('r'); };
  h.sensorActive = []() { return g_sensoreAttivo; };
  h.msUntilSensorPoll = msAlPoll;
  h.pollSensor = poll;
  h.drawFrame = []() { evento('F'); ++g_frame; g_msFinto += (uint32_t)DISPLAY_REFRESH_PIENO_S * 1000u; };
  return h;
}

/** Riporta la piattaforma finta allo stato del boot. */
static void boot(Scheduler::Task* tab, uint8_t n)
{
  g_sensoreAttivo = true; g_maiInterrogato = true; g_poll = 0;
  g_radioOk = true; g_frame = 0; g_durataRunMs = 0; g_fallisciA = false;
  g_fallisciPrimi = 0; g_fallisciB = false; g_durataBMs = -1; g_radioCadeAlFallimento = false;
  g_runA = g_runB = 0;
  WiFi.stato = WL_CONNECTED;
  for (uint8_t i = 0; i < n; ++i) tab[i].state = {};
  azzeraEventi();
  Scheduler::begin(tab, n, hooksFinti());
}

/** Porta l'orologio monotono alla scadenza esatta del sensore. */
static void alProssimoPoll() { g_msFinto = g_ultimoPollMs + (uint32_t)BSEC_PERIODO_ULP_S * 1000u + BSEC_MARGINE_POLL_MS; }

/** Fa avanzare insieme orologio monotono e ora locale, come fa il light sleep. */
static void avanzaMs(uint32_t ms) { g_msFinto += ms; g_epochFinta += (time_t)(ms / 1000u); }

static Scheduler::Task taskPeriodico(const char* tag, uint16_t Timings::Valori::*cadenza,
                                     Scheduler::Outcome (*run)(), uint8_t tentativi = FETCH_MAX_TENTATIVI)
{
  Scheduler::Task t {};
  t.tag = tag;
  t.intervalMinutes = cadenza; t.maxAttempts = tentativi; t.run = run;
  return t;
}

struct PrintFinto : Print { size_t write(uint8_t) override { return 1; } };

int main()
{
  ok(fusoImposta(TZ_TEST), "stringa POSIX del fuso interpretata");
  verificaFuso();
  Timings::begin();
  const uint32_t CICLO_MS = (uint32_t)CYCLE_S * 1000u;

  // ---- orologio del ciclo, ordine dei passi e cadenze in cicli -------------
  {
    static Scheduler::Task tab[2];
    tab[0] = taskPeriodico("a", &Timings::Valori::owmMin, runA);     // 10 min = 2 cicli
    tab[1] = taskPeriodico("b", &Timings::Valori::routeMin, runB);   // 5 min = 1 ciclo
    impostaOra(2026, 9, 10, 12, 0);
    g_msFinto = 1000;
    boot(tab, 2);

    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
    ok(std::strcmp(g_eventi, "SRabrF") == 0, "primo ciclo: sensore, radio, task in ordine, radio giù, frame");
    std::printf("   eventi: %s\n", g_eventi);
    ok(Scheduler::currentCycle() == 1, "  è il ciclo 1");

    const char* chi = nullptr;
    const uint32_t attesa = Scheduler::msUntilNextEvent(&chi);
    ok(attesa == g_ultimoPollMs + 301000u - g_msFinto && std::strcmp(chi, "sensore") == 0,
       "  lo sleep dura fino alla scadenza esatta del sensore");

    azzeraEventi();
    alProssimoPoll();
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
    ok(std::strcmp(g_eventi, "SRbrF") == 0, "secondo ciclo: il task da 10 min salta, frame comunque");

    // Venti cicli con fetch lenti e di durata variabile: il task da 10 minuti
    // resta un ciclo sì e uno no, senza scivolare.
    bool alternanza = true;
    for (int i = 3; i <= 22; ++i)
    {
      g_durataRunMs = (uint32_t)(i % 5) * 7000u;
      const int primaA = g_runA;
      alProssimoPoll();
      Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
      const bool atteso = (i % 2) == 1;
      if ((g_runA > primaA) != atteso) alternanza = false;
    }
    ok(alternanza, "  meteo un ciclo sì e uno no per 20 cicli con fetch di durata variabile");
    ok(g_runB == 22 && g_frame == 22, "  percorso e frame a ogni ciclo");
    ok(g_poll == 22, "  un poll del sensore per ciclo");

    // Sleep senza pavimento: a 5 s dalla scadenza si dorme 5 s, non 30.
    g_msFinto = g_ultimoPollMs + 296000u;
    ok(Scheduler::msUntilNextEvent(&chi) == 5000u, "a 5 s dalla scadenza del sensore: sleep di 5 s, nessun pavimento");
    g_msFinto = g_ultimoPollMs + 302000u;
    ok(Scheduler::msUntilNextEvent(&chi) == 0u, "  scadenza passata: nessuno sleep, il giro riparte subito");

    PrintFinto stampa;
    Scheduler::printStatus(stampa);
    ok(true, "printStatus() senza errori");
  }

  // ---- fase di rete lunga quanto il ciclo: il sensore non aspetta ---------
  {
    static Scheduler::Task tab[2];
    tab[0] = taskPeriodico("a", &Timings::Valori::routeMin, runA);
    tab[1] = taskPeriodico("b", &Timings::Valori::routeMin, runB);
    impostaOra(2026, 9, 10, 12, 0);
    g_msFinto = 5000000;
    boot(tab, 2);
    g_durataRunMs = 302000u;                   // due fetch, ciascuno più lungo del ciclo
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
    std::printf("   eventi: %s\n", g_eventi);
    ok(std::strcmp(g_eventi, "SRaSbrSF") == 0,
       "fetch da 302 s: sensore interrogato fra un task e l'altro e prima del frame");
    ok(g_poll == 3 && g_ultimoPollMs - g_pollPrecedenteMs <= 301000u + 18750u,
       "  ogni poll entro il 6,25% del periodo ULP");
  }

  // ---- ritento al ciclo successivo, slot ancorato ---------------------------
  {
    static Scheduler::Task tab[1];
    tab[0] = taskPeriodico("a", &Timings::Valori::owmMin, runA);     // 2 cicli
    impostaOra(2026, 9, 10, 12, 0);
    g_msFinto = 9000000;
    boot(tab, 1);
    g_durataRunMs = 20000u;                    // un timeout da 20 s
    g_fallisciA = true;
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);   // ciclo 1: fallisce
    ok(tab[0].state.retrying && g_runA == 1, "fallimento: ritento prenotato");
    g_fallisciA = false;
    alProssimoPoll();
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);   // ciclo 2: ritento ok
    ok(g_runA == 2 && !tab[0].state.retrying && tab[0].state.slotCycle == 1,
       "  ritento al ciclo successivo, slot ancorato al ciclo in cui si era aperto");
    alProssimoPoll();
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);   // ciclo 3
    ok(g_runA == 3, "  la cadenza da 2 cicli riprende in fase: ciclo 3");

    // Due fallimenti: soglia raggiunta, si attende la cadenza piena.
    g_fallisciA = true;
    alProssimoPoll(); Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);   // 4: non dovuto
    alProssimoPoll(); Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);   // 5: fallisce
    alProssimoPoll(); Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);   // 6: ritento fallito
    ok(g_runA == 5 && !tab[0].state.retrying && tab[0].state.slotCycle == 5,
       "  secondo fallimento: soglia raggiunta, slot chiuso al ciclo 5");
    g_fallisciA = false;
  }

  // ---- rinvio al ciclo successivo: slot aperto, nessun tentativo speso ------
  {
    using O = Scheduler::Outcome;
    static Scheduler::Task tab[1];
    const auto giro = []() { alProssimoPoll(); azzeraEventi(); Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER); };

    // Cadenza del cinema: il rinvio apre lo slot, il download arriva al ciclo dopo.
    tab[0] = taskPeriodico("d", &Timings::Valori::cinemaMin, runD);   // 30 min = 6 cicli
    impostaOra(2026, 9, 10, 18, 0);
    g_msFinto = 90000000;
    sequenza({O::DEFERRED});
    boot(tab, 1);
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);   // ciclo 1: rinvio
    ok(g_runD == 1 && tab[0].state.deferred && !tab[0].state.retrying && tab[0].state.failedAttempts == 0,
       "rinvio: task in rinvio, nessun tentativo speso");
    ok(tab[0].state.outcomeCount == 0 && !Scheduler::isDue(tab[0]),
       "  il rinvio non conta fra gli esiti e nello stesso ciclo il task non è più dovuto");
    PrintFinto stampa;
    Scheduler::printStatus(stampa);
    giro();                                                            // ciclo 2: download
    ok(std::strcmp(g_eventi, "SRdrF") == 0 && g_runD == 2,
       "  al ciclo dopo il task è dovuto e la radio si accende");
    ok(!tab[0].state.deferred && tab[0].state.slotCycle == 1 && tab[0].state.okCount == 1,
       "  download riuscito: slot ancorato al ciclo del rinvio");
    for (int c = 3; c <= 6; ++c) giro();
    ok(g_runD == 2, "  nessun download fino alla cadenza piena");
    giro();                                                            // ciclo 7 = 1 + 6
    ok(g_runD == 3, "  slot successivo a 6 cicli dal rinvio, non dal download");

    // Rinvio e poi fallimento: il ritento c'è ancora.
    g_msFinto = 91000000;
    sequenza({O::DEFERRED, O::FAILED, O::OK});
    boot(tab, 1);
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);   // ciclo 1: rinvio
    giro();                                                            // ciclo 2: fallisce
    ok(tab[0].state.retrying && tab[0].state.failedAttempts == 1 && !tab[0].state.deferred,
       "rinvio poi fallimento: il fallimento è il primo dello slot e il ritento resta");
    giro();                                                            // ciclo 3: ritento ok
    ok(g_runD == 3 && !tab[0].state.retrying && tab[0].state.slotCycle == 1,
       "  ritento riuscito, slot ancorato al ciclo del rinvio");

    // Rinvio durante un ritento: non spende tentativi, il secondo fallimento chiude lo slot.
    g_msFinto = 92000000;
    sequenza({O::FAILED, O::DEFERRED, O::FAILED});
    boot(tab, 1);
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);   // ciclo 1: fallisce
    giro();                                                            // ciclo 2: rinvio
    ok(tab[0].state.retrying && tab[0].state.deferred && tab[0].state.failedAttempts == 1,
       "rinvio durante un ritento: il ritento resta e nessun tentativo si spende");
    giro();                                                            // ciclo 3: fallisce
    ok(g_runD == 3 && !tab[0].state.retrying && !tab[0].state.deferred && tab[0].state.slotCycle == 1,
       "  secondo fallimento: soglia raggiunta, slot chiuso al ciclo in cui si era aperto");

    // Radio giù al ciclo dopo il rinvio: il rinvio resta in sospeso.
    g_msFinto = 93000000;
    sequenza({O::DEFERRED});
    boot(tab, 1);
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);   // ciclo 1: rinvio
    g_radioOk = false;
    giro();                                                            // ciclo 2: radio giù
    ok(g_runD == 1 && tab[0].state.deferred, "radio giù dopo un rinvio: il rinvio resta in sospeso");
    g_radioOk = true;
    giro();                                                            // ciclo 3: download
    ok(g_runD == 2 && !tab[0].state.deferred && tab[0].state.slotCycle == 1,
       "  il download parte al primo ciclo con la radio, slot ancorato al rinvio");

    // Fronte di salita dopo un rinvio: slot nuovo, il rinvio vecchio non conta.
    static bool idoneo = true;
    tab[0].preconditionsMet = []() { return idoneo; };
    g_msFinto = 94000000;
    sequenza({O::DEFERRED});
    boot(tab, 1);
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);   // ciclo 1: rinvio
    idoneo = false;
    giro();                                                            // ciclo 2: sospeso
    ok(g_runD == 1 && tab[0].state.deferred, "precondizione falsa dopo un rinvio: nessuna esecuzione");
    idoneo = true;
    giro();                                                            // ciclo 3: fronte di salita
    ok(g_runD == 2 && !tab[0].state.deferred && tab[0].state.slotCycle == 3,
       "  fronte di salita: slot nuovo aperto al ciclo 3, il rinvio vecchio azzerato");
    tab[0].preconditionsMet = nullptr;
  }

  // ---- connessione fallita: stati intatti, frame comunque ------------------
  {
    static Scheduler::Task tab[1];
    tab[0] = taskPeriodico("a", &Timings::Valori::owmMin, runA);
    impostaOra(2026, 9, 10, 12, 0);
    g_msFinto = 12000000;
    boot(tab, 1);
    g_radioOk = false;
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
    ok(g_runA == 0 && tab[0].state.neverRun && g_frame == 1,
       "radio giù: nessun tentativo speso, frame del ciclo disegnato");
    g_radioOk = true;
    alProssimoPoll();
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
    ok(g_runA == 1, "  il task parte al ciclo successivo");
  }

  // ---- notte: solo il sensore ----------------------------------------------
  {
    static Scheduler::Task tab[1];
    tab[0] = taskPeriodico("a", &Timings::Valori::routeMin, runA);
    impostaOra(2026, 9, 10, 3, 0);
    g_msFinto = 15000000;
    boot(tab, 1);
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
    ok(std::strcmp(g_eventi, "S") == 0, "alle 03:00: solo il sensore, niente radio nè frame");
    const char* chi = nullptr;
    ok(Scheduler::msUntilNextEvent(&chi) == 301000u, "  prossimo risveglio fra un ciclo, per il sensore");

    // Senza sensore la notte non ha risvegli: si dorme fino all'apertura.
    g_sensoreAttivo = false;
    ok(Scheduler::msUntilNextEvent(&chi) == Scheduler::msUntilLocalHour(7, false) &&
           std::strcmp(chi, "apertura fascia") == 0,
       "senza sensore: sleep fino alle 07:00");
    impostaOra(2026, 9, 10, 6, 59);
    g_epochFinta += 40;                        // 06:59:40, risveglio anticipato dalla deriva
    ok(Scheduler::msUntilNextEvent(&chi) == 20000u, "  risveglio anticipato alle 06:59:40: si riaddormenta 20 s");

    // Notte del cambio dell'ora legale di marzo: le 02 non esistono.
    impostaOra(2026, 3, 29, 0, 30);
    struct tm b;
    localtime_r(&g_epochFinta, &b);
    b.tm_hour = 7; b.tm_min = 0; b.tm_sec = 0; b.tm_isdst = -1;
    const time_t alle7 = mktime(&b);
    ok(Scheduler::msUntilNextEvent(&chi) == (uint32_t)((alle7 - g_epochFinta) * 1000),
       "  attraverso l'ora legale: dalle 00:30 alle 07:00 sono 5 h 30 di orologio");
  }

  // ---- primo ciclo della giornata: slot riaperti, ritenti della sera azzerati ----
  {
    Timings::Valori v = Timings::get(); v.googleMin = 1440;
    ok(Timings::applica(v, nullptr), "cadenza di 24 h applicata");
    static Scheduler::Task tab[2];
    tab[0] = taskPeriodico("a", &Timings::Valori::googleMin, runA);  // 288 cicli
    tab[1] = taskPeriodico("b", &Timings::Valori::routeMin, runB);   // un ciclo
    impostaOra(2026, 9, 10, 22, 50);
    g_msFinto = 20000000;
    boot(tab, 2);
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);  // 22:50
    ok(g_runA == 1 && g_runB == 1, "alle 22:50 girano il periodico da 24 h e quello da un ciclo");

    // Un ciclo del sensore, con l'ora locale che avanza come in light sleep.
    const auto ciclo = []() {
      const uint32_t prima = g_msFinto;
      alProssimoPoll();
      g_epochFinta += (time_t)((g_msFinto - prima) / 1000u);
      azzeraEventi();
      Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
    };
    // Ora locale dell'inizio del prossimo ciclo e di quello dopo.
    const auto oraDelCiclo = [](int cicliAvanti) {
      const time_t at = g_epochFinta + (time_t)((g_ultimoPollMs + 301000u - g_msFinto) / 1000u)
                        + (time_t)(cicliAvanti * 301);
      struct tm local;
      localtime_r(&at, &local);
      return local.tm_hour;
    };
    // b fallisce solo nell'ultimo ciclo della fascia WiFi: il suo ritento resta in sospeso per la notte.
    while (oraDelCiclo(0) != 0)
    {
      g_fallisciB = oraDelCiclo(1) == 0;
      ciclo();
    }
    ok(tab[1].state.retrying && tab[1].state.failedAttempts == 1,
       "ultimo ciclo della sera: b fallisce e il ritento resta in sospeso per la notte");
    const int runBSera = g_runB;
    while (oraDelCiclo(0) != 7) ciclo();
    ok(g_runB == runBSera, "  di notte nessun ritento");
    ciclo();                                   // primo ciclo delle 07, b fallisce ancora
    ok(g_runA == 2, "primo ciclo delle 07: il periodico da 24 h è dovuto, solo 8 h dopo");
    ok(g_runB == runBSera + 1 && tab[1].state.retrying && tab[1].state.failedAttempts == 1,
       "  il ritento della sera non si trascina: il fallimento delle 07 è il primo dello slot");
    ok(g_frame >= 2 && std::strchr(g_eventi, 'F') != nullptr, "  e il primo frame del mattino è disegnato");
    g_fallisciB = false;
    ciclo();
    ok(g_runB == runBSera + 2 && !tab[1].state.retrying, "  al ciclo dopo il ritento riesce");
    Timings::ripristinaDefault();
  }

  // ---- fronte di salita della precondizione: slot nuovo e dovuto ------------
  {
    static bool idoneo = true;
    static Scheduler::Task tab[1];
    tab[0] = taskPeriodico("a", &Timings::Valori::owmMin, runA);     // 2 cicli
    tab[0].preconditionsMet = []() { return idoneo; };
    impostaOra(2026, 9, 10, 12, 0);
    g_msFinto = 25000000;
    boot(tab, 1);
    const auto giro = []() { alProssimoPoll(); Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER); };
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);  // ciclo 1: gira, slot chiuso
    g_fallisciA = true;
    giro();                                    // ciclo 2: non dovuto
    giro();                                    // ciclo 3: fallisce
    ok(g_runA == 2 && tab[0].state.retrying, "fallimento: ritento in sospeso");
    idoneo = false;
    giro();                                    // ciclo 4: sospeso, il ritento non parte
    ok(g_runA == 2, "  precondizione falsa: nessun tentativo");
    idoneo = true;
    giro();                                    // ciclo 5: fronte di salita, slot nuovo
    ok(g_runA == 3 && tab[0].state.retrying && tab[0].state.failedAttempts == 1,
       "  al ritorno della precondizione lo slot riparte: il fallimento è il primo, non il secondo");
    g_fallisciA = false;
    giro();                                    // ciclo 6: ritento riuscito
    ok(g_runA == 4 && !tab[0].state.retrying && tab[0].state.slotCycle == 5, "  e il ritento al ciclo dopo riesce");
    giro();                                    // ciclo 7: la cadenza riparte dal ciclo 5
    ok(g_runA == 5, "  la cadenza da 2 cicli riprende dal nuovo slot");
    giro();                                    // ciclo 8: precondizione stabile, nessuna riapertura
    ok(g_runA == 5, "  con la precondizione stabile nessuna riapertura dello slot");
  }

  // ---- ora non valida senza task dovuti: lo scheduler riprende l'ora ------
  {
    static Scheduler::Task tab[1];
    tab[0] = taskPeriodico("b", &Timings::Valori::routeMin, runB);   // richiede l'ora, come i calendari
    tab[0].preconditionsMet = []() { return Clock::valido(); };
    const auto accensioni = []() {
      int n = 0;
      for (const char* p = g_eventi; *p; ++p) n += (*p == 'R');
      return n;
    };
    const auto giro = []() { Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER); };

    // SNTP che non risponde: la radio si riaccende ogni CLOCK_RECOVERY_CYCLES, il frame a ogni ciclo.
    g_epochFinta = 1000;
    g_msFinto = 31000000;
    boot(tab, 1);
    giro();
    ok(accensioni() == 1 && g_runB == 0 && g_frame == 1,
       "ora non valida, nessun task dovuto: radio accesa per l'ora, poi il frame");
    for (int i = 1; i < CLOCK_RECOVERY_CYCLES; ++i) { alProssimoPoll(); giro(); }
    ok(accensioni() == 1 && g_frame == CLOCK_RECOVERY_CYCLES,
       "  nei cicli successivi niente radio, il frame sì");
    alProssimoPoll();
    giro();
    ok(accensioni() == 2 && g_frame == CLOCK_RECOVERY_CYCLES + 1,
       "  dopo CLOCK_RECOVERY_CYCLES cicli la radio riprova");
    ok(g_runB == 0, "  il task che chiede l'ora resta sospeso");

    // SNTP che risponde: il task che chiede l'ora gira nello stesso giro.
    g_epochFinta = 1000;
    boot(tab, 1);
    Scheduler::Hooks h = hooksFinti();
    h.radioOn = []() { evento('R'); impostaOra(2026, 9, 10, 12, 0); return true; };
    Scheduler::begin(tab, 1, h);
    giro();
    ok(accensioni() == 1 && g_runB == 1 && g_frame == 1,
       "ora ripresa dalla radio: il task che la chiede gira nello stesso giro, poi il frame");

    // Ora valida e nessun task dovuto: nessuna accensione.
    impostaOra(2026, 9, 10, 12, 0);
    tab[0].preconditionsMet = []() { return false; };
    boot(tab, 1);
    giro();
    ok(accensioni() == 0 && g_frame == 1, "ora valida senza task dovuti: niente radio, solo il frame");

    // Finestra di manutenzione al boot: il primo frame aspetta l'ora al più TIME_SYNC_TIMEOUT_MS
    // dalla STA vista, poi parte comunque.
    tab[0].preconditionsMet = []() { return Clock::valido(); };
    g_epochFinta = 1000;
    boot(tab, 1);
    const auto giroManutenzione = []() { Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE); };
    giroManutenzione();
    ok(g_frame == 0, "manutenzione, ora non valida e nessun task dovuto: il primo frame aspetta l'ora");
    g_msFinto += TIME_SYNC_TIMEOUT_MS;
    giroManutenzione();
    ok(g_frame == 1 && g_runB == 0 && accensioni() == 0,
       "  allo scadere del budget il frame parte senza ora, e la radio della manutenzione non si tocca");
  }

  // ---- finestra di manutenzione al boot ------------------------------------
  {
    static Scheduler::Task tab[1];
    tab[0] = taskPeriodico("a", &Timings::Valori::owmMin, runA);
    impostaOra(2026, 9, 10, 12, 0);
    g_msFinto = 40000000;
    boot(tab, 1);
    WiFi.stato = 0;                            // STA non ancora connessa
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE);
    ok(g_poll == 1 && g_runA == 0 && g_frame == 0, "manutenzione, STA giù: sensore sì, fetch e frame in attesa");
    g_msFinto += MAINTENANCE_SCHEDULER_PERIOD_MS / 2;
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE);
    ok(g_runA == 0, "  a mezzo periodo il giro non si rivaluta");
    g_msFinto += 3000;
    WiFi.stato = WL_CONNECTED;
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE);
    ok(g_runA == 1 && g_frame == 1, "  STA su: fetch e poi frame, nello stesso giro");
    ok(std::strchr(g_eventi, 'R') == nullptr, "  la radio della manutenzione non si tocca");

    // STA che non sale: il frame parte con i placeholder dopo RADIO_STA_ATTESA_MS.
    tab[0].state = {};
    boot(tab, 1);
    WiFi.stato = 0;
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE);
    g_msFinto += RADIO_STA_ATTESA_MS - 1000;
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE);
    ok(g_frame == 0 && g_runA == 0, "STA mai salita: prima del budget niente frame");
    g_msFinto += 1000;
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE);
    ok(g_frame == 1 && g_runA == 0, "  a RADIO_STA_ATTESA_MS frame con i placeholder, fetch non eseguiti");

    // Orologio da sincronizzare: i fetch aspettano l'ora dalla STA vista, poi partono comunque.
    tab[0].state = {};
    g_epochFinta = 1000;
    boot(tab, 1);
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE);
    ok(g_runA == 0 && g_frame == 0, "orologio non valido in manutenzione: fetch e frame aspettano l'ora");
    g_msFinto += TIME_SYNC_TIMEOUT_MS - 1000;
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE);
    ok(g_runA == 0 && g_frame == 0, "  a un secondo dal budget dell'ora ancora in attesa");
    g_msFinto += 1000;
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE);
    ok(g_runA == 1 && g_frame == 1, "  a TIME_SYNC_TIMEOUT_MS dalla STA vista partono senza ora, poi il frame");

    // Client sulle pagine: lo scheduler è fermo, il sensore no.
    impostaOra(2026, 9, 10, 12, 0);
    const int pollPrima = g_poll;
    alProssimoPoll();
    Scheduler::serviceSensor();
    ok(g_poll == pollPrima + 1, "client attivo: serviceSensor() interroga il sensore alla scadenza");
    Scheduler::serviceSensor();
    ok(g_poll == pollPrima + 1, "  e non prima della scadenza successiva");
  }

  // ---- primo frame dopo l'accensione: STA, ora e task dipendenti dall'ora ---
  {
    static Scheduler::Task tab[2];
    tab[0] = taskPeriodico("a", &Timings::Valori::routeMin, runA);   // senza precondizioni
    tab[1] = taskPeriodico("b", &Timings::Valori::routeMin, runB);   // richiede l'ora, come calendari e percorso
    tab[1].preconditionsMet = []() { return Clock::valido(); };
    const auto giro = []() { Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE); };

    // Regressione: STA vista a 5 s, ora valida a 18 s, quindi dopo i 15 s dal boot. Prima i fetch
    // partivano a 15 s senza ora, il frame subito dopo, e il task dell'ora girava a frame fatto.
    g_epochFinta = 1000;
    g_msFinto = 60000000;
    boot(tab, 2);
    WiFi.stato = 0;
    giro();
    g_msFinto += 5000;
    WiFi.stato = WL_CONNECTED;
    giro();
    ok(g_runA == 0 && g_runB == 0 && g_frame == 0, "accensione, STA vista a 5 s: nessun fetch, il budget dell'ora corre");
    g_msFinto += 10000;
    giro();
    ok(g_runA == 0 && g_runB == 0 && g_frame == 0, "  a 15 s dal boot niente fetch nè frame: il timer del boot non decide più");
    g_msFinto += 3000;
    impostaOra(2026, 9, 10, 8, 30);
    giro();
    ok(g_runA == 1 && g_runB == 1 && g_frame == 1, "  ora valida a 18 s: entrambi i task nello stesso passo, poi un solo frame");
    ok(std::strstr(g_eventi, "abF") != nullptr && std::strchr(g_eventi, 'R') == nullptr,
       "  nell'ordine task, task, frame, e la radio della manutenzione non si tocca");

    // L'ora non arriva: partono i task che non la richiedono, il frame esce, l'altro resta al ciclo dopo.
    g_epochFinta = 1000;
    g_msFinto = 61000000;
    boot(tab, 2);
    giro();
    g_msFinto += TIME_SYNC_TIMEOUT_MS + 1000;
    giro();
    ok(g_runA == 1 && g_runB == 0 && g_frame == 1, "ora mai valida: il task senza ora parte, quello con l'ora resta, frame comunque");

    // STA vista poi caduta prima dell'ora: allo scadere del budget della STA il frame esce, senza stallo.
    g_epochFinta = 1000;
    g_msFinto = 62000000;
    boot(tab, 2);
    giro();
    g_msFinto += 5000;
    giro();
    WiFi.stato = 0;
    g_msFinto += 5000;
    giro();
    ok(g_frame == 0, "STA caduta a 10 s con budget non scaduto: ancora in attesa");
    g_msFinto += RADIO_STA_ATTESA_MS;
    giro();
    ok(g_frame == 1 && g_runA == 0, "  scaduto il budget della STA il frame esce con i placeholder");

    // STA che sale dopo il budget: i task non girano nel ciclo del frame, partono al ciclo dopo.
    g_epochFinta = 1000;
    g_msFinto = 63000000;
    boot(tab, 2);
    WiFi.stato = 0;
    giro();
    g_msFinto += RADIO_STA_ATTESA_MS;
    giro();
    ok(g_frame == 1 && g_runA == 0, "STA assente al budget: frame con i placeholder");
    WiFi.stato = WL_CONNECTED;
    impostaOra(2026, 9, 10, 8, 30);
    g_msFinto += 2000;
    giro();
    ok(g_runA == 0 && g_frame == 1, "  STA salita dopo: nel ciclo del frame i task non partono");
    alProssimoPoll();
    giro();
    ok(g_runA == 1 && g_runB == 1 && g_frame == 2, "  al ciclo successivo girano, con il loro frame");

    // Riavvio software con l'ora ancora valida: nessuna attesa dell'ora.
    impostaOra(2026, 9, 10, 8, 30);
    g_msFinto = 64000000;
    boot(tab, 2);
    giro();
    ok(g_runA == 1 && g_runB == 1 && g_frame == 1, "ora già valida al boot: task e frame al primo giro, senza attese");

    // Boot di notte con l'ora da sincronizzare: la notte si scopre a ora arrivata, niente fetch nè frame.
    g_epochFinta = 1000;
    g_msFinto = 65000000;
    boot(tab, 2);
    giro();
    ok(g_runA == 0 && g_frame == 0, "boot di notte senza ora: in attesa dell'ora");
    impostaOra(2026, 9, 10, 3, 0);
    g_msFinto += 2000;
    giro();
    ok(g_runA == 0 && g_runB == 0 && g_frame == 0, "  ora arrivata, sono le 03:00: nessun fetch nè frame");
  }

  // ---- ritento solo al ciclo successivo, radio della manutenzione ----------
  {
    static Scheduler::Task tab[1];
    tab[0] = taskPeriodico("a", &Timings::Valori::routeMin, runA);   // un ciclo
    const auto giro = []() { Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_MAINTENANCE); };

    // Primo passo dopo il boot: il frame non aspetta, nessun secondo tentativo nel ciclo.
    impostaOra(2026, 9, 10, 8, 30);
    g_msFinto = 70000000;
    boot(tab, 1);
    g_fallisciA = true;
    giro();
    ok(g_runA == 1 && tab[0].state.retrying && g_frame == 1, "fallimento al boot: ritento in sospeso, frame subito");
    g_fallisciA = false;
    g_msFinto += 10000;
    giro();
    ok(g_runA == 1, "  nessun secondo tentativo nello stesso ciclo");
    alProssimoPoll();
    giro();
    ok(g_runA == 2 && !tab[0].state.retrying && g_frame == 2, "  il ritento al ciclo successivo riesce, con il suo frame");

    // Due fallimenti in due cicli: slot chiuso, un frame per ciclo, nessuno stallo.
    impostaOra(2026, 9, 10, 8, 30);
    g_msFinto = 71000000;
    boot(tab, 1);
    g_fallisciA = true;
    giro();
    alProssimoPoll();
    giro();
    ok(g_runA == 2 && !tab[0].state.retrying && tab[0].state.slotCycle == 1 && g_frame == 2,
       "due fallimenti in due cicli: soglia raggiunta, slot chiuso, un frame per ciclo");
    g_fallisciA = false;

    // Un task con maxAttempts 0 non ritenta.
    tab[0] = taskPeriodico("a", &Timings::Valori::routeMin, runA, 0);
    impostaOra(2026, 9, 10, 8, 30);
    g_msFinto = 72000000;
    boot(tab, 1);
    g_fallisciA = true;
    giro();
    ok(g_runA == 1 && !tab[0].state.retrying && g_frame == 1, "maxAttempts 0: un solo tentativo per slot");
    g_fallisciA = false;

    // STA giù al ciclo del ritento: il ritento resta per il ciclo con la STA su.
    tab[0] = taskPeriodico("a", &Timings::Valori::routeMin, runA);
    impostaOra(2026, 9, 10, 8, 30);
    g_msFinto = 73000000;
    boot(tab, 1);
    g_fallisciA = true;
    giro();
    g_fallisciA = false;
    WiFi.stato = 0;
    alProssimoPoll();
    giro();
    g_msFinto += RADIO_STA_ATTESA_MS;
    giro();
    ok(g_runA == 1 && tab[0].state.retrying && g_frame == 2, "STA giù al ciclo del ritento: frame, ritento intatto");
    WiFi.stato = WL_CONNECTED;
    alProssimoPoll();
    giro();
    ok(g_runA == 2 && !tab[0].state.retrying, "  con la STA su il ritento parte e riesce");
  }

  // ---- ritento solo al ciclo successivo, radio dello scheduler --------------
  {
    static Scheduler::Task tab[2];
    tab[0] = taskPeriodico("a", &Timings::Valori::routeMin, runA);
    tab[1] = taskPeriodico("b", &Timings::Valori::routeMin, runB);
    const auto giro = []() { Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER); };

    // Fallimento rapido: una sola accensione, nessuna attesa, ritento al ciclo dopo.
    impostaOra(2026, 9, 10, 12, 0);
    g_msFinto = 80000000;
    boot(tab, 1);
    g_fallisciPrimi = 1;
    const uint32_t prima = g_msFinto;
    giro();
    ok(std::strcmp(g_eventi, "SRarF") == 0 && tab[0].state.retrying,
       "fallimento rapido: un tentativo, radio giù e frame, ritento prenotato");
    ok(g_msFinto - prima == (uint32_t)DISPLAY_REFRESH_PIENO_S * 1000u, "  nessuna attesa dentro il passo");
    azzeraEventi();
    alProssimoPoll();
    giro();
    ok(std::strcmp(g_eventi, "SRarF") == 0 && g_runA == 2 && !tab[0].state.retrying && tab[0].state.slotCycle == 1,
       "  al ciclo successivo riesce, slot ancorato al ciclo 1");

    // Radio caduta al fallimento: ritento prenotato.
    impostaOra(2026, 9, 10, 12, 0);
    g_msFinto = 84000000;
    boot(tab, 1);
    g_fallisciPrimi = 1;
    g_radioCadeAlFallimento = true;
    giro();
    ok(std::strcmp(g_eventi, "SRarF") == 0 && tab[0].state.retrying, "radio caduta al fallimento: ritento prenotato");
    WiFi.stato = WL_CONNECTED;

    // Due task falliti nello stesso passo: una sola passata, entrambi al ciclo dopo.
    impostaOra(2026, 9, 10, 12, 0);
    g_msFinto = 85000000;
    boot(tab, 2);
    g_fallisciPrimi = 1;
    g_fallisciB = true;
    giro();
    ok(std::strcmp(g_eventi, "SRabrF") == 0 && tab[0].state.retrying && tab[1].state.retrying,
       "due fallimenti nello stesso passo: una sola passata, entrambi ritentano al ciclo dopo");
    g_fallisciB = false;
  }

  // ---- senza sensore: ciclo interno ----------------------------------------
  {
    static Scheduler::Task tab[1];
    tab[0] = taskPeriodico("a", &Timings::Valori::routeMin, runA);
    impostaOra(2026, 9, 10, 12, 0);
    g_msFinto = 50000000;
    boot(tab, 1);
    g_sensoreAttivo = false;
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
    ok(g_poll == 0 && g_runA == 1 && g_frame == 1, "senza sensore: ciclo interno, task e frame");
    const char* chi = nullptr;
    const uint32_t attesa = Scheduler::msUntilNextEvent(&chi);
    ok(attesa == CICLO_MS - (uint32_t)DISPLAY_REFRESH_PIENO_S * 1000u, "  prossimo ciclo a CYCLE_S dall'inizio del precedente");
    avanzaMs(attesa);
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
    ok(g_runA == 2 && g_frame == 2, "  secondo ciclo interno");
  }

  // ---- sensore senza scadenza propria (BME280): ciclo interno, notte libera -
  {
    static Scheduler::Task tab[1];
    tab[0] = taskPeriodico("a", &Timings::Valori::routeMin, runA);
    impostaOra(2026, 9, 10, 12, 0);
    g_msFinto = 60000000;
    boot(tab, 1);
    Scheduler::Hooks h = hooksFinti();
    h.msUntilSensorPoll = nullptr;             // come il .ino col BME280
    Scheduler::begin(tab, 1, h);
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
    ok(g_poll == 1 && g_eventi[0] == 'S' && g_frame == 1,
       "sensore senza scadenza: lettura in testa al ciclo, poi task e frame");
    const char* chi = nullptr;
    const uint32_t attesa = Scheduler::msUntilNextEvent(&chi);
    ok(attesa == CICLO_MS - (uint32_t)DISPLAY_REFRESH_PIENO_S * 1000u && std::strcmp(chi, "ciclo") == 0,
       "  ciclo interno: il prossimo a CYCLE_S dall'inizio del precedente");
    avanzaMs(attesa);
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
    ok(g_poll == 2 && g_frame == 2, "  secondo ciclo: nuova lettura e frame");

    // Di notte il ciclo si apre senza lettura, e lo sleep arriva all'apertura.
    impostaOra(2026, 9, 10, 3, 0);
    avanzaMs(CICLO_MS);
    azzeraEventi();
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
    ok(g_poll == 2 && g_eventi[0] == 0, "  alle 03:00: nessuna lettura, radio nè frame");
    const uint32_t notte = Scheduler::msUntilNextEvent(&chi);
    ok(notte == Scheduler::msUntilLocalHour(7, false) && std::strcmp(chi, "apertura fascia") == 0,
       "  nessun risveglio fino alle 07:00");

    // Alla riapertura la lettura torna in testa al ciclo, prima del frame.
    avanzaMs(notte);
    azzeraEventi();
    Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
    ok(g_poll == 3 && g_eventi[0] == 'S' && std::strchr(g_eventi, 'F') != nullptr,
       "  alle 07:00: lettura prima del frame");
  }

  // ---- rollover di millis() ------------------------------------------------
  {
    static Scheduler::Task tab[1];
    tab[0] = taskPeriodico("a", &Timings::Valori::owmMin, runA);
    impostaOra(2026, 9, 10, 12, 0);
    g_msFinto = 0xFFFFFF00u;                   // a un passo dal giro dei 49,7 giorni
    boot(tab, 1);
    for (int i = 0; i < 4; ++i)
    {
      Scheduler::runRound(Scheduler::RadioMode::MANAGED_BY_SCHEDULER);
      alProssimoPoll();
    }
    ok(g_poll == 4 && g_runA == 2 && g_frame == 4, "rollover di millis(): cicli e cadenze regolari");
  }

  // ---- fascia oraria e orologio non sincronizzato --------------------------
  impostaOra(2026, 9, 10, 3, 0);
  ok(!Scheduler::isInsideRadioHours(), "alle 03:00 fuori dalla fascia 7..23");
  g_epochFinta = 1000;
  ok(!Clock::valido() && Scheduler::isInsideRadioHours(),
     "orologio non sincronizzato: fascia fail-open, altrimenti il primo SNTP non avverrebbe mai");

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

  std::printf("\n%s\n", falliti ? "CI SONO FALLIMENTI" : "tutti i controlli superati");
  return falliti != 0;
}

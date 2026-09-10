/**
 * Verifica della logica temporale di Scheduler.h: cadenze, coalescing,
 * ritenti, cadenza giornaliera con recupero, fascia oraria, ora legale,
 * rollover di millis() e clamp del light sleep.
 *
 * Include l'header VERO del firmware (-I..), non una copia. L'orologio di
 * sistema e' sostituito da una macro perche' Scheduler.h chiama time(nullptr);
 * localtime_r e mktime restano quelli di libc, cosi' il fuso Europe/Rome e il
 * cambio dell'ora legale sono quelli veri.
 */
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static time_t g_epochFinta = 0;
static inline time_t tempoFinto(time_t* p) { if (p) *p = g_epochFinta; return g_epochFinta; }
#define time(p) tempoFinto(p)
#include "Scheduler.h"
#undef time

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

static Scheduler::Esito esitoOk() { return Scheduler::Esito::OK; }
static Scheduler::Esito esitoKo() { return Scheduler::Esito::FALLITO; }

int main()
{
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  Timings::begin();
  const uint32_t MIN = 60u * 1000u;

  // ---- cadenza periodica e coalescing ------------------------------------
  Scheduler::Task p {};
  p.tag = "p"; p.cadenza = Scheduler::Cadenza::PERIODICA; p.richiedeRadio = false;
  p.intervalloMin = &Timings::Valori::owmMin;            // 10 min
  p.intervalloRitentoMs = FETCH_RITENTO_MS; p.maxTentativi = FETCH_MAX_TENTATIVI;
  p.esegui = esitoOk;

  impostaOra(2026, 9, 10, 12, 0);
  g_msFinto = 1000;
  ok(Scheduler::msAllaScadenza(p, g_msFinto, oggiLocale()) == 0,
     "periodica mai eseguita: dovuta subito");
  Scheduler::registraEsito(p, Scheduler::Esito::OK, g_msFinto, 5, oggiLocale());
  ok(!p.stato.maiEseguito && p.stato.ultimoMs == 1000, "  slot consumato dopo l'esito");

  g_msFinto = 1000 + 9 * MIN;
  ok(!Scheduler::dovuto(p, g_msFinto, oggiLocale(), 0), "a 9 min: non dovuta (scadenza stretta)");
  ok(Scheduler::dovuto(p, g_msFinto, oggiLocale(), 2 * MIN), "a 9 min: dovuta con coalescing 2 min");
  g_msFinto = 1000 + 10 * MIN;
  ok(Scheduler::dovuto(p, g_msFinto, oggiLocale(), 0), "a 10 min: dovuta");

  // ---- ritento e soglia --------------------------------------------------
  Scheduler::Task r = p; r.tag = "r"; r.esegui = esitoKo; r.stato = {};
  g_msFinto = 100000;
  Scheduler::registraEsito(r, Scheduler::Esito::FALLITO, g_msFinto, 5, oggiLocale());
  ok(r.stato.inRitento && r.stato.tentativi == 1, "1o fallimento: in ritento");
  ok(Scheduler::msAllaScadenza(r, g_msFinto, oggiLocale()) == FETCH_RITENTO_MS,
     "  scadenza = intervallo di ritento");
  ok(!Scheduler::dovuto(r, g_msFinto + FETCH_RITENTO_MS - 1, oggiLocale(), 0),
     "  non dovuta prima del ritento");
  ok(Scheduler::dovuto(r, g_msFinto + FETCH_RITENTO_MS, oggiLocale(), 0),
     "  dovuta allo scadere del ritento");
  g_msFinto += FETCH_RITENTO_MS;
  Scheduler::registraEsito(r, Scheduler::Esito::FALLITO, g_msFinto, 5, oggiLocale());
  ok(!r.stato.inRitento && r.stato.tentativi == 0,
     "2o fallimento: soglia raggiunta, slot consumato");
  ok(Scheduler::msAllaScadenza(r, g_msFinto, oggiLocale()) == 10 * MIN,
     "  si attende la cadenza piena");

  // ---- esito saltato: guasto a monte, non del task -----------------------
  Scheduler::Task s = p; s.tag = "s"; s.stato = {};
  g_msFinto = 200000;
  Scheduler::registraEsito(s, Scheduler::Esito::SALTATO, g_msFinto, 0, oggiLocale());
  ok(s.stato.maiEseguito && s.stato.tentativi == 0 && s.stato.esiti == 0,
     "saltato: slot intatto, nessun tentativo speso");

  // ---- cadenza giornaliera -----------------------------------------------
  Scheduler::Task g {};
  g.tag = "g"; g.cadenza = Scheduler::Cadenza::GIORNALIERA; g.richiedeRadio = false;
  g.oraLocale = &Timings::Valori::cinemaOra;             // ora 7
  g.intervalloRitentoMs = FETCH_RITENTO_MS; g.maxTentativi = FETCH_MAX_TENTATIVI;
  g.esegui = esitoOk;
  g.stato.maiEseguito = false; g.stato.ultimoGiorno = -1; g.stato.ultimoMs = 0;

  impostaOra(2026, 9, 10, 6, 0);
  g_msFinto = 50u * 3600u * 1000u;                        // ultimoMs lontano
  {
    uint32_t d = Scheduler::msAllaScadenza(g, g_msFinto, oggiLocale());
    ok(d > 55 * MIN && d < 65 * MIN, "giornaliera alle 06:00 con ora 7: manca circa un'ora");
  }
  impostaOra(2026, 9, 10, 7, 30);
  ok(Scheduler::msAllaScadenza(g, g_msFinto, oggiLocale()) == 0,
     "alle 07:30, mai fatta oggi: dovuta (recupero con >=)");
  impostaOra(2026, 9, 10, 11, 0);
  ok(Scheduler::msAllaScadenza(g, g_msFinto, oggiLocale()) == 0,
     "alle 11:00, mai fatta oggi: ancora recuperabile");
  Scheduler::registraEsito(g, Scheduler::Esito::OK, g_msFinto, 5, oggiLocale());
  ok(g.stato.ultimoGiorno == oggiLocale()->tm_yday, "  giorno registrato");
  {
    uint32_t d = Scheduler::msAllaScadenza(g, g_msFinto, oggiLocale());
    ok(d > 19u * 3600u * 1000u && d < 21u * 3600u * 1000u, "  poi mancano ~20 h a domani alle 07");
  }
  impostaOra(2026, 9, 11, 9, 0);
  g_msFinto += 22u * 3600u * 1000u;
  ok(Scheduler::msAllaScadenza(g, g_msFinto, oggiLocale()) == 0,
     "giorno dopo alle 09:00: dovuta (recupero)");

  // ---- fascia oraria -----------------------------------------------------
  Scheduler::Task n = p; n.tag = "n"; n.richiedeRadio = true; n.stato = {};
  impostaOra(2026, 9, 10, 3, 0);
  ok(!Scheduler::inFascia(), "alle 03:00 fuori dalla fascia 7..23");
  {
    uint32_t e = Scheduler::msAllaScadenzaEffettiva(n, g_msFinto, oggiLocale());
    ok(e > 3u * 3600u * 1000u && e < 5u * 3600u * 1000u,
       "  task di rete rimandato all'apertura della fascia");
  }
  impostaOra(2026, 9, 10, 12, 0);
  ok(Scheduler::inFascia() && Scheduler::msAllaScadenzaEffettiva(n, g_msFinto, oggiLocale()) == 0,
     "a mezzogiorno: dentro la fascia, dovuta");

  // ---- orologio non sincronizzato ----------------------------------------
  g_epochFinta = 1000;
  ok(!Scheduler::orologioValido(), "epoch bassa: orologio non sincronizzato");
  ok(Scheduler::inFascia(), "  fascia fail-open, altrimenti il primo SNTP non avverrebbe mai");
  ok(Scheduler::msAllaScadenza(g, g_msFinto, nullptr) == Scheduler::MAI,
     "  cadenza giornaliera sospesa");

  // ---- cambio dell'ora legale --------------------------------------------
  {
    impostaOra(2026, 10, 24, 23, 30);          // sabato prima del cambio del 25 ottobre
    struct tm b;
    localtime_r(&g_epochFinta, &b);
    b.tm_mday += 1; b.tm_hour = 7; b.tm_min = 0; b.tm_sec = 0; b.tm_isdst = -1;
    time_t atteso = mktime(&b);
    uint32_t d = Scheduler::msFinoAllOra(7, false);
    ok((uint32_t)((atteso - g_epochFinta) * 1000) == d,
       "ora legale: msFinoAllOra coincide con il calcolo di libc");
    ok(d > 8u * 3600u * 1000u, "  attraversando il cambio la distanza cresce di un'ora");
  }

  // ---- rollover di millis() ----------------------------------------------
  {
    Scheduler::Task w = p; w.tag = "w"; w.stato = {};
    g_msFinto = 0xFFFFFF00u;                   // a un passo dal giro dei 49,7 giorni
    impostaOra(2026, 9, 10, 12, 0);
    Scheduler::registraEsito(w, Scheduler::Esito::OK, g_msFinto, 5, oggiLocale());
    g_msFinto = 0xFFFFFF00u + 9 * MIN;         // qui millis() ha gia' wrappato
    ok(!Scheduler::dovuto(w, g_msFinto, oggiLocale(), 0), "rollover millis: a 9 min non dovuta");
    g_msFinto = 0xFFFFFF00u + 10 * MIN;
    ok(Scheduler::dovuto(w, g_msFinto, oggiLocale(), 0), "rollover millis: a 10 min dovuta");
  }

  // ---- clamp del light sleep ---------------------------------------------
  {
    static Scheduler::Task tab[1];
    tab[0] = p; tab[0].tag = "solo"; tab[0].stato = {};
    Scheduler::begin(tab, 1, nullptr, nullptr);
    g_msFinto = 500000;
    impostaOra(2026, 9, 10, 12, 0);
    const char* chi = nullptr;
    Scheduler::registraEsito(tab[0], Scheduler::Esito::OK, g_msFinto, 5, oggiLocale());
    ok(Scheduler::msAlProssimoEvento(&chi) == (uint32_t)SLEEP_MAX_S * 1000u,
       "scadenza a 10 min: sleep limitato al tetto di 300 s");
    g_msFinto += 10 * MIN - 5000u;             // mancano 5 s
    ok(Scheduler::msAlProssimoEvento(&chi) == (uint32_t)SLEEP_MIN_S * 1000u,
       "scadenza fra 5 s: sleep alzato al pavimento di 30 s");
    ok(std::strcmp(chi, "solo") == 0, "  e si sa quale task ha deciso il risveglio");
    tab[0].pronto = []() { return false; };
    ok(Scheduler::msAlProssimoEvento(&chi) == (uint32_t)SLEEP_MAX_S * 1000u &&
           std::strcmp(chi, "nessuno") == 0,
       "nessun task pronto: sleep al tetto, log esplicito");
  }

  std::printf("\n%s\n", falliti ? "CI SONO FALLIMENTI" : "tutti i controlli superati");
  return falliti != 0;
}

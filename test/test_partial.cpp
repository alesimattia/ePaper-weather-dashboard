/**
 * Verifica del refresh differenziale: la macchina a stati del driver
 * (GxEPD2_SOLUM_Differential.h) e la politica del firmware (PartialRefresh.h).
 *
 * Il driver vero non gira su host: la struct dello stato non ha I/O proprio
 * perchè questa verifica sia possibile. Include gli header VERI (-I..), non
 * copie; gli stub in stub/ coprono solo la piattaforma.
 */
#include "PartialRefresh.h"
#include "GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM_Differential.h"
#include <cmath>
#include <cstdio>
#include <cstring>

uint32_t g_msFinto = 0;
SerialeFinta Serial;

static int falliti = 0;
static void ok(bool c, const char* nome)
{
  std::printf("%-72s %s\n", nome, c ? "ok" : "FALLITO");
  if (!c) ++falliti;
}

using Diff = GxEPD2_SOLUM_Differential;
using Action = Diff::PageAction;

static const int16_t HEIGHT = 672;
static const int16_t PAGE_H = 84;
static const int PAGES = 8;

/** CRC finti: la pagina i vale base + i, così due frame con la stessa base sono uguali. */
static uint32_t crcOf(uint32_t base, int page) { return base + (uint32_t)page; }

/** Un frame pieno completo: tutte le pagine WriteFull e commit. */
static void fullFrame(Diff& d, uint32_t base)
{
  d.disarm();
  d.beginFrame();
  for (int i = 0; i < PAGES; ++i) d.onPage(crcOf(base, i), (int16_t)(i * PAGE_H), PAGE_H);
  d.commitFull(HEIGHT);
}

/** Un frame armato: ritorna true se tutte le pagine sono WriteNew. Non fa commit. */
static bool armedFrame(Diff& d, uint32_t base, int changedPage = -1, int changedPage2 = -1)
{
  d.beginFrame();
  bool allNew = true;
  for (int i = 0; i < PAGES; ++i)
  {
    const uint32_t crc = (i == changedPage || i == changedPage2) ? crcOf(base, i) ^ 0xFFFFu : crcOf(base, i);
    if (d.onPage(crc, (int16_t)(i * PAGE_H), PAGE_H) != Action::WriteNew) allNew = false;
  }
  return allNew;
}

// -------------------------------------------------------------------------
// Pannelli finti per il trait: uno con l'API, uno senza, uno che la nega.
// -------------------------------------------------------------------------
struct PanelWith
{
  static const bool hasDifferentialRefresh = true;
  bool enabled = false;
  bool armResult = true;
  bool fired = true;
  void setDifferentialRefresh(bool on) { enabled = on; }
  bool beginDifferentialFrame() { return armResult; }
  bool endDifferentialFrame() { return fired; }
  uint8_t differentialChain() const { return 3; }
  uint16_t differentialRefusedPages() const { return 0x0A; }
};
struct PanelWithout {};
struct PanelNegated { static const bool hasDifferentialRefresh = false; };

// Driver che sa saltare il refresh di un frame invariato, con o senza differenziale.
struct PanelSkip
{
  bool skipOn = false;
  bool skipped = false;
  void setSkipUnchanged(bool on) { skipOn = on; }
  bool frameSkipped() const { return skipped; }
};
struct PanelSkipAndDiff : PanelWith
{
  bool skipOn = false;
  bool skipped = false;
  void setSkipUnchanged(bool on) { skipOn = on; }
  bool frameSkipped() const { return skipped; }
};
struct PanelHalfSkip { void setSkipUnchanged(bool) {} };   // senza frameSkipped(): non vale

static_assert(PartialRefresh::SupportsDifferential<PanelWith>::value, "PanelWith deve essere riconosciuto");
static_assert(!PartialRefresh::SupportsDifferential<PanelWithout>::value, "PanelWithout non ha l'API");
static_assert(!PartialRefresh::SupportsDifferential<PanelNegated>::value, "hasDifferentialRefresh = false vale come assente");
static_assert(PartialRefresh::SupportsSkip<PanelSkip>::value, "PanelSkip ha l'API del salto");
static_assert(PartialRefresh::SupportsSkip<PanelSkipAndDiff>::value, "il salto è indipendente dal differenziale");
static_assert(!PartialRefresh::SupportsSkip<PanelWithout>::value, "PanelWithout non ha l'API del salto");
static_assert(!PartialRefresh::SupportsSkip<PanelWith>::value, "il solo differenziale non basta al salto");
static_assert(!PartialRefresh::SupportsSkip<PanelHalfSkip>::value, "il salto richiede sia setSkipUnchanged() sia frameSkipped()");

int main()
{
  // -----------------------------------------------------------------------
  // Macchina a stati
  // -----------------------------------------------------------------------
  {
    Diff d;
    d.setEnabled(true);
    ok(!d.arm(), "senza riferimento non si arma: il primo frame è pieno");
    ok(!d.keepsReference(), "senza riferimento il controller può andare in deep sleep");

    d.beginFrame();
    bool allFull = true;
    for (int i = 0; i < PAGES; ++i)
      if (d.onPage(crcOf(100, i), (int16_t)(i * PAGE_H), PAGE_H) != Action::WriteFull) allFull = false;
    ok(allFull, "frame non armato: ogni pagina è WriteFull");
    ok(!d.refValid(), "durante il frame pieno il riferimento vecchio è già spento");
    d.commitFull(HEIGHT);
    ok(d.refValid() && d.keepsReference(), "dopo il refresh pieno il riferimento è valido");
    ok(d.chain() == 0 && d.newGoesToRam26(), "dopo il pieno il B/N sta in 0x24 e il nuovo va in 0x26");
    ok(d.lutSwapped(), "con il nuovo in 0x26 la LUT va scambiata");
  }

  {
    Diff d;
    d.setEnabled(true);
    fullFrame(d, 100);

    ok(d.arm(), "con un riferimento valido il frame si arma");
    ok(armedFrame(d, 100), "rosso identico: tutte le pagine sono WriteNew");
    ok(d.canFire(HEIGHT), "rosso identico: il partial può partire");
    d.commitPartial();
    d.disarm();
    ok(d.chain() == 1 && !d.newGoesToRam26() && !d.lutSwapped(),
       "dopo il primo partial il B/N sta in 0x26, il nuovo va in 0x24 e la LUT è originale");
    ok(d.refValid(), "un partial non spegne il riferimento");

    ok(d.arm() && armedFrame(d, 100) && d.canFire(HEIGHT), "secondo frame armato con rosso identico");
    d.commitPartial();
    d.disarm();
    ok(d.chain() == 2 && d.newGoesToRam26() && d.lutSwapped(),
       "dopo il secondo partial i ruoli sono tornati come dopo il pieno");
  }

  {
    Diff d;
    d.setEnabled(true);
    fullFrame(d, 100);
    d.arm();
    const bool allNew = armedFrame(d, 100, 3, 5);
    ok(!allNew && d.refused(), "rosso cambiato a pagina 3: il frame è rifiutato");
    ok(!d.canFire(HEIGHT), "frame rifiutato: nessun partial");
    ok(d.refusedMask() == ((1u << 3) | (1u << 5)),
       "la mask riporta tutte le pagine cambiate, anche dopo il primo rifiuto");
    d.disarm();
    ok(d.refusedMask() != 0, "la mask resta leggibile dopo il disarmo, prima del secondo disegno");
    ok(d.refValid(), "un rifiuto per rosso non spegne il riferimento: il pieno lo rimpiazza");
    d.beginFrame();
    ok(d.refusedMask() != 0, "beginFrame() non azzera la mask: il secondo disegno non la perde");
    d.disarm();
    fullFrame(d, 200);
    ok(d.chain() == 0 && d.refValid(), "il frame pieno dopo un rifiuto rimette la catena a zero");
    ok(d.arm() && d.refusedMask() == 0, "armare di nuovo azzera mask e rifiuto");
  }

  {
    // Uscita anticipata di GxEPDFrame::render(): la passata armata si ferma alla
    // prima pagina rifiutata, il frame si disarma e quello pieno riparte da capo.
    Diff d;
    d.setEnabled(true);
    fullFrame(d, 100);
    d.arm();
    d.beginFrame();
    ok(d.onPage(crcOf(100, 0), 0, PAGE_H) == Action::WriteNew, "uscita anticipata: pagina 0 uguale");
    ok(d.onPage(crcOf(100, 1) ^ 0xFFFFu, PAGE_H, PAGE_H) == Action::Skip, "uscita anticipata: pagina 1 rifiutata");
    ok(d.refusedMask() == (1u << 1), "uscita anticipata: la mask ha solo la prima pagina rifiutata");
    ok(!d.canFire(HEIGHT), "uscita anticipata: nessun partial con le pagine restanti mai viste");
    d.disarm();
    d.beginFrame();
    bool allFull = true;
    for (int i = 0; i < PAGES; ++i)
      if (d.onPage(crcOf(300, i), (int16_t)(i * PAGE_H), PAGE_H) != Action::WriteFull) allFull = false;
    ok(allFull, "uscita anticipata: il frame pieno successivo riscrive tutte le pagine");
    d.commitFull(HEIGHT);
    ok(d.refValid() && d.chain() == 0, "uscita anticipata: il pieno ricostruisce il riferimento");
    ok(d.arm() && armedFrame(d, 300), "uscita anticipata: il frame dopo può andare in partial sul nuovo riferimento");
  }

  {
    Diff d;
    d.setEnabled(true);
    fullFrame(d, 100);
    d.arm();
    ok(!armedFrame(d, 100, 0), "rosso cambiato alla pagina 0: rifiutato");
    ok(d.refusedMask() == 1u, "mask con la sola pagina 0");
    d.disarm();
    d.arm();
    ok(!armedFrame(d, 100, PAGES - 1), "rosso cambiato all'ultima pagina: rifiutato");
    ok(d.refusedMask() == (1u << (PAGES - 1)), "mask con la sola ultima pagina");
  }

  {
    Diff d;
    d.setEnabled(true);
    fullFrame(d, 100);
    d.arm();
    d.beginFrame();
    ok(d.onPage(crcOf(100, 0), 0, PAGE_H) == Action::WriteNew, "prima pagina uguale");
    ok(d.onPage(crcOf(100, 2), 2 * PAGE_H, PAGE_H) == Action::Skip,
       "pagina fuori ordine: non è un loop paged, si salta");
    ok(d.refused() && !d.refValid() && d.dirty(), "pagina fuori ordine: riferimento spento e frame rifiutato");
  }

  {
    Diff d;
    d.setEnabled(true);
    fullFrame(d, 100);
    d.arm();
    d.beginFrame();
    for (int i = 0; i < 4; ++i) d.onPage(crcOf(100, i), (int16_t)(i * PAGE_H), PAGE_H);
    d.invalidate();
    ok(d.refused(), "una scrittura estranea a frame armato lo fa rifiutare");
    bool anyNew = false;
    for (int i = 4; i < PAGES; ++i)
      if (d.onPage(crcOf(100, i), (int16_t)(i * PAGE_H), PAGE_H) == Action::WriteNew) anyNew = true;
    ok(!anyNew && !d.canFire(HEIGHT), "con CRC tutti uguali il partial non parte dopo l'invalidazione");
    d.disarm();
    ok(!d.arm(), "dopo l'invalidazione non si arma finchè un pieno non rimette il riferimento");
  }

  {
    Diff d;
    d.setEnabled(true);
    fullFrame(d, 100);
    d.arm();
    d.beginFrame();
    for (int i = 0; i < PAGES - 1; ++i) d.onPage(crcOf(100, i), (int16_t)(i * PAGE_H), PAGE_H);
    ok(!d.canFire(HEIGHT), "meno pagine di quelle del riferimento: il partial non parte");
  }

  {
    Diff d;
    d.setEnabled(true);
    fullFrame(d, 100);
    // Un refresh pieno fuori da un loop paged, per esempio clearScreen(): nessuna pagina vista.
    d.commitFull(HEIGHT);
    ok(!d.refValid(), "refresh pieno senza pagine: non rivalida il riferimento con CRC vecchi");
  }

  {
    Diff d;
    d.setEnabled(true);
    fullFrame(d, 100);
    d.invalidate();
    d.commitFull(HEIGHT);
    ok(!d.refValid(), "dopo una scrittura estranea il refresh pieno non rivalida");
    fullFrame(d, 100);
    ok(d.refValid(), "un frame pieno completo rimette il riferimento");
  }

  {
    Diff d;
    d.setEnabled(true);
    d.beginFrame();
    for (int i = 0; i < PAGES; ++i) d.onPage(crcOf(1, i), (int16_t)(i * PAGE_H), PAGE_H);
    d.commitFull(HEIGHT - PAGE_H);
    ok(!d.refValid(), "pagine che non coprono l'altezza: nessun riferimento");
  }

  {
    Diff d;
    d.setEnabled(true);
    d.beginFrame();
    Action last = Action::WriteFull;
    for (int i = 0; i < Diff::MAX_PAGES + 1; ++i) last = d.onPage(1, (int16_t)(i * 8), 8);
    ok(!d.refValid() && d.dirty(), "più pagine del massimo: nessun riferimento");
    (void)last;
    d.beginFrame();
    d.onPage(1, 0, 0);
    ok(d.dirty(), "altezza di pagina nulla: frame sporco");
  }

  {
    Diff d;
    d.setEnabled(true);
    fullFrame(d, 100);
    d.arm();
    armedFrame(d, 100);
    d.commitPartial();
    d.disarm();
    fullFrame(d, 100);
    ok(d.chain() == 0 && d.newGoesToRam26(), "il pieno dopo un partial riporta catena e ruoli all'inizio");
  }

  {
    Diff d;
    d.setEnabled(true);
    fullFrame(d, 100);
    d.setEnabled(false);
    ok(!d.arm() && !d.keepsReference() && !d.refValid(), "spento non tiene riferimenti e non arma");
    d.setEnabled(true);
    ok(!d.arm(), "riacceso riparte senza riferimento");
  }

  // -----------------------------------------------------------------------
  // Politica
  // -----------------------------------------------------------------------
  using namespace PartialRefresh;
  const Params P = { 6, 18.0f, 2.0f, 601000UL };
  const Inputs base = { true, true, 300000UL, 0, true, true, 22.0f, false };

  ok(decide(P, base) == Reason::Partial, "condizioni normali: partial");
  { Inputs i = base; i.enabled = false; ok(decide(P, i) == Reason::Disabled, "spento: Disabled"); }
  { Inputs i = base; i.hasLastFrame = false; ok(decide(P, i) == Reason::NoLastFrame, "primo frame: NoLastFrame"); }
  { Inputs i = base; i.msSinceLastFrame = 601000UL; ok(decide(P, i) == Reason::Partial, "gap uguale al massimo: ancora partial"); }
  { Inputs i = base; i.msSinceLastFrame = 601001UL; ok(decide(P, i) == Reason::LongGap, "gap oltre il massimo: LongGap"); }
  { Inputs i = base; i.chain = 5; ok(decide(P, i) == Reason::Partial, "catena a 5 su 6: ancora partial"); }
  { Inputs i = base; i.chain = 6; ok(decide(P, i) == Reason::ChainLimit, "catena al tetto: ChainLimit"); }
  { Inputs i = base; i.sensorValid = false; ok(decide(P, i) == Reason::SensorInvalid, "sensore non valido: SensorInvalid"); }
  { Inputs i = base; i.sensorPresent = false; i.sensorValid = false; i.temperatureC = 0.0f;
    ok(decide(P, i) == Reason::Partial, "sensore assente: partial senza controllo della temperatura"); }
  { Inputs i = base; i.sensorPresent = false; i.sensorValid = false; i.temperatureC = std::nanf("");
    ok(decide(P, i) == Reason::Partial, "sensore assente: una temperatura NaN non conta"); }
  { Inputs i = base; i.sensorPresent = false; i.coldLatched = true; i.temperatureC = 5.0f;
    ok(decide(P, i) == Reason::Partial, "sensore assente: niente soglia nè isteresi"); }
  { Inputs i = base; i.sensorPresent = false; i.chain = 6;
    ok(decide(P, i) == Reason::ChainLimit, "sensore assente: il tetto della catena vale comunque"); }
  { Inputs i = base; i.sensorPresent = false; i.msSinceLastFrame = 601001UL;
    ok(decide(P, i) == Reason::LongGap, "sensore assente: il gap massimo vale comunque"); }
  { Inputs i = base; i.temperatureC = 17.9f; ok(decide(P, i) == Reason::TooCold, "sotto soglia: TooCold"); }
  { Inputs i = base; i.temperatureC = 18.0f; ok(decide(P, i) == Reason::Partial, "esattamente sulla soglia: partial"); }
  { Inputs i = base; i.temperatureC = std::nanf(""); ok(decide(P, i) == Reason::TooCold, "temperatura NaN: mai partial"); }
  { Inputs i = base; i.coldLatched = true; i.temperatureC = 19.5f;
    ok(decide(P, i) == Reason::TooCold, "dopo un pieno per freddo serve soglia più isteresi"); }
  { Inputs i = base; i.coldLatched = true; i.temperatureC = 20.0f;
    ok(decide(P, i) == Reason::Partial, "soglia più isteresi raggiunta: partial"); }
  { Inputs i = base; i.enabled = false; i.hasLastFrame = false; i.chain = 9; i.sensorValid = false;
    ok(decide(P, i) == Reason::Disabled, "Disabled ha la precedenza su tutto"); }
  { Inputs i = base; i.msSinceLastFrame = 999999UL; i.chain = 9; i.sensorValid = false;
    ok(decide(P, i) == Reason::LongGap, "il gap ha la precedenza su catena e sensore"); }

  {
    State s;
    s.hasLastFrame = true;
    s.lastFrameMs = 0xFFFFFF00u;
    const Inputs in = makeInputs(s, 0x100u, true, 1, true, true, 22.0f);
    ok(in.msSinceLastFrame == 0x200u, "la distanza dall'ultimo frame regge il rollover di millis()");
    State fresh;
    ok(makeInputs(fresh, 5000u, true, 0, true, true, 22.0f).hasLastFrame == false, "senza frame precedente niente distanza");
    const Inputs absent = makeInputs(s, 0x100u, true, 1, false, false, 0.0f);
    ok(!absent.sensorPresent && !absent.sensorValid, "makeInputs riporta il sensore assente");
  }

  ok(DEFAULT_PARAMS.maxGapMs == 2UL * CYCLE_S * 1000UL + BSEC_MARGINE_POLL_MS,
     "il gap di default è in millisecondi: due cicli più il margine");
  ok(DEFAULT_PARAMS.maxGapMs > CYCLE_S * 1000UL + BSEC_MARGINE_POLL_MS,
     "il gap di default supera un ciclo più il margine del poll");
  ok(DEFAULT_PARAMS.maxGapMs == 601000UL, "con il ciclo da 300 s il gap è 601000 ms");

  {
    State s;
    onFrameDone(s, 1000, Reason::Partial, Reason::Partial, 5);
    onFrameDone(s, 2000, Reason::Partial, Reason::Partial, 5);
    onFrameDone(s, 3000, Reason::Partial, Reason::RedChanged, 5);
    onFrameDone(s, 4000, Reason::ChainLimit, Reason::ChainLimit, 5);
    ok(s.day.partial == 2 && s.day.full == 2 && s.day.redRefused == 1, "contatori: partial, pieni e rifiutati per rosso");
    ok(s.hasLastFrame && s.lastFrameMs == 4000, "l'ultimo frame è registrato con il suo istante");
    onFrameDone(s, 5000, Reason::Partial, Reason::Partial, 6);
    ok(s.day.dayKey == 6 && s.day.partial == 1 && s.day.full == 0, "il cambio di giorno azzera i contatori");
    onFrameDone(s, 6000, Reason::Partial, Reason::Partial, -1);
    ok(s.day.dayKey == 6 && s.day.partial == 2, "senza ora valida i contatori non si azzerano");
    onFrameDone(s, 7000, Reason::TooCold, Reason::TooCold, 6);
    ok(s.coldLatched, "dopo una decisione TooCold l'isteresi si arma");
    onFrameDone(s, 7500, Reason::ChainLimit, Reason::ChainLimit, 6);
    ok(s.coldLatched, "una decisione presa prima della temperatura lascia l'isteresi armata");
    onFrameDone(s, 7600, Reason::SensorInvalid, Reason::SensorInvalid, 6);
    onFrameDone(s, 7700, Reason::LongGap, Reason::LongGap, 6);
    ok(s.coldLatched, "anche SensorInvalid e LongGap non toccano l'isteresi");
    onFrameDone(s, 8000, Reason::Partial, Reason::Partial, 6);
    ok(!s.coldLatched, "una decisione Partial, presa sopra la soglia, la disarma");
  }

  {
    // Motivo finale del frame dall'esito di GxEPDFrame::render().
    GxEPDFrame::Outcome out;
    out.result = GxEPDFrame::FrameResult::Skipped;
    ok(finalReason(Reason::Partial, out) == Reason::Unchanged, "frame saltato: Unchanged");
    out.result = GxEPDFrame::FrameResult::Partial;
    ok(finalReason(Reason::Partial, out) == Reason::Partial, "partial partito: Partial");
    out.result = GxEPDFrame::FrameResult::Refused;
    out.refusedPages = 0x0003;
    ok(finalReason(Reason::Partial, out) == Reason::RedChanged, "rifiutato con pagine segnate: RedChanged");
    out.refusedPages = 0;
    ok(finalReason(Reason::Partial, out) == Reason::NoReference, "rifiutato senza pagine: NoReference");
    out.result = GxEPDFrame::FrameResult::Full;
    ok(finalReason(Reason::Partial, out) == Reason::NoReference, "partial deciso ma non armato: NoReference");
    ok(finalReason(Reason::ChainLimit, out) == Reason::ChainLimit, "pieno per politica: la decisione");
  }

  {
    // Un frame saltato perchè invariato non è nè partial nè pieno, ma è un frame valutato.
    State s;
    onFrameDone(s, 1000, Reason::Partial, Reason::Partial, 5);
    onFrameDone(s, 2000, Reason::Partial, Reason::Unchanged, 5);
    onFrameDone(s, 3000, Reason::ChainLimit, Reason::Unchanged, 5);
    onFrameDone(s, 4000, Reason::Partial, Reason::ChainLimit, 5);
    ok(s.day.partial == 1 && s.day.full == 1 && s.day.skipped == 2 && s.day.redRefused == 0,
       "contatori: i frame invariati stanno a parte, non contano come pieni");
    onFrameDone(s, 5000, Reason::Partial, Reason::Unchanged, 5);
    ok(s.hasLastFrame && s.lastFrameMs == 5000,
       "un frame invariato aggiorna l'ultimo frame: il gap per LongGap misura i frame valutati");
    Inputs in = makeInputs(s, 5000 + (uint32_t)(CYCLE_S * 1000UL), true, 2, true, true, 22.0f);
    ok(decide(DEFAULT_PARAMS, in) == Reason::Partial,
       "dopo un frame invariato, a un ciclo di distanza il partial resta ammesso");
    onFrameDone(s, 6000, Reason::Partial, Reason::Partial, 6);
    ok(s.day.dayKey == 6 && s.day.skipped == 0, "il cambio di giorno azzera anche i frame invariati");
  }

  ok(std::strcmp(reasonText(Reason::RedChanged), "rosso cambiato") == 0, "testo del motivo per il log");
  ok(std::strcmp(reasonText(Reason::Unchanged), "frame invariato") == 0, "testo di Unchanged per il log");

  // -----------------------------------------------------------------------
  // Aggancio al driver
  // -----------------------------------------------------------------------
  {
    PanelWith with;
    PanelWithout without;
    PanelNegated negated;
    setEnabled(with, true);
    ok(with.enabled, "con l'API setEnabled() arriva al driver");
    ok(beginFrame(with) && endFrame(with), "con l'API beginFrame() e endFrame() arrivano al driver");
    with.armResult = false;
    ok(!beginFrame(with), "il driver che non arma lo dice");
    ok(chain(with) == 3 && refusedPages(with) == 0x0A, "catena e mask arrivano dal driver");
    setEnabled(without, true);
    ok(!beginFrame(without) && !endFrame(without) && chain(without) == 0 && refusedPages(without) == 0,
       "un pannello senza l'API resta sul refresh pieno");
    ok(!beginFrame(negated), "hasDifferentialRefresh = false vale come assente");
#if DISPLAY_PARTIAL_REFRESH
    ok(isAvailable<PanelWith>() && !isAvailable<PanelWithout>() && !isAvailable<PanelNegated>(),
       "interruttore acceso: disponibile solo con un driver che lo dichiara");
#else
    ok(!isAvailable<PanelWith>(), "interruttore spento: mai disponibile");
#endif
  }

  {
    // Aggancio del salto: indipendente dal differenziale.
    PanelSkip skip;
    PanelSkipAndDiff both;
    PanelWith diffOnly;
    PanelWithout none;
    PanelHalfSkip half;
    setSkipUnchanged(skip, true);
    ok(skip.skipOn, "con l'API setSkipUnchanged() arriva al driver");
    skip.skipped = true;
    ok(wasSkipped(skip), "wasSkipped() legge l'esito del driver");
    setSkipUnchanged(both, true);
    both.skipped = true;
    ok(both.skipOn && wasSkipped(both), "un driver con differenziale e salto li usa entrambi");
    ok(!wasSkipped(diffOnly) && !wasSkipped(none) && !wasSkipped(half),
       "un driver senza l'API completa non salta mai");
    setSkipUnchanged(diffOnly, true);
    setSkipUnchanged(none, true);
    setSkipUnchanged(half, true);
    ok(true, "setSkipUnchanged() su un driver senza l'API non fa niente");
  }

  if (falliti) { std::printf("\n%d test FALLITI\n", falliti); return 1; }
  std::printf("\ntutti i test superati\n");
  return 0;
}

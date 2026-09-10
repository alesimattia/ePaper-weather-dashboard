/**
 * Verifica della validazione di Timings.h: pavimenti, vincoli incrociati,
 * applicazione transazionale e round-trip su NVS.
 *
 * Include l'header VERO del firmware (-I..), non una copia: gli stub in
 * stub/ coprono solo la piattaforma (Arduino, Preferences).
 */
#include "Timings.h"
#include <cstdio>

uint32_t g_msFinto = 0;
SerialeFinta Serial;

static int falliti = 0;
static void ok(bool c, const char* nome)
{
  std::printf("%-64s %s\n", nome, c ? "ok" : "FALLITO");
  if (!c) ++falliti;
}

int main()
{
  using namespace Timings;
  Timings::begin();
  ok(get().owmMin == 10 && get().displayRefreshMin == 5 && get().cinemaOra == 7,
     "default caricati");

  // Combinazione valida che l'applicazione campo-per-campo respingeva:
  // allargare la fascia e spostare l'ora del cinema vanno insieme.
  Valori a = get();
  a.wifiOraInizio = 2; a.wifiOraFine = 5; a.cinemaOra = 3;
  const char* m = nullptr;
  ok(applica(a, &m), "fascia 2..5 + cinema 3 insieme: combinazione valida accettata");
  ok(get().wifiOraFine == 5 && get().cinemaOra == 3, "  e applicata per intero");

  Valori b = get(); b.cinemaOra = 20;              // fuori dalla fascia 2..5
  m = nullptr;
  ok(!applica(b, &m) && get().cinemaOra == 3, "cinema fuori fascia respinto, stato invariato");
  std::printf("   motivo: %s\n", m ? m : "(nessuno)");

  Valori c = get(); c.wifiOraInizio = 20; c.wifiOraFine = 5;
  ok(!applica(c, nullptr), "inizio fascia oltre la fine respinto");

  Valori d = get(); d.coalesceMin = 99;            // >= di ogni cadenza di fetch
  ok(!applica(d, nullptr), "coalescing >= cadenza respinto");

  Valori e = get(); e.owmMin = 3;                  // sotto il pavimento 10
  ok(!applica(e, nullptr), "meteo sotto il pavimento respinto");

  Valori f = get(); f.displayRefreshMin = 0;
  ok(!applica(f, nullptr), "refresh display a 0 respinto");

  Valori g = get(); g.mailMin = 3;
  ok(applica(g, nullptr) && save(), "mail a 3 min: applicato e salvato");
  Timings::begin();
  ok(get().mailMin == 3, "  riletto da NVS dopo un riavvio");

  // Pavimento alzato da un firmware successivo al valore gia' salvato.
  { Preferences p; p.begin("timings"); p.putUShort("owm_min", 4); p.end(); }
  Timings::begin();
  ok(get().owmMin == 10, "valore sotto il pavimento in NVS: clampato al boot");
  { Preferences p; p.begin("timings");
    ok(p.getUShort("owm_min", 0) == 10, "  e riscritto, cosi' la pagina mostra il valore reale"); }

  Preferences::store.clear();
  Preferences::disponibile = false;
  Timings::begin();
  ok(get().owmMin == 10 && get().mailMin == 10, "NVS non disponibile: default, nessun blocco");
  Preferences::disponibile = true;

  Valori h = get(); h.owmMin = 600;
  ok(applica(h, nullptr) && save(), "override a 600 salvato");
  Timings::begin();
  ok(get().owmMin == 600, "  riletto dopo riavvio");

  // Chiavi presenti ma schema assente: NVS scritta da fuori, o da un firmware
  // precedente a Timings. Non si interpretano valori di significato ignoto.
  Preferences::store.clear();
  Preferences::store["owm_min"] = 600;
  Timings::begin();
  ok(get().owmMin == 10, "schema assente: default, non i valori orfani");

  ripristinaDefault();
  ok(get().owmMin == 10, "ripristinaDefault() riporta ai valori compilati");

  std::printf("\n%s\n", falliti ? "CI SONO FALLIMENTI" : "tutti i controlli superati");
  return falliti != 0;
}

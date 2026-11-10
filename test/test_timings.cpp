/**
 * Verifica della validazione di Timings.h: pavimenti, vincoli incrociati,
 * applicazione transazionale e round-trip su NVS.
 *
 * Include l'header VERO del firmware (-I..), non una copia: gli stub in
 * stub/ coprono solo la piattaforma (Arduino, Preferences).
 */
#include "Timings.h"
#include <cstdio>
#include <cstring>

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
  ok(get().owmMin == 10 && get().cinemaOra == 7 && get().tuyaMin == 5,
     "default caricati");
  ok(CYCLE_MIN == 5 && CYCLE_S == BSEC_PERIODO_ULP_S, "ciclo di 5 minuti, pari al periodo ULP");

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

  Valori e = get(); e.owmMin = 3;                  // sotto il pavimento 10
  ok(!applica(e, nullptr), "meteo sotto il pavimento respinto");

  // Il pavimento della telemetria Tuya e' il periodo di campionamento BSEC:
  // sotto, si ripubblica lo stesso campione.
  Valori i = get(); i.tuyaMin = 2;
  ok(!applica(i, nullptr), "telemetria Tuya sotto il pavimento respinta");

  // Il percorso gira a ogni ciclo della fascia percorso: default e pavimento
  // coincidono con il ciclo.
  ok(get().routeMin == 5, "cadenza del percorso al default, un ciclo");
  Valori k = get(); k.routeMin = 4;
  ok(!applica(k, nullptr), "percorso sotto il pavimento respinto");

  // Le cadenze sono multipli del ciclo: lo scheduler le conta in cicli.
  Valori n = get(); n.mailMin = 12;
  m = nullptr;
  ok(!applica(n, &m) && get().mailMin == 10, "mail a 12 min, non multiplo del ciclo: respinto, stato invariato");
  std::printf("   motivo: %s\n", m ? m : "(nessuno)");
  ok(m != nullptr && std::strstr(m, "mail_min") && std::strstr(m, "multiplo di 5"),
     "  il motivo nomina la chiave e il passo");
  Valori o = get(); o.owmMin = 15; o.wifiOraFine = 22;   // ore a passo 1
  ok(applica(o, nullptr), "meteo a 15 min e fine fascia alle 22: accettati");
  ok(campi()[0].step == CYCLE_MIN, "passo delle cadenze nel descrittore = CYCLE_MIN");

  Valori g = get(); g.mailMin = 20;
  ok(applica(g, nullptr) && save(), "mail a 20 min: applicato e salvato");
  Timings::begin();
  ok(get().mailMin == 20, "  riletto da NVS dopo un riavvio");

  // Valore fuori passo in NVS, per esempio scritto da un firmware con un
  // ciclo diverso: al boot sale al multiplo successivo e si riscrive.
  { Preferences p; p.begin("timings"); p.putUShort("mail_min", 7); p.end(); }
  Timings::begin();
  ok(get().mailMin == 10, "mail a 7 in NVS: al boot sale al multiplo successivo, 10");
  { Preferences p; p.begin("timings");
    ok(p.getUShort("mail_min", 0) == 10, "  e riscritto"); }

  // Schema di un firmware precedente (1): le chiavi cambiano significato, il
  // namespace si azzera e si riparte dai default.
  { Preferences p; p.begin("timings"); p.putUShort("schema", 1); p.putUShort("mail_min", 20); p.end(); }
  Timings::begin();
  ok(get().mailMin == 10 && get().routeMin == 5, "schema 1 in NVS: azzerato, default del ciclo");

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

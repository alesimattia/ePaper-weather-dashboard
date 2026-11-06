/**
 * Verifica di Routefinder.h: parser delle coordinate, URL, validazione e
 * persistenza delle impostazioni, fascia ai minuti con il cambio dell'ora
 * legale, sorgente del riquadro e freschezza del percorso.
 *
 * Include l'header VERO del firmware. Orologio e fuso sono quelli finti di
 * test_scheduler.cpp: time(), localtime_r() e mktime() passano per macro dallo
 * stub POSIX di stub/fuso_posix.h, così il risultato non dipende dall'host.
 */
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fuso_posix.h"

#define TZ_TEST "CET-1CEST,M3.5.0,M10.5.0/3"

#include "Timings.h"

static time_t g_epochFinta = 0;
static inline time_t tempoFinto(time_t* p) { if (p) *p = g_epochFinta; return g_epochFinta; }
#define time(p) tempoFinto(p)
#define localtime_r(a, b) fusoLocaltime(a, b)
#define mktime(t) fusoMktime(t)

/** Al posto di Env.h, che è gitignored e contiene le credenziali: arrivo al
 *  placeholder 0,0 come in Env_template.h. */
#define ROUTEFINDER_HOST_TEST
#define LAT 43.6158f
#define LON 13.5189f
#define ROUTEFINDER_TOKEN "paste_token_here"
#include "Routefinder.h"

uint32_t g_msFinto = 0;
SerialeFinta Serial;

static int falliti = 0;
static void ok(bool c, const char* nome)
{
  std::printf("%-64s %s\n", nome, c ? "ok" : "FALLITO");
  if (!c) ++falliti;
}

static void impostaOra(int anno, int mese, int giorno, int h, int m)
{
  struct tm t {};
  t.tm_year = anno - 1900; t.tm_mon = mese - 1; t.tm_mday = giorno;
  t.tm_hour = h; t.tm_min = m; t.tm_sec = 0; t.tm_isdst = -1;
  g_epochFinta = mktime(&t);
}

static void verificaParser()
{
  using namespace Routefinder;
  int32_t lat = 1, lng = 1;
  ok(parseCoordinates("43.615800, 13.518900", lat, lng) && lat == 43615800 && lng == 13518900,
     "coppia copiata da Google Maps");
  ok(parseCoordinates("  -33.9,151.2  ", lat, lng) && lat == -33900000 && lng == 151200000,
     "segno negativo e spazi esterni");
  ok(parseCoordinates("45.123456789,9.1", lat, lng) && lat == 45123456,
     "decimali oltre il sesto scartati");
  ok(parseCoordinates("+45,-0.5", lat, lng) && lat == 45000000 && lng == -500000, "interi e segno +");
  lat = 7; lng = 7;
  ok(!parseCoordinates("95.0, 10", lat, lng) && lat == 7, "latitudine fuori range, uscite intatte");
  ok(!parseCoordinates("45, 181", lat, lng), "longitudine fuori range");
  ok(!parseCoordinates("45 10", lat, lng), "virgola mancante");
  ok(!parseCoordinates("45.,10", lat, lng), "punto senza decimali");
  ok(!parseCoordinates("4e1,10", lat, lng), "esponente rifiutato");
  ok(!parseCoordinates("45,10,3", lat, lng), "terzo valore rifiutato");
  ok(!parseCoordinates("", lat, lng) && !parseCoordinates(nullptr, lat, lng), "vuoto e nullptr");

  char buf[32];
  formatDegrees(buf, sizeof buf, -500000);
  ok(strcmp(buf, "-0.500000") == 0, "formattazione di -0.5 col segno");
}

static void verificaImpostazioni()
{
  using namespace Routefinder;
  Preferences::store.clear(); Preferences::storeInt.clear(); Preferences::storeUChar.clear();
  Timings::begin();
  begin();
  ok(get().originLatE6 == 43615800 && get().originLngE6 == 13518900,
     "partenza di default dalla posizione del meteo");
  ok(!hasDestination(), "arrivo al placeholder 0,0: non configurato");
  ok(get().windowStartMin == 420 && get().windowEndMin == 540 && get().source == Source::Auto,
     "fascia 07:00-09:00 e sorgente automatica di default");

  Settings s = get();
  s.destinationLatE6 = 43500000; s.destinationLngE6 = -13300000;
  s.fuel = Fuel::Diesel;
  const char* motivo = nullptr;
  ok(apply(s, &motivo) && save(), "arrivo e carburante applicati e salvati");
  begin();
  ok(hasDestination() && get().fuel == Fuel::Diesel && get().destinationLngE6 == -13300000,
     "  riletti da NVS dopo un riavvio");

  char url[200];
  const int n = buildUrl(url, sizeof url, "https://x", 610, 290, "bwr");
  ok(n > 0 && n < (int)sizeof url &&
         strcmp(url, "https://x/route?origin=43.615800,13.518900&destination=43.500000,-13.300000"
                     "&fuel=diesel&width=610&height=290&format=arduino&colors=bwr&dither=floyd") == 0,
     "URL con coordinata negativa");

  Settings f = get(); f.windowStartMin = 540; f.windowEndMin = 540;
  ok(!apply(f, nullptr) && get().windowStartMin == 420, "fascia vuota respinta, stato invariato");
  Settings g = get(); g.windowStartMin = 6 * 60;
  ok(!apply(g, &motivo), "fascia che inizia prima della fascia WiFi respinta");
  Settings h = get(); h.windowEndMin = 24 * 60;
  ok(apply(h, nullptr), "fine fascia a mezzanotte dentro la fascia WiFi 7..23 accettata");
  ok(windowOutsideRadioHours(420, 540, Timings::get()) == nullptr, "default dentro la fascia WiFi");

  // Fascia WiFi ristretta dopo il salvataggio: al boot si torna ai default.
  Settings k = get(); k.windowStartMin = 22 * 60; k.windowEndMin = 23 * 60 + 30;
  ok(apply(k, nullptr) && save(), "fascia 22:00-23:30 salvata");
  Timings::Valori w = Timings::get(); w.wifiOraFine = 22;
  ok(Timings::applica(w, nullptr), "fascia WiFi ristretta a 7..22");
  begin();
  ok(get().windowStartMin == 420 && get().windowEndMin == 540, "  fascia percorso tornata ai default al boot");
  Timings::ripristinaDefault();
}

static void verificaFascia()
{
  using namespace Routefinder;
  Settings s = get(); s.source = Source::Auto; s.windowStartMin = 7 * 60 + 30; s.windowEndMin = 9 * 60;
  apply(s, nullptr);

  g_epochFinta = 1000;   // orologio non sincronizzato
  ok(!isInsideRouteHours() && !isRouteSourceActive(), "senza orologio fuori fascia");
  ok(msUntilNextBoundary() == UINT32_MAX, "  e nessun bordo calcolabile");

  impostaOra(2026, 11, 6, 7, 29);
  ok(!isInsideRouteHours(), "07:29 fuori dalla fascia 07:30-09:00");
  ok(msUntilNextBoundary() == 60000, "  bordo fra un minuto");
  impostaOra(2026, 11, 6, 7, 30);
  ok(isInsideRouteHours() && isRouteSourceActive(), "07:30 dentro");
  ok(msUntilNextBoundary() == 90UL * 60000UL, "  prossimo bordo alle 09:00");
  impostaOra(2026, 11, 6, 9, 0);
  ok(!isInsideRouteHours(), "09:00 fuori: fine esclusa");
  ok(msUntilNextBoundary() == (22UL * 60 + 30) * 60000UL, "  prossimo bordo domani 07:30");

  // 29/03/2026 alle 02:00 si passa alle 03:00: la notte dura 23 ore.
  impostaOra(2026, 3, 28, 9, 0);
  ok(msUntilNextBoundary() == (21UL * 60 + 30) * 60000UL, "bordo attraverso l'ora legale");

  s.source = Source::Cinema; apply(s, nullptr);
  impostaOra(2026, 11, 6, 8, 0);
  ok(!isRouteSourceActive(), "sorgente Cinema: mai percorso, nemmeno in fascia");
  ok(shownImage(true, (uint32_t)g_epochFinta) == ShownImage::Cinema, "  e si mostra il cinema");

  s.source = Source::Wallpaper; apply(s, nullptr);
  ok(shownImage(true, (uint32_t)g_epochFinta) == ShownImage::Wallpaper, "sorgente Wallpaper");

  s.source = Source::Route; apply(s, nullptr);
  impostaOra(2026, 11, 6, 15, 0);
  ok(isRouteSourceActive(), "sorgente Percorso: attiva anche fuori fascia");
  const uint32_t scaricato = (uint32_t)g_epochFinta - 50 * 60;
  ok(shownImage(true, scaricato) == ShownImage::Route, "  percorso di 50 min mostrato");
  ok(msUntilRouteStale(scaricato) == 601000UL, "  scade fra 10 min");
  ok(shownImage(true, (uint32_t)g_epochFinta - 61 * 60) == ShownImage::Cinema, "  oltre 1 h torna il cinema");
  ok(shownImage(false, scaricato) == ShownImage::Cinema, "  senza immagine il cinema");

  s.source = Source::Auto; apply(s, nullptr);
  impostaOra(2026, 11, 6, 8, 0);
  ok(shownImage(true, (uint32_t)g_epochFinta - 29 * 60) == ShownImage::Route, "Auto: 29 min, entro 3 cadenze");
  ok(shownImage(true, (uint32_t)g_epochFinta - 31 * 60) == ShownImage::Cinema, "Auto: 31 min, scaduto");
  impostaOra(2026, 11, 6, 10, 0);
  ok(shownImage(true, (uint32_t)g_epochFinta) == ShownImage::Cinema, "Auto fuori fascia: cinema");
}

int main()
{
  ok(fusoImposta(TZ_TEST), "stringa POSIX del fuso interpretata");
  verificaParser();
  verificaImpostazioni();
  verificaFascia();
  std::printf("\n%s\n", falliti ? "CI SONO FALLIMENTI" : "tutti i controlli superati");
  return falliti != 0;
}

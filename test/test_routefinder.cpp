/**
 * Verifica di Routefinder.h: parser delle coordinate, URL, validazione e
 * persistenza delle impostazioni, fasce di percorso e cinema, sorgente
 * programmata e forzata del riquadro, validità di percorso e cinema.
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
  ok(get().windowStartMin == 420 && get().windowEndMin == 540 &&
         get().cinemaStartMin == 18 * 60 && get().cinemaEndMin == 21 * 60 && activeSource() == Source::Auto,
     "fasce 07:00-09:00 e 18:00-21:00, sorgente automatica di default");

  Settings s = get();
  s.destinationLatE6 = 43500000; s.destinationLngE6 = -13300000;
  s.fuel = Fuel::Diesel;
  s.cinemaStartMin = 19 * 60; s.cinemaEndMin = 22 * 60;
  const char* motivo = nullptr;
  ok(apply(s, &motivo) && save(), "arrivo, carburante e fascia cinema applicati e salvati");
  begin();
  ok(hasDestination() && get().fuel == Fuel::Diesel && get().destinationLngE6 == -13300000 &&
         get().cinemaStartMin == 19 * 60 && get().cinemaEndMin == 22 * 60,
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
  ok(!apply(h, &motivo), "fascia percorso fino a mezzanotte sovrapposta al cinema respinta");
  std::printf("   motivo: %s\n", motivo ? motivo : "(nessuno)");
  Settings c1 = get(); c1.cinemaStartMin = 8 * 60;
  ok(!apply(c1, nullptr), "fascia cinema dentro quella del percorso respinta");
  Settings c2 = get(); c2.cinemaStartMin = 22 * 60; c2.cinemaEndMin = 21 * 60;
  ok(!apply(c2, nullptr), "fascia cinema rovesciata respinta");
  Settings c3 = get(); c3.cinemaStartMin = 10 * 60; c3.cinemaEndMin = 24 * 60;
  ok(apply(c3, nullptr), "fascia cinema fino a mezzanotte dentro la fascia WiFi 7..23 accettata");
  ok(windowsOutsideRadioHours(get(), Timings::get()) == nullptr, "fasce dentro la fascia WiFi");

  // Fascia WiFi ristretta dopo il salvataggio: al boot si torna ai default.
  ok(save(), "fascia cinema 10:00-24:00 salvata");
  Timings::Valori w = Timings::get(); w.wifiOraFine = 22;
  ok(windowsOutsideRadioHours(get(), w) != nullptr, "  una fascia WiFi che non la contiene si rifiuta in /config");
  ok(Timings::applica(w, nullptr), "fascia WiFi ristretta a 7..22");
  begin();
  ok(get().cinemaStartMin == 18 * 60 && get().cinemaEndMin == 21 * 60, "  fasce tornate ai default al boot");
  ok(hasDestination() && get().destinationLngE6 == -13300000 && get().fuel == Fuel::Diesel,
     "  la destinazione e il carburante salvati si conservano");

  // Il contrario: coordinate non valide in NVS tornano ai default da sole e le
  // fasce salvate restano. Il ripristino dei tempi svuota anche lo stub di NVS.
  Timings::ripristinaDefault();
  Settings v = get();
  v.destinationLatE6 = 43500000; v.destinationLngE6 = -13300000;
  v.cinemaStartMin = 19 * 60; v.cinemaEndMin = 22 * 60;
  ok(apply(v, nullptr) && save(), "arrivo e fascia cinema 19:00-22:00 salvati");
  Preferences::storeInt["orig_lat"] = 95000000;   // latitudine fuori range
  begin();
  ok(get().originLatE6 == DEFAULTS.originLatE6 && !hasDestination() && get().fuel == DEFAULTS.fuel,
     "partenza fuori range: coordinate e carburante tornano ai default");
  ok(get().cinemaStartMin == 19 * 60 && get().cinemaEndMin == 22 * 60,
     "  le fasce salvate restano");

  // Una scrittura che non va a buon fine si segnala: le impostazioni restano
  // attive in RAM ma il chiamante lo sa.
  ok(save(), "save() con la NVS sana ritorna true");
  Preferences::scritturaFallisce = true;
  ok(!save(), "save() con la NVS che rifiuta le scritture ritorna false");
  Preferences::scritturaFallisce = false;
  Timings::ripristinaDefault();
}

static void verificaRiquadro()
{
  using namespace Routefinder;
  Settings s = get(); s.windowStartMin = 7 * 60 + 30; s.windowEndMin = 9 * 60;
  ok(apply(s, nullptr), "fascia percorso 07:30-09:00, cinema 18:00-21:00");
  setOverride(Source::Auto);
  g_msFinto = 1000000;

  g_epochFinta = 1000;   // orologio non sincronizzato
  ok(scheduledImage() == ShownImage::Wallpaper, "senza orologio: wallpaper");

  impostaOra(2026, 11, 6, 7, 29);
  ok(scheduledImage() == ShownImage::Wallpaper, "07:29: wallpaper");
  impostaOra(2026, 11, 6, 7, 30);
  ok(scheduledImage() == ShownImage::Route, "07:30: percorso");
  impostaOra(2026, 11, 6, 9, 0);
  ok(scheduledImage() == ShownImage::Wallpaper, "09:00: wallpaper, fine esclusa");
  impostaOra(2026, 11, 6, 18, 0);
  ok(scheduledImage() == ShownImage::Cinema, "18:00: cinema");
  impostaOra(2026, 11, 6, 21, 0);
  ok(scheduledImage() == ShownImage::Wallpaper, "21:00: wallpaper");

  // Percorso nella sua fascia: valido entro 3 cadenze, altrimenti wallpaper.
  impostaOra(2026, 11, 6, 8, 0);
  const uint32_t ora = (uint32_t)g_epochFinta;
  ok(shownImage(ShownImage::Route, ora - 14 * 60, 0) == ShownImage::Route, "08:00: percorso di 14 min mostrato");
  ok(shownImage(ShownImage::Route, ora - 16 * 60, 0) == ShownImage::Wallpaper, "  di 16 min: wallpaper");
  ok(shownImage(ShownImage::Cinema, ora, 0) == ShownImage::Wallpaper, "  buffer con il cinema: wallpaper");
  ok(shownImage(ShownImage::Wallpaper, ora, 0) == ShownImage::Wallpaper, "  buffer vuoto: wallpaper");

  // Cinema nella sua fascia: valido solo se scaricato oggi dopo le 18:00.
  impostaOra(2026, 11, 5, 20, 0);
  const uint32_t ieriSera = (uint32_t)g_epochFinta;
  impostaOra(2026, 11, 6, 18, 5);
  const uint32_t alle1805 = (uint32_t)g_epochFinta;
  impostaOra(2026, 11, 6, 19, 0);
  ok(shownImage(ShownImage::Cinema, alle1805, 0) == ShownImage::Cinema, "19:00: cinema delle 18:05 mostrato");
  ok(shownImage(ShownImage::Cinema, ieriSera, 0) == ShownImage::Wallpaper, "  cinema di ieri sera: wallpaper");
  ok(shownImage(ShownImage::Route, alle1805, 0) == ShownImage::Wallpaper, "  buffer con il percorso: wallpaper");

  // Forzatura del cinema alle 12:00: vale solo un'immagine scaricata dopo l'inizio.
  impostaOra(2026, 11, 6, 12, 0);
  setOverride(Source::Cinema);
  ok(activeSource() == Source::Cinema && scheduledImage() == ShownImage::Cinema &&
         overrideRemainingMs() == SOURCE_OVERRIDE_MS,
     "forzatura a Cinema alle 12:00: un'ora intera");
  ok(shownImage(ShownImage::Cinema, ieriSera, g_msFinto - 1000) == ShownImage::Wallpaper,
     "  il cinema di ieri sera non vale");
  g_msFinto += 5000;
  ok(shownImage(ShownImage::Cinema, (uint32_t)g_epochFinta, g_msFinto) == ShownImage::Cinema,
     "  quello scaricato dopo l'inizio della forzatura sì");
  g_msFinto += SOURCE_OVERRIDE_MS - 5000 - 1;
  ok(activeSource() == Source::Cinema, "  un millisecondo prima della scadenza ancora forzato");
  g_msFinto += 1;
  ok(activeSource() == Source::Auto && scheduledImage() == ShownImage::Wallpaper && overrideRemainingMs() == 0,
     "  scaduta l'ora torna l'automatico: alle 12 il wallpaper");

  // Forzatura del percorso fuori fascia e del wallpaper in fascia.
  impostaOra(2026, 11, 6, 15, 0);
  setOverride(Source::Route);
  ok(scheduledImage() == ShownImage::Route, "forzatura a Percorso alle 15:00: percorso");
  ok(shownImage(ShownImage::Route, (uint32_t)g_epochFinta - 5 * 60, g_msFinto) == ShownImage::Route,
     "  percorso di 5 min mostrato");
  impostaOra(2026, 11, 6, 8, 0);
  setOverride(Source::Wallpaper);
  ok(scheduledImage() == ShownImage::Wallpaper, "forzatura a Wallpaper alle 08:00: niente percorso");
  setOverride(Source::Auto);
  ok(activeSource() == Source::Auto && scheduledImage() == ShownImage::Route, "Automatica annulla la forzatura");

  // SNTP può riportare indietro l'orologio dopo il download: un percorso con
  // un'età negativa di pochi secondi vale ancora, un'incoerenza più grande no.
  const uint32_t adesso = (uint32_t)g_epochFinta;
  ok(isRouteFresh(adesso + 5), "percorso scaricato 5 s nel futuro (SNTP all'indietro): ancora valido");
  ok(isRouteFresh(adesso + 60) && !isRouteFresh(adesso + 61), "  la tolleranza è di 60 s");
  ok(shownImage(ShownImage::Route, adesso + 5, 0) == ShownImage::Route, "  e il riquadro lo mostra");

  // Oltre 24,8 giorni di uptime la differenza signed fra due istanti di millis()
  // cambia segno: un cinema scaricato all'avvio non deve risultare successivo
  // all'inizio di una forzatura partita 24,8 giorni dopo.
  impostaOra(2026, 11, 6, 12, 0);
  g_msFinto = 100;
  const uint32_t scaricatoAllAvvio = g_msFinto;
  g_msFinto = scaricatoAllAvvio + 0x80000000u + 5000u;
  setOverride(Source::Cinema);
  ok(!isCinemaFresh(ieriSera, scaricatoAllAvvio),
     "forzatura a Cinema dopo 24,8 giorni di uptime: il cinema scaricato all'avvio non vale");
  g_msFinto += 3000;
  ok(isCinemaFresh(ieriSera, g_msFinto - 1000), "  quello scaricato dopo l'inizio della forzatura sì");
  setOverride(Source::Auto);
}

int main()
{
  ok(fusoImposta(TZ_TEST), "stringa POSIX del fuso interpretata");
  verificaParser();
  verificaImpostazioni();
  verificaRiquadro();
  std::printf("\n%s\n", falliti ? "CI SONO FALLIMENTI" : "tutti i controlli superati");
  return falliti != 0;
}

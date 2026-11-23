#ifndef ROUTEFINDER_H
#define ROUTEFINDER_H

#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

// I test su host dichiarano da sè le coordinate: con le virgolette l'include
// troverebbe sempre l'Env.h vero accanto a questo file, credenziali comprese.
#ifndef ROUTEFINDER_HOST_TEST
  #include "Env.h"
#endif
#include "Timings.h"
#include "Clock.h"
#include "Log.h"

/**
 * Riquadro in alto a sinistra: impostazioni del percorso servito da
 * routefinder (submodule omonimo), fasce orarie di percorso e cinema con
 * persistenza in NVS, e sorgente forzata dalla manutenzione, che resta in RAM e
 * scade dopo SOURCE_OVERRIDE_MS. In automatico il riquadro mostra il percorso
 * nella sua fascia, il cinema nella sua e il wallpaper altrove. Logica pura,
 * senza rete nè display, così si verifica su host (test/test_routefinder.cpp).
 * Usato dal .ino (task route e cinema, drawTestBackground) e da Maintenance.h
 * (pagina /route e controllo della fascia WiFi in /config).
 * @Mattia Alesi
 */

/** Partenza e arrivo di default, da Env.h. La partenza ricade sulla posizione
 *  di casa del meteo; un arrivo a 0,0 vale come non configurato. */
#ifndef ROUTEFINDER_ORIGIN_LAT
  #define ROUTEFINDER_ORIGIN_LAT LAT
#endif
#ifndef ROUTEFINDER_ORIGIN_LON
  #define ROUTEFINDER_ORIGIN_LON LON
#endif
#ifndef ROUTEFINDER_DESTINATION_LAT
  #define ROUTEFINDER_DESTINATION_LAT 0.0f
#endif
#ifndef ROUTEFINDER_DESTINATION_LON
  #define ROUTEFINDER_DESTINATION_LON 0.0f
#endif
/** Token condiviso con il servizio; vuoto o al placeholder sospende il task. */
#ifndef ROUTEFINDER_TOKEN
  #define ROUTEFINDER_TOKEN ""
#endif

/** Fascia del percorso di default, in minuti dal mezzanotte, fine esclusa. */
#ifndef ROUTE_WINDOW_START_MIN
  #define ROUTE_WINDOW_START_MIN (7 * 60)
#endif
#ifndef ROUTE_WINDOW_END_MIN
  #define ROUTE_WINDOW_END_MIN (9 * 60)
#endif

/** Fascia del cinema di default, con la stessa convenzione. */
#ifndef CINEMA_WINDOW_START_MIN
  #define CINEMA_WINDOW_START_MIN (18 * 60)
#endif
#ifndef CINEMA_WINDOW_END_MIN
  #define CINEMA_WINDOW_END_MIN (21 * 60)
#endif

static_assert(ROUTE_WINDOW_START_MIN < ROUTE_WINDOW_END_MIN && ROUTE_WINDOW_END_MIN <= 24 * 60,
              "fascia del percorso non valida");
static_assert(ROUTE_WINDOW_START_MIN >= WIFI_ACTIVE_HOUR_START * 60 &&
                  ROUTE_WINDOW_END_MIN <= (WIFI_ACTIVE_HOUR_END + 1) * 60,
              "la fascia del percorso deve stare dentro la fascia WiFi");
static_assert(CINEMA_WINDOW_START_MIN < CINEMA_WINDOW_END_MIN && CINEMA_WINDOW_END_MIN <= 24 * 60,
              "fascia del cinema non valida");
static_assert(CINEMA_WINDOW_START_MIN >= WIFI_ACTIVE_HOUR_START * 60 &&
                  CINEMA_WINDOW_END_MIN <= (WIFI_ACTIVE_HOUR_END + 1) * 60,
              "la fascia del cinema deve stare dentro la fascia WiFi");
static_assert(CINEMA_WINDOW_END_MIN <= ROUTE_WINDOW_START_MIN || ROUTE_WINDOW_END_MIN <= CINEMA_WINDOW_START_MIN,
              "le fasce di percorso e cinema non devono sovrapporsi: condividono il buffer dell'immagine");

namespace Routefinder
{
  /** Sorgente del riquadro in alto a sinistra. Auto segue le fasce; le altre
   *  sono forzature temporanee dalla manutenzione. */
  enum class Source : uint8_t
  {
    Auto      = 0,   // percorso e cinema nelle loro fasce, wallpaper altrove
    Cinema    = 1,
    Route     = 2,
    Wallpaper = 3    // wallpaper della partizione in flash
  };

  /** Carburante passato alla Routes API. I valori sono quelli in NVS. */
  enum class Fuel : uint8_t
  {
    Gasoline = 0,
    Diesel   = 1,
    Hybrid   = 2,
    Electric = 3
  };

  /** Immagine che il riquadro deve disegnare adesso, o contenuto del buffer
   *  in RAM, dove Wallpaper vuol dire buffer vuoto. */
  enum class ShownImage : uint8_t { Route, Cinema, Wallpaper };

  /** Coordinate in microgradi: niente float in NVS nè nella formattazione
   *  dell'URL. Le fasce sono in minuti dalla mezzanotte, fine esclusa. */
  struct Settings
  {
    int32_t  originLatE6;
    int32_t  originLngE6;
    int32_t  destinationLatE6;
    int32_t  destinationLngE6;
    Fuel     fuel;
    uint16_t windowStartMin;
    uint16_t windowEndMin;
    uint16_t cinemaStartMin;
    uint16_t cinemaEndMin;
  };

  /**
   * Microgradi dai gradi di Env.h, usabile a compile-time. Si arrotonda al
   * quinto decimale (circa 1 m) perchè LAT e LON sono letterali float, che a 7
   * cifre significative non portano il sesto: 43.6158f vale 43.61579895.
   */
  inline constexpr int32_t toE6(double degrees)
  {
    return (int32_t)(degrees >= 0 ? degrees * 1e5 + 0.5 : degrees * 1e5 - 0.5) * 10;
  }

  inline constexpr Settings DEFAULTS = {
      toE6(ROUTEFINDER_ORIGIN_LAT),      toE6(ROUTEFINDER_ORIGIN_LON),
      toE6(ROUTEFINDER_DESTINATION_LAT), toE6(ROUTEFINDER_DESTINATION_LON),
      Fuel::Gasoline,
      (uint16_t)ROUTE_WINDOW_START_MIN, (uint16_t)ROUTE_WINDOW_END_MIN,
      (uint16_t)CINEMA_WINDOW_START_MIN, (uint16_t)CINEMA_WINDOW_END_MIN,
  };

  namespace detail
  {
    inline constexpr const char* NVS_NAMESPACE = "routefinder";
    inline constexpr const char* KEY_SCHEMA    = "schema";
    inline constexpr uint16_t    SCHEMA        = 1;

    inline Settings settings = DEFAULTS;

    // Forzatura in corso: solo in RAM, quindi un riavvio torna all'automatico.
    inline Source   overrideSource  = Source::Auto;
    inline uint32_t overrideStartMs = 0;

    /** Valore di `fuel` nella querystring del servizio, per enum. */
    inline constexpr const char* FUEL_PARAMS[] = {"gasoline", "diesel", "hybrid", "electric"};
    /** Etichette italiane della pagina /route, nello stesso ordine. */
    inline constexpr const char* FUEL_LABELS[] = {"Benzina", "Diesel", "Ibrido", "Elettrico"};
    inline constexpr const char* SOURCE_LABELS[] = {"Automatica", "Cinema", "Percorso", "Wallpaper"};

    inline bool latitudeInRange(int32_t e6)  { return e6 >= -90000000 && e6 <= 90000000; }
    inline bool longitudeInRange(int32_t e6) { return e6 >= -180000000 && e6 <= 180000000; }
  }

  inline constexpr uint8_t FUEL_COUNT   = 4;
  inline constexpr uint8_t SOURCE_COUNT = 4;

  inline const Settings& get() { return detail::settings; }

  inline const char* fuelParam(Fuel f)   { return detail::FUEL_PARAMS[(uint8_t)f < FUEL_COUNT ? (uint8_t)f : 0]; }
  inline const char* fuelLabel(Fuel f)   { return detail::FUEL_LABELS[(uint8_t)f < FUEL_COUNT ? (uint8_t)f : 0]; }
  inline const char* sourceLabel(Source s) { return detail::SOURCE_LABELS[(uint8_t)s < SOURCE_COUNT ? (uint8_t)s : 0]; }

  /**
   * Motivo per cui una delle due fasce non sta dentro la fascia WiFi, o
   * nullptr. Percorso e cinema hanno bisogno della radio: fuori dalla fascia
   * WiFi i loro task non partirebbero mai. Lo usa anche /config, perchè il
   * vincolo lega i due namespace.
   */
  inline const char* windowsOutsideRadioHours(const Settings& s, const Timings::Valori& t)
  {
    const uint32_t first = (uint32_t)t.wifiOraInizio * 60U;
    const uint32_t last = ((uint32_t)t.wifiOraFine + 1U) * 60U;
    if (s.windowStartMin < first || s.windowEndMin > last) return "la fascia del percorso esce dalla fascia WiFi";
    if (s.cinemaStartMin < first || s.cinemaEndMin > last) return "la fascia del cinema esce dalla fascia WiFi";
    return nullptr;
  }

  /** Motivo del rifiuto di `s`, o nullptr se è una configurazione valida. Le
   *  due fasce non si sovrappongono perchè le immagini condividono il buffer. */
  inline const char* invalidReason(const Settings& s, const Timings::Valori& t)
  {
    if (!detail::latitudeInRange(s.originLatE6) || !detail::longitudeInRange(s.originLngE6))
      return "partenza fuori range";
    if (!detail::latitudeInRange(s.destinationLatE6) || !detail::longitudeInRange(s.destinationLngE6))
      return "arrivo fuori range";
    if ((uint8_t)s.fuel >= FUEL_COUNT) return "carburante sconosciuto";
    if (s.windowStartMin >= s.windowEndMin || s.windowEndMin > 24 * 60)
      return "l'inizio della fascia del percorso non precede la fine";
    if (s.cinemaStartMin >= s.cinemaEndMin || s.cinemaEndMin > 24 * 60)
      return "l'inizio della fascia del cinema non precede la fine";
    if (s.windowStartMin < s.cinemaEndMin && s.cinemaStartMin < s.windowEndMin)
      return "le fasce di percorso e cinema si sovrappongono";
    return windowsOutsideRadioHours(s, t);
  }

  /** Scrive in NVS i soli campi cambiati, come Timings::save(). */
  inline bool save()
  {
    using namespace detail;
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false))
    {
      LOG("route", "NVS non apribile in scrittura: impostazioni attive solo fino al riavvio");
      return false;
    }
    prefs.putUShort(KEY_SCHEMA, SCHEMA);
    const Settings& s = settings;
    auto putInt = [&](const char* k, int32_t v) { if (prefs.getInt(k, INT32_MIN) != v) prefs.putInt(k, v); };
    auto putByte = [&](const char* k, uint8_t v) { if (prefs.getUChar(k, 0xFF) != v) prefs.putUChar(k, v); };
    auto putShort = [&](const char* k, uint16_t v) { if (prefs.getUShort(k, 0xFFFF) != v) prefs.putUShort(k, v); };
    putInt("orig_lat", s.originLatE6);
    putInt("orig_lng", s.originLngE6);
    putInt("dest_lat", s.destinationLatE6);
    putInt("dest_lng", s.destinationLngE6);
    putByte("fuel", (uint8_t)s.fuel);
    putShort("win_start", s.windowStartMin);
    putShort("win_end", s.windowEndMin);
    putShort("cine_start", s.cinemaStartMin);
    putShort("cine_end", s.cinemaEndMin);
    prefs.end();
    return true;
  }

  /**
   * Carica le impostazioni da NVS; da chiamare dopo Timings::begin(), perchè
   * la fascia si confronta con quella WiFi. Una chiave assente ricade sul
   * default di Env.h; una combinazione non valida, anche solo perchè la fascia
   * WiFi è stata ristretta da /config/reset, torna interamente ai default.
   */
  inline void begin()
  {
    using namespace detail;
    settings = DEFAULTS;
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true))
    {
      LOG("route", "NVS non disponibile: impostazioni predefinite");
      return;
    }
    if (prefs.getUShort(KEY_SCHEMA, 0) == SCHEMA)
    {
      settings.originLatE6      = prefs.getInt("orig_lat", DEFAULTS.originLatE6);
      settings.originLngE6      = prefs.getInt("orig_lng", DEFAULTS.originLngE6);
      settings.destinationLatE6 = prefs.getInt("dest_lat", DEFAULTS.destinationLatE6);
      settings.destinationLngE6 = prefs.getInt("dest_lng", DEFAULTS.destinationLngE6);
      settings.fuel             = (Fuel)prefs.getUChar("fuel", (uint8_t)DEFAULTS.fuel);
      settings.windowStartMin   = prefs.getUShort("win_start", DEFAULTS.windowStartMin);
      settings.windowEndMin     = prefs.getUShort("win_end", DEFAULTS.windowEndMin);
      settings.cinemaStartMin   = prefs.getUShort("cine_start", DEFAULTS.cinemaStartMin);
      settings.cinemaEndMin     = prefs.getUShort("cine_end", DEFAULTS.cinemaEndMin);
    }
    prefs.end();
    const char* reason = invalidReason(settings, Timings::get());
    if (reason)
    {
      LOG("route", "impostazioni non valide (%s): torno ai predefiniti", reason);
      settings = DEFAULTS;
      save();
    }
  }

  /** Applica in blocco senza salvare, come Timings::applica(); falso e stato
   *  invariato se `s` non è valida. */
  inline bool apply(const Settings& s, const char** reason = nullptr)
  {
    const char* err = invalidReason(s, Timings::get());
    if (err)
    {
      if (reason) *reason = err;
      return false;
    }
    detail::settings = s;
    return true;
  }

  /** Vero se l'arrivo è configurato: 0,0 è il placeholder di Env_template.h. */
  inline bool hasDestination()
  {
    return detail::settings.destinationLatE6 != 0 || detail::settings.destinationLngE6 != 0;
  }

  /**
   * Gradi decimali in microgradi: segno opzionale, cifre, punto e fino a sei
   * decimali (gli altri si scartano, perchè sotto i 10 cm non servono). Rifiuta
   * tutto il resto, compresi esponenti, spazi interni e stringhe vuote.
   * @return puntatore al primo carattere non consumato, o nullptr se malformato.
   */
  inline const char* parseDegrees(const char* p, int32_t& out)
  {
    bool negative = false;
    if (*p == '-' || *p == '+') { negative = (*p == '-'); ++p; }
    if (*p < '0' || *p > '9') return nullptr;
    int64_t integer = 0;
    while (*p >= '0' && *p <= '9')
    {
      integer = integer * 10 + (*p - '0');
      if (integer > 180) return nullptr;
      ++p;
    }
    int64_t fraction = 0;
    int digits = 0;
    if (*p == '.')
    {
      ++p;
      if (*p < '0' || *p > '9') return nullptr;
      while (*p >= '0' && *p <= '9')
      {
        if (digits < 6) { fraction = fraction * 10 + (*p - '0'); ++digits; }
        ++p;
      }
    }
    while (digits < 6) { fraction *= 10; ++digits; }
    const int64_t value = integer * 1000000 + fraction;
    out = (int32_t)(negative ? -value : value);
    return p;
  }

  /**
   * Coppia "lat, lng" nel formato che Google Maps copia col tasto destro: spazi
   * ammessi attorno ai numeri e alla virgola, nient'altro.
   * @return falso se malformata o fuori range; le uscite restano intatte.
   */
  inline bool parseCoordinates(const char* text, int32_t& latE6, int32_t& lngE6)
  {
    if (text == nullptr) return false;
    const char* p = text;
    while (*p == ' ') ++p;
    int32_t lat = 0, lng = 0;
    p = parseDegrees(p, lat);
    if (!p) return false;
    while (*p == ' ') ++p;
    if (*p != ',') return false;
    ++p;
    while (*p == ' ') ++p;
    p = parseDegrees(p, lng);
    if (!p) return false;
    while (*p == ' ') ++p;
    if (*p != '\0') return false;
    if (!detail::latitudeInRange(lat) || !detail::longitudeInRange(lng)) return false;
    latE6 = lat;
    lngE6 = lng;
    return true;
  }

  /** Microgradi in "43.615800", con il segno anche per -0.5. */
  inline int formatDegrees(char* buf, size_t size, int32_t e6)
  {
    const uint32_t magnitude = e6 < 0 ? (uint32_t)(-(int64_t)e6) : (uint32_t)e6;
    return snprintf(buf, size, "%s%lu.%06lu", e6 < 0 ? "-" : "",
                    (unsigned long)(magnitude / 1000000UL), (unsigned long)(magnitude % 1000000UL));
  }

  /** Coppia "lat,lng" senza spazi, la forma della querystring e del form. */
  inline int formatCoordinates(char* buf, size_t size, int32_t latE6, int32_t lngE6)
  {
    char lat[16], lng[16];
    formatDegrees(lat, sizeof lat, latE6);
    formatDegrees(lng, sizeof lng, lngE6);
    return snprintf(buf, size, "%s,%s", lat, lng);
  }

  /**
   * URL dell'immagine per il pannello: misura e piani li decide il chiamante
   * dal layout, così non possono divergere dal buffer che li riceve.
   * @return lunghezza come snprintf; un valore >= size indica troncamento.
   */
  inline int buildUrl(char* buf, size_t size, const char* origin, uint16_t width, uint16_t height,
                      const char* colors)
  {
    const Settings& s = detail::settings;
    char from[40], to[40];
    formatCoordinates(from, sizeof from, s.originLatE6, s.originLngE6);
    formatCoordinates(to, sizeof to, s.destinationLatE6, s.destinationLngE6);
    return snprintf(buf, size, "%s/route?origin=%s&destination=%s&fuel=%s&width=%u&height=%u"
                    "&format=arduino&colors=%s&dither=floyd",
                    origin, from, to, fuelParam(s.fuel), (unsigned)width, (unsigned)height, colors);
  }

  /** Minuti dalla mezzanotte locale, o -1 senza orologio valido. */
  inline int32_t localMinuteOfDay()
  {
    if (!Clock::valido()) return -1;
    const time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    return local.tm_hour * 60 + local.tm_min;
  }

  /** Vero se l'ora locale cade nella fascia [startMin, endMin); falso senza
   *  orologio, perchè senza ora non si sa in quale fascia si è. */
  inline bool isInsideWindow(uint16_t startMin, uint16_t endMin)
  {
    const int32_t minute = localMinuteOfDay();
    return minute >= 0 && minute >= startMin && minute < endMin;
  }

  /**
   * Forza la sorgente del riquadro per SOURCE_OVERRIDE_MS da adesso; Auto
   * annulla la forzatura. Usato da Maintenance.h (pagina /route).
   */
  inline void setOverride(Source source)
  {
    detail::overrideSource = (uint8_t)source < SOURCE_COUNT ? source : Source::Auto;
    detail::overrideStartMs = millis();
    if (detail::overrideSource != Source::Auto)
      LOG("route", "riquadro forzato a %s per %lu min", sourceLabel(detail::overrideSource),
          (unsigned long)(SOURCE_OVERRIDE_MS / 60000UL));
  }

  /** Sorgente in vigore: la forzatura finchè non scade, poi Auto. La scadenza
   *  si misura su millis(), che avanza anche in light sleep e non chiede l'ora. */
  inline Source activeSource()
  {
    if (detail::overrideSource == Source::Auto) return Source::Auto;
    if ((uint32_t)(millis() - detail::overrideStartMs) >= SOURCE_OVERRIDE_MS)
    {
      LOG("route", "forzatura a %s scaduta: riquadro automatico", sourceLabel(detail::overrideSource));
      detail::overrideSource = Source::Auto;
      return Source::Auto;
    }
    return detail::overrideSource;
  }

  /** Millisecondi che restano alla forzatura in corso, 0 senza forzatura. */
  inline uint32_t overrideRemainingMs()
  {
    if (activeSource() == Source::Auto) return 0;
    return SOURCE_OVERRIDE_MS - (uint32_t)(millis() - detail::overrideStartMs);
  }

  /**
   * Immagine che il riquadro deve mostrare adesso secondo la sorgente: quella
   * forzata, oppure in automatico il percorso nella sua fascia, il cinema nella
   * sua e il wallpaper altrove o senza orologio. Decide anche quale task di rete
   * è attivo, quindi percorso e cinema non si scaricano mai fuori da qui.
   */
  inline ShownImage scheduledImage()
  {
    const Settings& s = detail::settings;
    switch (activeSource())
    {
      case Source::Route:     return ShownImage::Route;
      case Source::Cinema:    return ShownImage::Cinema;
      case Source::Wallpaper: return ShownImage::Wallpaper;
      case Source::Auto:      break;
    }
    if (isInsideWindow(s.windowStartMin, s.windowEndMin)) return ShownImage::Route;
    if (isInsideWindow(s.cinemaStartMin, s.cinemaEndMin)) return ShownImage::Cinema;
    return ShownImage::Wallpaper;
  }

  /** Età massima, in secondi, con cui un percorso si mostra: un multiplo della cadenza. */
  inline uint32_t maxRouteAgeS()
  {
    return (uint32_t)ROUTE_MAX_AGE_CADENCES * Timings::get().routeMin * 60U;
  }

  /** Vero se un percorso scaricato all'epoch `downloadedAt` è ancora mostrabile. */
  inline bool isRouteFresh(uint32_t downloadedAt)
  {
    if (!Clock::valido() || downloadedAt == 0) return false;
    const int64_t age = (int64_t)time(nullptr) - (int64_t)downloadedAt;
    return age >= 0 && age <= (int64_t)maxRouteAgeS();
  }

  /** Epoch di oggi all'inizio della fascia del cinema, 0 senza orologio.
   *  mktime() con tm_isdst = -1 assorbe il cambio dell'ora legale. */
  inline uint32_t cinemaWindowStartEpoch()
  {
    if (!Clock::valido()) return 0;
    const time_t now = time(nullptr);
    struct tm start;
    localtime_r(&now, &start);
    start.tm_hour = detail::settings.cinemaStartMin / 60;
    start.tm_min = detail::settings.cinemaStartMin % 60;
    start.tm_sec = 0;
    start.tm_isdst = -1;
    const time_t at = mktime(&start);
    return at == (time_t)-1 ? 0 : (uint32_t)at;
  }

  /**
   * Vero se un cinema scaricato a `downloadedAt` (epoch) e `downloadedMs`
   * (millis) vale ancora: scaricato oggi dopo l'inizio della fascia cinema,
   * oppure dopo l'inizio della forzatura in corso. Un'immagine della sera prima
   * non vale più, anche se il buffer la contiene ancora.
   */
  inline bool isCinemaFresh(uint32_t downloadedAt, uint32_t downloadedMs)
  {
    if (!Clock::valido() || downloadedAt == 0) return false;
    if (downloadedAt >= cinemaWindowStartEpoch()) return true;
    return activeSource() == Source::Cinema && (int32_t)(downloadedMs - detail::overrideStartMs) >= 0;
  }

  /**
   * Cosa disegna il riquadro: l'immagine programmata se il buffer in RAM la
   * contiene ed è ancora valida, altrimenti il wallpaper.
   * @param stored contenuto del buffer, Wallpaper se vuoto.
   * @param downloadedAt epoch del download.
   * @param downloadedMs millis() del download.
   */
  inline ShownImage shownImage(ShownImage stored, uint32_t downloadedAt, uint32_t downloadedMs)
  {
    const ShownImage scheduled = scheduledImage();
    if (scheduled == ShownImage::Route && stored == ShownImage::Route && isRouteFresh(downloadedAt))
      return ShownImage::Route;
    if (scheduled == ShownImage::Cinema && stored == ShownImage::Cinema && isCinemaFresh(downloadedAt, downloadedMs))
      return ShownImage::Cinema;
    return ShownImage::Wallpaper;
  }
}

#endif // ROUTEFINDER_H

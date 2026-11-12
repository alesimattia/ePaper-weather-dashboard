#ifndef WEATHER_H
#define WEATHER_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <stdint.h>

#include <GxEPD2_3C.h>
#include "Layout.h"

#include "Env.h"
#include "Timings.h"
#include "Log.h"
#include "Clock.h"
#include "Net.h"
#include "icons.h"
#include "icons_weather.h"
#include "Graphics.h"
#include "Calendar.h"
#include "Indoor.h"
#include "Mail.h"      // Mail::draw() invocato dentro renderFrame()
#include "Tuya.h"      // Tuya::hasFailed() letto da drawBanner() per il badge di guasto
#include "PartialRefresh.h"   // politica del refresh differenziale, usata da renderFrame()

// ---------------------------------------------------------------------------
// Istanza del display (definita nello sketch .ino). Il tipo concreto del
// pannello (Layout::Panel) e' scelto dal dispatcher Layout.h in base al
// #define DISPLAY_VARIANT_* nello sketch.
// ---------------------------------------------------------------------------
extern GxEPD2_3C<Layout::Panel, Layout::PAGE_HEIGHT> display;

// ---------------------------------------------------------------------------
// Helper definito nello sketch .ino: disegna l'immagine di background
// corrispondente all'indice passato. Le variabili immagine hanno linkage
// interno (const + namespace scope) quindi non sono accessibili via extern
// da questo header; il .ino fa da proxy.
// ---------------------------------------------------------------------------
extern void drawTestBackground();

/**
 * Modulo meteo header-only: scheduling, fetch OpenWeatherMap One Call 3.0 e
 * disegno del banner sul display e-paper.
 *
 * Endpoint unico: /data/3.0/onecall. Una singola richiesta restituisce
 * meteo corrente + hourly + daily; slots[0] viene riempito da `current`,
 * slots[1..3] da `hourly[3/6/9]` (step 3h). Il codice condizione
 * `weather[0].id` sceglie icona ed etichetta in icons_weather.h; i campi
 * accessori (sunrise, sunset, pop, rain.1h, snow.1h, daily.feels_like.morn/eve)
 * alimentano la riga precipitazioni dei blocchi, la colonna alba/tramonto e
 * la barra morn/eve.
 *
 * Il WiFi NON è gestito qui: runFetch() presuppone che sia gia' connesso
 * (lo sketch .ino si occupa di wifiOn()/wifiOff() immediatamente intorno
 * alla chiamata). Cosi' la radio resta spenta fra un fetch e l'altro.
 *
 * Credenziali WiFi e API key OWM sono in Env.h. Posizione GPS (LAT/LON)
 * pure in Env.h. Il fuso orario è gestito da Clock::begin()
 * (POSIX TZ in Clock.h): localtime_r() applica CET/CEST in automatico.
 */
namespace Weather
{
  // -------------------------------------------------------------------------
  // Stato, costanti di layout e helper interni. `detail` tiene tutto lo
  // scope interno fuori dalla API pubblica del modulo.
  // -------------------------------------------------------------------------
  namespace detail
  {
    // -----------------------------------------------------------------------
    // Intervalli di fetch (millisecondi). I valori derivano dai #define
    // definiti in Timings.h. One Call 3.0 restituisce corrente + hourly +
    // daily in un unica chiamata, quindi un solo task ne governa la cadenza.
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Layout: tutte le costanti pixel (riquadri, blocchi, baseline, colonna
    // sole) stanno in Layout_common.h e si leggono come Layout::*. Le baseline
    // del banner derivano da BANNER_Y, quindi seguono l'altezza del pannello
    // senza altre modifiche.
    // -----------------------------------------------------------------------
    // Cache dati meteo.
    // -----------------------------------------------------------------------
    /**
     * Slot di dati meteo. Slot[0] = previsione corrente (orario = download),
     * slot[1..3] = 3 previsioni future (orario = epoch dell'API).
     */
    struct WeatherSlot
    {
      uint16_t conditionId;        // weather[0].id, sceglie icona ed etichetta
      bool     night;              // suffisso "n" di weather[0].icon
      float    feelsLikeC;         // feels_like (temperatura percepita, °C)
      time_t   epoch;              // orario dell'osservazione o della previsione (UTC)
      bool     valid;

      // parametri della riga precipitazioni, vedi composeDescLine()
      float    precipitation_prob; // hourly[].pop [0..1], per slots[0] quello dell'ora in corso
      float    rain1h;             // hourly[].rain.1h (mm/h previsti); NaN se assente
      float    snow1h;             // hourly[].snow.1h (mm/h previsti); NaN se assente
    };

    inline WeatherSlot slots[4];

    // colonna alba/tramonto e barra morn/eve, vedi renderSunColumn()
    inline time_t sunriseEpoch       = 0;     // current.sunrise (UTC epoch)
    inline time_t sunsetEpoch        = 0;     // current.sunset  (UTC epoch)
    inline float  dailyFeelsLikeMorn = NAN;   // daily[0].feels_like.morn (°C)
    inline float  dailyFeelsLikeEve  = NAN;   // daily[0].feels_like.eve  (°C)

    /**
     * @widget storico-temperature
     *
     * Storico temperatura percepita esterna per il mini-chart sotto la
     * slider temp-range. Ring buffer in RAM (~3h20m a 10 min/fetch).
     * Non persiste fra reboot: il light sleep mantiene la RAM, ma un
     * reboot completo riparte da buffer vuoto e il chart si riempie
     * gradualmente nei fetch successivi.
     */
    struct TempSample { time_t epoch; float t; };
    inline constexpr uint8_t TC_HIST_CAP = 20;
    inline TempSample tcHistBuf[TC_HIST_CAP];
    inline uint8_t    tcHistCount     = 0;     // numero campioni validi (≤ TC_HIST_CAP)
    inline uint8_t    tcHistHead      = 0;     // indice next-write nel ring
    inline time_t     tcHistLastEpoch = 0;     // anti-doppione fra fetch ravvicinati

    // =======================================================================
    // Helper interni
    // =======================================================================

    /**
     * Formatta un epoch UTC in "HH:MM" locale Europe/Rome (DST gestito
     * automaticamente dal TZ di sistema impostato da Clock::begin()).
     * @param epoch epoch UTC (o 0 per "--:--")
     * @param out   buffer di almeno 6 byte
     */
    inline void formatHHMM(time_t epoch, char* out)
    {
      if (epoch == 0)
      {
        strcpy(out, "--:--");
        return;
      }
      struct tm tm;
      localtime_r(&epoch, &tm);
      snprintf(out, 6, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }

    /**
     * Stampa a Serial un riepilogo leggibile di uno slot meteo. Utile per
     * debug senza togliere il display dal muro.
     */
    inline void logSlot(const char* tag, const WeatherSlot& s)
    {
      char hhmm[6];
      formatHHMM(s.epoch, hhmm);
      LOGV("OWM", "%-8s %s  %.1f C  id=%u %s  pop=%.2f  pioggia=%.1f  neve=%.1f  \"%s\"",
           tag, hhmm, s.feelsLikeC, (unsigned)s.conditionId, s.night ? "n" : "d",
           s.precipitation_prob, s.rain1h, s.snow1h, WeatherIcons::labelFor(s.conditionId));
    }

    /**
     * Esegue una GET HTTPS verso OpenWeatherMap e deserializza la risposta JSON
     * applicando un DeserializationOption::Filter durante il parsing, cosi' che
     * dal JSON ricevuto vengano conservati in memoria solo i campi marcati
     * `true` nel documento filter. Utile con One Call 3.0 (payload ampio:
     * current + hourly*48 + daily*8). Ritorna true solo se HTTP 200 e parsing
     * ok. Certificato non verificato, accettabile per dati meteo pubblici.
     */
    inline bool httpGetJsonFiltered(const String& url, JsonDocument& doc, const JsonDocument& filter)
    {
      WiFiClientSecure client;
      HTTPClient http;
      if (!Net::beginHttps(http, client, url))
      {
        LOG("OWM", "http.begin() failed");
        return false;
      }
      int code = http.GET();
      if (code != 200)
      {
        LOG("OWM", "fetch failed: http=%d", code);
        http.end();
        return false;
      }
      Net::BodyStream body(http.getStream());
      DeserializationError err = deserializeJson(doc, body,
                                                 DeserializationOption::Filter(filter));
      http.end();
      if (err)
      {
        LOG("OWM", "json parse error: %s", err.c_str());
        return false;
      }
      return true;
    }

    /**
     * Copia una stringa JSON in un buffer C a lunghezza fissa, troncando se serve.
     */
    inline void copyStr(char* dst, size_t dstLen, const char* src)
    {
      if (!src)
      {
        dst[0] = '\0';
        return;
      }
      strncpy(dst, src, dstLen - 1);
      dst[dstLen - 1] = '\0';
    }

    // =======================================================================
    // Fetch
    // =======================================================================

    /**
     * Legge da un elemento `weather` il codice condizione e il giorno/notte.
     * La notte viene dal suffisso "n" del campo icon, che OpenWeather calcola
     * su alba e tramonto anche per le ore previste; un elemento mancante
     * lascia il codice a 0, che icons_weather.h tratta come sconosciuto.
     */
    inline void readCondition(JsonVariantConst weather, WeatherSlot& slot)
    {
      slot.conditionId = weather["id"] | 0U;
      const char* icon = weather["icon"] | "";
      slot.night = strlen(icon) >= 3 && icon[2] == 'n';
    }

    /**
     * Scarica meteo corrente + 3 previsioni orarie + dati giornalieri di oggi
     * in una singola chiamata all'endpoint One Call 3.0. Popola slots[0] da
     * `current`, slots[1..3] da hourly[3], hourly[6], hourly[9] (step 3h per
     * mantenere la cadenza della precedente /forecast?cnt=3).
     *
     * URL: exclude=minutely,alerts (scarta cio' che non serve; daily rimane
     * perchè ne leggiamo morn/eve). Il Filter ArduinoJson sotto restringe
     * ulteriormente i campi conservati in memoria.
     *
     * Di ogni slot conserva il codice condizione e il giorno/notte, da cui
     * icons_weather.h ricava icona ed etichetta: il testo descrittivo di
     * OpenWeather non serve e non si scarica, così il JsonDocument non tiene
     * 48 stringhe. Memorizza anche i parametri accessori (sunrise, sunset,
     * pop, rain.1h e snow.1h previsti da hourly, daily.feels_like.morn/eve)
     * mostrati nella riga precipitazioni, nella colonna alba/tramonto e nella
     * barra morn/eve.
     *
     * Ritorna true solo con corrente e previsioni entrambe aggiornate: una
     * risposta parziale e' un fallimento, cosi' runFetch() non resetta i
     * timer e il giro successivo ritenta invece di lasciare in pagina dati
     * di epoche diverse.
     */
    inline bool fetchOneCall()
    {
      // Composto in un buffer locale: la concatenazione di String allocherebbe
      // un temporaneo a ogni pezzo.
      char url[256];
      const int urlLength = snprintf(url, sizeof(url),
          "https://api.openweathermap.org/data/3.0/onecall?lat=%.4f&lon=%.4f"
          "&exclude=minutely,alerts&units=metric&appid=%s",
          (double)LAT, (double)LON, OWM_API_KEY);
      if (urlLength < 0 || urlLength >= (int)sizeof(url))
      {
        LOG("OWM", "URL troncato (%d caratteri)", urlLength);
        return false;
      }

      // Filtro campi: memorizza solo quelli effettivamente utilizzati.
      JsonDocument filter;
      filter["current"]["dt"]                   = true;
      filter["current"]["feels_like"]           = true;
      filter["current"]["weather"][0]["id"]     = true;
      filter["current"]["weather"][0]["icon"]   = true;
      filter["hourly"][0]["dt"]                 = true;
      filter["hourly"][0]["feels_like"]         = true;
      filter["hourly"][0]["weather"][0]["id"]   = true;
      filter["hourly"][0]["weather"][0]["icon"] = true;

      // parametri accessori (riga precipitazioni, alba/tramonto, barra morn/eve)
      filter["current"]["sunrise"]             = true;
      filter["current"]["sunset"]              = true;
      filter["hourly"][0]["pop"]               = true;
      filter["hourly"][0]["rain"]["1h"]        = true;
      filter["hourly"][0]["snow"]["1h"]        = true;
      filter["daily"][0]["feels_like"]["morn"] = true;
      filter["daily"][0]["feels_like"]["eve"]  = true;

      JsonDocument doc;
      if (!httpGetJsonFiltered(url, doc, filter)) return false;

      // slots[0] <- current
      JsonObjectConst cur = doc["current"].as<JsonObjectConst>();
      if (cur.isNull())
      {
        LOG("OWM", "onecall: current mancante");
        return false;
      }
      // Senza dt lo slot non ha un'ora: non è valido, e expireIfStale() non
      // potrebbe giudicarne l'età.
      WeatherSlot& s0 = slots[0];
      s0.feelsLikeC = cur["feels_like"] | NAN;
      readCondition(cur["weather"][0], s0);
      s0.epoch = (time_t)(cur["dt"] | 0UL);
      s0.valid = !isnan(s0.feelsLikeC) && s0.epoch != 0;

      // `current` non ha pop: probabilità e volumi vengono da hourly[0], l'ora
      // in corso, così il blocco corrente usa la stessa riga delle previsioni.
      JsonVariantConst thisHour = doc["hourly"][0];
      s0.precipitation_prob = thisHour["pop"]        | 0.0f;
      s0.rain1h             = thisHour["rain"]["1h"] | NAN;
      s0.snow1h             = thisHour["snow"]["1h"] | NAN;
      sunriseEpoch = (time_t)(cur["sunrise"] | 0UL);
      sunsetEpoch  = (time_t)(cur["sunset"]  | 0UL);

      logSlot("current", s0);

      // slots[1..3] <- hourly[3], hourly[6], hourly[9]
      JsonArrayConst hourly = doc["hourly"].as<JsonArrayConst>();
      if (hourly.isNull())
      {
        // Fallimento anche con la corrente valida: slots[0] e' gia' stato
        // sovrascritto ma slots[1..3] contengono ancora le previsioni del
        // fetch precedente, e dichiarare successo resetterebbe i timer
        // lasciando in pagina un misto di corrente fresca e previsioni
        // vecchie fino alla cadenza successiva. Ritornando false il
        // giro successivo ritenta.
        LOG("OWM", "onecall: hourly mancante");
        return false;
      }

      static const uint8_t offsets[3] = { 3, 6, 9 };
      for (int i = 0; i < 3; ++i)
      {
        JsonVariantConst item = hourly[offsets[i]];
        WeatherSlot& s = slots[1 + i];
        s.feelsLikeC = item["feels_like"] | NAN;
        readCondition(item["weather"][0], s);
        s.epoch = (time_t)(item["dt"] | 0UL);
        s.valid = !isnan(s.feelsLikeC) && s.epoch != 0;

        // parametri accessori (riga precipitazioni)
        s.precipitation_prob = item["pop"]        | 0.0f;
        s.rain1h             = item["rain"]["1h"] | NAN;
        s.snow1h             = item["snow"]["1h"] | NAN;

        char tag[8];
        snprintf(tag, sizeof(tag), "fc[%d]", i);
        logSlot(tag, s);
      }

      // Barra morn/eve: senza daily[0] i valori tornano NaN e la barra non si
      // disegna, invece di restare quelli di un altro giorno.
      JsonObjectConst today = doc["daily"][0].as<JsonObjectConst>();
      dailyFeelsLikeMorn = today.isNull() ? NAN : (today["feels_like"]["morn"] | NAN);
      dailyFeelsLikeEve  = today.isNull() ? NAN : (today["feels_like"]["eve"]  | NAN);

      return s0.valid;
    }

    // =======================================================================
    // Rendering
    // =======================================================================

    /**
     * Scrive in out la temperatura arrotondata all'intero più vicino. Passa da
     * lroundf e da un intero perchè "%.0f" stamperebbe "-0" fra -0.5 e 0.
     */
    inline void formatRoundedTemp(float celsius, char* out, size_t outLen)
    {
      snprintf(out, outLen, "%ld", lroundf(celsius));
    }

    /**
     * Disegna una temperatura nel formato "<num>°C". Il pallino ° è un
     * cerchietto disegnato fra numero e "C" perchè i font FreeSans*pt7b
     * sono "7b" (ASCII 0x20..0x7E) e non contengono il glifo 0xB0.
     *
     * @param numStr     parte numerica gia' formattata (es. "22", "22.5", "-10.5").
     * @param anchorX    se centered=true è il centro orizzontale del blocco
     *                   composito; se centered=false è il bordo sinistro
     *                   del numero.
     * @param baselineY  baseline del testo (condivisa da numero e "C").
     * @param color      colore di numero, ° e "C".
     * @param degRadius  raggio del cerchietto ° (3 per Bold18pt, 2 per 18pt regular).
     * @param capHeight  altezza a cui sale il cerchietto sopra la baseline.
     * @param centered   true = centra il blocco su anchorX; false = parte
     *                   dal bordo sinistro anchorX.
     */
    inline void drawTempWithDegree(const char* numStr, int16_t anchorX,
                                   int16_t baselineY, uint16_t color,
                                   int16_t degRadius, int16_t capHeight,
                                   bool centered)
    {
      int16_t  x1, y1, x1c, y1c;
      uint16_t wNum, hNum, wC, hC;
      display.getTextBounds(numStr, 0, 0, &x1,  &y1,  &wNum, &hNum);
      display.getTextBounds("C",    0, 0, &x1c, &y1c, &wC,   &hC);

      const int16_t gapBefore = 2;
      const int16_t gapAfter  = 2;
      const int16_t degW      = 2 * degRadius + 1;
      const int16_t totalW    = (int16_t)wNum + gapBefore + degW + gapAfter + (int16_t)wC;

      int16_t x = centered ? (anchorX - totalW / 2) : anchorX;

      display.setTextColor(color);
      display.setCursor(x - x1, baselineY);
      display.print(numStr);

      x += (int16_t)wNum + gapBefore;
      display.drawCircle(x + degRadius, baselineY - capHeight + degRadius, degRadius, color);

      x += degW + gapAfter;
      display.setCursor(x - x1c, baselineY);
      display.print("C");
    }

    /**
     * Compone la riga sotto la temperatura di un blocco meteo. Con una
     * probabilità di precipitazione o un volume previsto diventa
     * "<pop>% X.Xmm", dove i mm sommano pioggia e neve dell'ora; altrimenti è
     * l'etichetta breve del codice condizione. La regola vale per i quattro
     * blocchi: per il corrente pop e volumi vengono dall'ora in corso.
     */
    inline void composeDescLine(const WeatherSlot& s, char* out, size_t outLen)
    {
      const bool hasRain = !isnan(s.rain1h) && s.rain1h > 0.0f;
      const bool hasSnow = !isnan(s.snow1h) && s.snow1h > 0.0f;
      const bool hasPop  = s.precipitation_prob > 0.0f;

      if (!hasRain && !hasSnow && !hasPop)
      {
        copyStr(out, outLen, WeatherIcons::labelFor(s.conditionId));
        return;
      }

      const float volume = (hasRain ? s.rain1h : 0.0f) + (hasSnow ? s.snow1h : 0.0f);
      snprintf(out, outLen, "%ld%% %.1fmm", lroundf(s.precipitation_prob * 100.0f), volume);
    }

    /**
     * Disegna un blocco meteo del banner centrato sul proprio asse: orario,
     * icona, temperatura percepita e la riga sotto, etichetta o
     * precipitazioni. Uno slot non valido non ha icona e mostra "--", perchè
     * l'icona di ripiego sembrerebbe un dato.
     *
     * @param slotIdx  indice dello slot in `slots[]`: 0 = corrente, 1..3 = previsioni.
     * @param blockX   x (px) del margine sinistro del blocco.
     * @param blockW   larghezza (px) del blocco.
     */
    inline void renderBlock(uint8_t slotIdx, int16_t blockX, int16_t blockW)
    {
      const int16_t centerX  = blockX + blockW / 2;
      const int16_t descMaxW = blockW - 2 * Layout::WX_DESC_SIDE_PAD;
      const WeatherSlot& s   = slots[slotIdx];

      char hhmm[6];
      formatHHMM(s.epoch, hhmm);
      display.setFont(Layout::FONT_BODY);
      Graphics::drawCentered(hhmm, centerX, Layout::WX_TIME_BASELINE, GxEPD_BLACK);

      /**
       * Icona centrata nello spazio ICON_SIZE con il lato delle bitmap
       * generate: se le due misure divergono l'icona esce fuori misura, ma
       * drawBitmap non legge mai oltre gli array.
       */
      if (s.valid)
      {
        const WeatherIcons::Icon& icon = WeatherIcons::forCondition(s.conditionId, s.night);
        const int16_t iconX = centerX - WeatherIcons::SIDE / 2;
        const int16_t iconY = Layout::WX_ICON_Y + (Layout::ICON_SIZE - WeatherIcons::SIDE) / 2;
        if (icon.black)
          display.drawBitmap(iconX, iconY, icon.black, WeatherIcons::SIDE, WeatherIcons::SIDE, GxEPD_BLACK);
        if (icon.red)
          display.drawBitmap(iconX, iconY, icon.red, WeatherIcons::SIDE, WeatherIcons::SIDE, GxEPD_RED);
      }

      /** Temperatura percepita in rosso, con il pallino disegnato come
       *  cerchietto (i font 7b non hanno il glifo °). */
      display.setFont(Layout::FONT_LARGE_BOLD);
      if (s.valid)
      {
        char numStr[8];
        formatRoundedTemp(s.feelsLikeC, numStr, sizeof(numStr));
        drawTempWithDegree(numStr, centerX, Layout::WX_TEMP_BASELINE, GxEPD_RED, 3, 18, true);
      }
      else
      {
        Graphics::drawCentered("--", centerX, Layout::WX_TEMP_BASELINE, GxEPD_RED);
      }

      // Una riga troppo larga per FONT_BODY passa a FONT_SMALL; il troncamento
      // resta come salvaguardia, ma etichette e "pop% mm" entrano sempre.
      char descBuf[24];
      if (s.valid) composeDescLine(s, descBuf, sizeof(descBuf));
      else         strcpy(descBuf, "--");
      display.setFont(Layout::FONT_BODY);
      if (Graphics::textWidth(descBuf) > descMaxW)
      {
        display.setFont(Layout::FONT_SMALL);
        Graphics::truncateToWidth(descBuf, descMaxW);
      }
      Graphics::drawCentered(descBuf, centerX, Layout::WX_DESC_BASELINE, GxEPD_BLACK);
    }

    /**
     * Disegna una riga del riquadro Interno: icona piccola nera e testo nel
     * font corrente, incolonnati a partire da startX. Restituisce la x dopo
     * l'ultimo carattere, dove il chiamante accoda unità e pedici.
     *
     * @param icon      bitmap 1bpp 20x20 PROGMEM (definiti in icons.h).
     * @param txt       testo a destra dell'icona.
     * @param startX    bordo sinistro dell'icona.
     * @param baselineY baseline del testo; l'icona si centra sulla sua cap height.
     * @param color     colore del testo.
     */
    inline int16_t drawIndoorRow(const uint8_t* icon, const char* txt,
                                 int16_t startX, int16_t baselineY, uint16_t color)
    {
      display.drawBitmap(startX, Graphics::smallIconY(baselineY), icon,
                         Layout::SMALL_ICON_SIZE, Layout::SMALL_ICON_SIZE, GxEPD_BLACK);
      return Graphics::drawLeftAligned(txt, startX + Layout::SMALL_ICON_SIZE + Layout::SMALL_ICON_GAP,
                                       baselineY, color);
    }

    /**
     * Disegna una riga della colonna alba/tramonto: icona rossa e orario nero,
     * centrati insieme su centerX.
     */
    inline void drawSunRow(const uint8_t* icon, const char* txt, int16_t centerX, int16_t baselineY)
    {
      const int16_t totalW = Layout::SMALL_ICON_SIZE + Layout::SMALL_ICON_GAP + Graphics::textWidth(txt);
      const int16_t startX = centerX - totalW / 2;
      display.drawBitmap(startX, Graphics::smallIconY(baselineY), icon,
                         Layout::SMALL_ICON_SIZE, Layout::SMALL_ICON_SIZE, GxEPD_RED);
      Graphics::drawLeftAligned(txt, startX + Layout::SMALL_ICON_SIZE + Layout::SMALL_ICON_GAP,
                                baselineY, GxEPD_BLACK);
    }

    // =======================================================================
    // @widget slider-temp-range
    //
    // Barra orizzontale con indicatore triangolo che mostra dove cade la
    // temperatura percepita corrente fra morn ed eve (terza riga della
    // colonna sole), più le due cifre agli estremi coi cerchietti dei gradi.
    //
    // Tutto nero e tutto dentro il paged loop, con le primitive di
    // Adafruit_GFX: i pannelli hanno due soli piani, black e accent, e il
    // template GxEPD2_3C li copre entrambi. Il comando 0x28, che su altri
    // controller è un terzo piano, sull'SSD1677 è VCOM Sense: vedi la nota in
    // testa a GxEPD2_SOLUM_097c_960x672.h.
    // =======================================================================

    /**
     * Disegna la barra temp-range completa: barra orizzontale, indicatore
     * triangolare sulla posizione della temperatura percepita corrente, e le
     * cifre morn / eve agli estremi coi cerchietti dei gradi. Tutto in nero.
     *
     * Va chiamata dentro il loop paged, da renderSunColumn.
     *
     * Geometria: la cella logica del widget è larga Layout::TRB_W, con
     * origine a (centerX - bufCenterX, baselineY + Layout::TRB_CELL_Y_OFFSET).
     * La barra sta a cellY + 7 spessa 4 px, il triangolo ha la base sul bordo
     * alto della cella e il vertice sulla barra. Ogni cifra si posiziona con
     * il proprio bearing e il proprio cerchietto con la propria altezza.
     *
     * @param centerX    centro orizzontale (display) del punto medio della
     *                   barra, allineato col centro della riga sunset sopra.
     * @param baselineY  baseline delle cifre (= Layout::SUN_ROW3_BASELINE).
     */
    inline void drawTempRangeBar(int16_t centerX, int16_t baselineY)
    {
      if (!slots[0].valid) return;
      const float morn = dailyFeelsLikeMorn;
      const float eve  = dailyFeelsLikeEve;
      const float cur  = slots[0].feelsLikeC;
      if (isnan(morn) || isnan(eve)) return;

      display.setFont(Layout::FONT_BODY);
      char labL[8], labR[8];
      formatRoundedTemp(morn, labL, sizeof(labL));
      formatRoundedTemp(eve,  labR, sizeof(labR));
      int16_t  x1L, y1L, x1R, y1R;
      uint16_t wL, hL, wR, hR;
      display.getTextBounds(labL, 0, 0, &x1L, &y1L, &wL, &hL);
      display.getTextBounds(labR, 0, 0, &x1R, &y1R, &wR, &hR);

      const int16_t PAD      = 2;
      // Il triangolo sporge di TRI_HALF oltre l'estremo della barra quando la
      // percepita lo raggiunge: il distacco dalle cifre lo copre e lascia
      // una colonna libera prima del loro inchiostro.
      const int16_t TRI_HALF = 5;
      const int16_t GAP      = TRI_HALF + 2;
      const int16_t DEG_GAP  = 2;
      const int16_t degR     = 2;
      const int16_t labSlotL = (int16_t)wL + DEG_GAP + 2*degR + 1;
      const int16_t labSlotR = (int16_t)wR + DEG_GAP + 2*degR + 1;

      // Estremi della barra dentro la cella, e origine della cella sul
      // display: un unico conto per barra, triangolo e cifre.
      const int16_t barLeft    = PAD + labSlotL + GAP;
      const int16_t barRight   = Layout::TRB_W - PAD - labSlotR - GAP;
      const int16_t barWidth   = barRight - barLeft;
      const int16_t bufCenterX = (barLeft + barRight) / 2;
      const int16_t cellX      = centerX - bufCenterX;
      const int16_t cellY      = baselineY + Layout::TRB_CELL_Y_OFFSET;

      const int16_t barY  = 7;
      const int16_t barTh = 4;
      display.fillRect(cellX + barLeft, cellY + barY, barWidth, barTh, GxEPD_BLACK);

      // Indicatore triangolare punta-giù: base sul bordo alto della cella,
      // vertice sulla barra. Base 2 * TRI_HALF + 1 px, altezza barY.
      const float lo = fminf(morn, eve);
      const float hi = fmaxf(morn, eve);
      const float range = hi - lo;
      float ratio = (isnan(cur) || range < 0.5f) ? 0.5f : (cur - lo) / range;
      if (ratio < 0.0f) ratio = 0.0f;
      if (ratio > 1.0f) ratio = 1.0f;
      const int16_t cursorX = cellX + barLeft + (int16_t)(ratio * barWidth + 0.5f);
      display.fillTriangle(cursorX - TRI_HALF, cellY,
                           cursorX + TRI_HALF, cellY,
                           cursorX,     cellY + barY, GxEPD_BLACK);

      display.setTextColor(GxEPD_BLACK);

      // Cifra sinistra con il cerchietto dei gradi vicino al suo apice.
      display.setCursor(cellX + PAD - x1L, baselineY);
      display.print(labL);
      const int16_t degXL = cellX + PAD + (int16_t)wL + DEG_GAP + degR;
      display.drawCircle(degXL, baselineY - (int16_t)hL + degR + 1, degR, GxEPD_BLACK);

      // Cifra destra, con bearing e altezza propri.
      const int16_t rightLabX = cellX + Layout::TRB_W - PAD - labSlotR;
      display.setCursor(rightLabX - x1R, baselineY);
      display.print(labR);
      const int16_t degXR = rightLabX + (int16_t)wR + DEG_GAP + degR;
      display.drawCircle(degXR, baselineY - (int16_t)hR + degR + 1, degR, GxEPD_BLACK);
    }

    // =======================================================================
    // @widget storico-temperature
    //
    // Mini-chart andamento temperatura percepita esterna sotto la slider
    // temp-range (quarta riga della colonna sole).
    //
    // Range temporale: da (now - 3h) a slots[3].epoch (~now + 9h).
    // Tratto passato pieno, tratto previsione tratteggiato, pallino "ora",
    // tutto in nero e dentro il paged loop.
    // =======================================================================

    /**
     * Registra un campione storico della temperatura percepita esterna nel
     * ring buffer. Va chiamata su ogni fetch riuscito (slots[0] valido).
     *
     * Logica anti-doppione: scarta inserimenti a meno di 9 minuti dall'ultimo
     * (tolleranza per retry ravvicinati a fronte del WEATHER_FORECAST_FETCH_MIN
     * default di 10 minuti).
     *
     * @param tC     temperatura percepita (°C); NaN -> no-op.
     * @param epoch  epoch UTC del campione; 0 -> no-op.
     */
    inline void recordHistory(float tC, time_t epoch)
    {
      if (isnan(tC) || epoch == 0) return;
      if (tcHistLastEpoch != 0 && (epoch - tcHistLastEpoch) < 540) return;
      tcHistBuf[tcHistHead] = TempSample{ epoch, tC };
      tcHistHead = (uint8_t)((tcHistHead + 1) % TC_HIST_CAP);
      if (tcHistCount < TC_HIST_CAP) ++tcHistCount;
      tcHistLastEpoch = epoch;
    }

    /**
     * Disegna il mini-chart temperatura sotto la slider temp-range.
     * Larghezza Layout::TC_W centrata su `centerX`, altezza Layout::TC_H
     * a partire da `topY`. Curva, asse, tick, ore e pallino stanno a destra
     * della colonna Layout::TC_LABEL_W, dove si allineano a destra le
     * temperature minima e massima, in FONT_SMALL come le ore.
     *
     * @param centerX centro orizzontale (display) del chart.
     * @param topY    y assoluto del bordo superiore dell'area chart.
     */
    inline void drawTempChart(int16_t centerX, int16_t topY)
    {
      if (!slots[0].valid || isnan(slots[0].feelsLikeC)) return;

      /** Estremi temporali dell'asse X. e1 dal forecast piu' lontano valido,
       *  fallback a now+9h se nessuno e' disponibile. */
      const time_t now = slots[0].epoch;
      const time_t e0  = now - 3 * 3600;
      time_t e1 = now + 9 * 3600;
      if      (slots[3].valid) e1 = slots[3].epoch;
      else if (slots[2].valid) e1 = slots[2].epoch;
      else if (slots[1].valid) e1 = slots[1].epoch;
      if (e1 <= now) e1 = now + 3600;   // safety: asse degenerato

      /** Costruzione dataset ordinato per epoch (storia in range + slots[0..3]). */
      constexpr size_t MAX_PTS = TC_HIST_CAP + 4;
      TempSample pts[MAX_PTS];
      size_t n = 0;

      const uint8_t tail = (uint8_t)((tcHistHead + TC_HIST_CAP - tcHistCount) % TC_HIST_CAP);
      for (uint8_t i = 0; i < tcHistCount; ++i)
      {
        const TempSample& s = tcHistBuf[(tail + i) % TC_HIST_CAP];
        if (s.epoch >= e0 && s.epoch < now && !isnan(s.t))
          pts[n++] = s;
      }
      for (uint8_t k = 0; k < 4; ++k)
      {
        if (!slots[k].valid || isnan(slots[k].feelsLikeC)) continue;
        pts[n++] = TempSample{ slots[k].epoch, slots[k].feelsLikeC };
      }
      if (n == 0) return;

      /** Geometria del chart. */
      const int16_t chartL  = centerX - Layout::TC_W / 2 + Layout::TC_LABEL_W;
      const int16_t chartR  = centerX + Layout::TC_W / 2;
      const int16_t axisY   = topY + Layout::TC_H - Layout::TC_AXIS_PAD_BOT;
      const int16_t plotTop = topY + 6;
      const int16_t plotBot = axisY - 1;

      /** Range Y con clamp minimo di 1°C per evitare divisioni piatte. */
      float tmin = pts[0].t, tmax = pts[0].t;
      for (size_t i = 1; i < n; ++i)
      {
        if (pts[i].t < tmin) tmin = pts[i].t;
        if (pts[i].t > tmax) tmax = pts[i].t;
      }
      if (tmax - tmin < 1.0f)
      {
        const float mid = (tmax + tmin) * 0.5f;
        tmin = mid - 0.5f;
        tmax = mid + 0.5f;
      }

      /** Mappature epoch->x e t->y. Clamp ai bordi per non sforare. */
      auto mapX = [&](time_t e) -> int16_t
      {
        long span = (long)(e1 - e0); if (span <= 0) span = 1;
        long rel  = (long)(e - e0);
        long px   = ((long)(chartR - chartL) * rel) / span;
        int16_t x = chartL + (int16_t)px;
        if (x < chartL) x = chartL;
        if (x > chartR) x = chartR;
        return x;
      };
      auto mapY = [&](float t) -> int16_t
      {
        float ratio = (t - tmin) / (tmax - tmin);
        if (ratio < 0.0f) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;
        // Y display cresce verso il basso: t alta -> y basso.
        int16_t y = plotBot - (int16_t)(ratio * (float)(plotBot - plotTop) + 0.5f);
        if (y < plotTop) y = plotTop;
        if (y > plotBot) y = plotBot;
        return y;
      };
      // Catmull-Rom uniforme: curva smooth passa per i 4 punti di controllo.
      auto catmull = [](float p0, float p1, float p2, float p3, float u) -> float
      {
        const float u2 = u * u;
        const float u3 = u2 * u;
        return 0.5f * ((2.0f * p1)
                      + (-p0 + p2) * u
                      + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u2
                      + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * u3);
      };

      /** Disegno curva: per ogni segmento [pts[i], pts[i+1]] interpolazione
       *  Catmull-Rom con vicini pts[i-1] e pts[i+2] (clamp ai bordi).
       *  "Adesso" è sempre un vertice, perchè lo storico sta strettamente prima
       *  di slots[0] e le previsioni dopo: ogni segmento è tutto passato (pieno)
       *  o tutto previsione (tratteggiato). Tutto è nero, perchè un tratto rosso
       *  che si sposta a ogni fetch impedirebbe il refresh parziale. La fase del
       *  tratteggio parte dal vertice "adesso" e avanza solo sui passi della
       *  previsione, quindi non dipende dalla lunghezza dello storico. */
      if (n >= 2)
      {
        int16_t dashPhase = 0;
        for (size_t i = 0; i < n - 1; ++i)
        {
          const TempSample& p1 = pts[i];
          const TempSample& p2 = pts[i + 1];
          const TempSample& p0 = (i == 0)         ? p1 : pts[i - 1];
          const TempSample& p3 = (i + 2 >= n)     ? p2 : pts[i + 2];
          const bool isForecast = (p1.epoch >= now);

          const int16_t x1 = mapX(p1.epoch);
          const int16_t x2 = mapX(p2.epoch);
          int16_t steps = x2 - x1;
          if (steps < 1) steps = 1;

          int16_t prevX = x1;
          int16_t prevY = mapY(p1.t);
          for (int16_t s = 1; s <= steps; ++s)
          {
            const float   u  = (float)s / (float)steps;
            const float   tu = catmull(p0.t, p1.t, p2.t, p3.t, u);
            const int16_t cx = x1 + (int16_t)(((long)(x2 - x1) * (long)s) / (long)steps);
            const int16_t cy = mapY(tu);
            bool visible = true;
            if (isForecast)
            {
              visible = (dashPhase % (Layout::TC_DASH_ON + Layout::TC_DASH_OFF)) < Layout::TC_DASH_ON;
              ++dashPhase;
            }
            if (visible) display.drawLine(prevX, prevY, cx, cy, GxEPD_BLACK);
            prevX = cx;
            prevY = cy;
          }
        }
      }

      /** Asse X: linea orizzontale alla base dell'area plot. */
      display.drawFastHLine(chartL, axisY, chartR - chartL, GxEPD_BLACK);

      /** 3 tick: inizio range (e0), mediana temporale (eMid), fine range (e1).
       *  Il "now" (slots[0]) non e' un tick: e' indicato dal pallino sulla curva. */
      const time_t  eMid = e0 + (e1 - e0) / 2;
      const int16_t xMid = mapX(eMid);
      display.drawFastVLine(chartL,     axisY + 1, Layout::TC_TICK_H, GxEPD_BLACK);
      display.drawFastVLine(xMid,       axisY + 1, Layout::TC_TICK_H, GxEPD_BLACK);
      display.drawFastVLine(chartR - 1, axisY + 1, Layout::TC_TICK_H, GxEPD_BLACK);

      /** Ora intera sotto ogni tick, con clamp ai bordi del chart: in
       *  FONT_SMALL "HH:MM" non entrerebbe nei 42 px fra un tick e l'altro. */
      display.setFont(Layout::FONT_SMALL);
      display.setTextColor(GxEPD_BLACK);
      const int16_t labY = axisY + Layout::TC_AXIS_PAD_BOT - 1;
      auto drawHour = [&](time_t e, int16_t tickX, bool clampLeft, bool clampRight)
      {
        struct tm tmL;
        localtime_r(&e, &tmL);
        char buf[4];
        snprintf(buf, sizeof(buf), "%02d", tmL.tm_hour);
        int16_t  bx1, by1;
        uint16_t tw, th;
        display.getTextBounds(buf, 0, 0, &bx1, &by1, &tw, &th);
        int16_t tx = tickX - (int16_t)tw / 2 - bx1;
        if (clampLeft  && tx + bx1 < chartL)
          tx = chartL - bx1;
        if (clampRight && tx + bx1 + (int16_t)tw > chartR)
          tx = chartR - (int16_t)tw - bx1;
        display.setCursor(tx, labY);
        display.print(buf);
      };
      drawHour(e0,   chartL,     true,  false);
      drawHour(eMid, xMid,       false, false);
      drawHour(e1,   chartR - 1, false, true);

      /** Temperature massima e minima allineate a destra verso la curva, con
       *  la massima che parte dalla cima dell'area di plot. */
      char numBuf[8];
      const int16_t labRightX = chartL - 3;
      formatRoundedTemp(tmax, numBuf, sizeof(numBuf));
      Graphics::drawRightAligned(numBuf, labRightX, plotTop + Graphics::capHeight(), GxEPD_BLACK);
      formatRoundedTemp(tmin, numBuf, sizeof(numBuf));
      Graphics::drawRightAligned(numBuf, labRightX, axisY - 1, GxEPD_BLACK);

      /** Pallino sulla temperatura corrente (sopra la curva). */
      display.fillCircle(mapX(now), mapY(slots[0].feelsLikeC), Layout::TC_DOT_R, GxEPD_BLACK);
    }

    /**
     * Disegna il contenuto del riquadro Interno, quattro righe con i dati del
     * sensore BME680: temperatura, umidità, qualità dell'aria con il
     * pedice di accuratezza e pressione, tutto in nero perchè i valori cambiano
     * a ogni ciclo e un testo rosso impedirebbe il refresh parziale. Valori in
     * FONT_LARGE, l'IAQ in
     * FONT_BODY perchè "Mediocre 500" in FONT_LARGE non entra, unità e
     * pedice in FONT_SMALL.
     */
    inline void renderIndoorBox()
    {
      const Indoor::Sample& s = Indoor::sample();
      const int16_t iconX     = Layout::INDOOR_ICON_X;
      const int16_t textX     = iconX + Layout::SMALL_ICON_SIZE + Layout::SMALL_ICON_GAP;
      char buf[24];

      // Temperatura, con il "°" disegnato come cerchietto.
      display.setFont(Layout::FONT_LARGE);
      display.drawBitmap(iconX, Graphics::smallIconY(Layout::INDOOR_ROW1_BASELINE), INDOOR_ICON_TEMPERATURE,
                         Layout::SMALL_ICON_SIZE, Layout::SMALL_ICON_SIZE, GxEPD_BLACK);
      if (s.valid)
      {
        snprintf(buf, sizeof(buf), "%.1f", s.temperature);
        drawTempWithDegree(buf, textX, Layout::INDOOR_ROW1_BASELINE, GxEPD_BLACK, 2, 16, false);
      }
      else
      {
        Graphics::drawLeftAligned("--", textX, Layout::INDOOR_ROW1_BASELINE, GxEPD_BLACK);
      }

      // Umidità, con il "%" in FONT_SMALL accanto al valore.
      if (s.valid) snprintf(buf, sizeof(buf), "%u", (unsigned)s.humidity);
      else         strcpy(buf, "--");
      int16_t endX = drawIndoorRow(INDOOR_ICON_HUMIDITY, buf, iconX, Layout::INDOOR_ROW2_BASELINE, GxEPD_BLACK);
      if (s.valid)
      {
        display.setFont(Layout::FONT_SMALL);
        Graphics::drawLeftAligned("%", endX + Layout::INDOOR_UNIT_GAP, Layout::INDOOR_ROW2_BASELINE, GxEPD_BLACK);
      }

      // Qualità dell'aria: etichetta e indice, con l'accuratezza come pedice.
      display.setFont(Layout::FONT_BODY);
      if (s.valid) snprintf(buf, sizeof(buf), "%s %u", Indoor::iaqLabel(s.iaq), (unsigned)s.iaq);
      else         strcpy(buf, "--");
      endX = drawIndoorRow(INDOOR_ICON_AIR_QUALITY, buf, iconX, Layout::INDOOR_ROW3_BASELINE, GxEPD_BLACK);
      if (s.valid)
      {
        char accStr[4];
        snprintf(accStr, sizeof(accStr), "%u", (unsigned)s.iaqAccuracy);
        display.setFont(Layout::FONT_SMALL);
        Graphics::drawLeftAligned(accStr, endX + Layout::INDOOR_UNIT_GAP,
                                  Layout::INDOOR_ROW3_BASELINE + Layout::INDOOR_SUBSCRIPT_DROP, GxEPD_BLACK);
      }

      // Pressione, con "mBar" in FONT_SMALL accanto al valore.
      display.setFont(Layout::FONT_LARGE);
      if (s.valid) snprintf(buf, sizeof(buf), "%u", (unsigned)s.pressure);
      else         strcpy(buf, "--");
      endX = drawIndoorRow(INDOOR_ICON_PRESSURE, buf, iconX, Layout::INDOOR_ROW4_BASELINE, GxEPD_BLACK);
      if (s.valid)
      {
        display.setFont(Layout::FONT_SMALL);
        Graphics::drawLeftAligned("mBar", endX + Layout::INDOOR_UNIT_GAP, Layout::INDOOR_ROW4_BASELINE, GxEPD_BLACK);
      }
    }

    /**
     * Disegna la colonna sole nel riquadro Meteo, a destra del blocco
     * corrente, centrata su Layout::SUN_COL_CENTER_X: alba e tramonto di oggi
     * con le icone in rosso, la barra mattino/sera (@widget slider-temp-range)
     * e sotto il grafico della temperatura percepita (@widget storico-temperature).
     */
    inline void renderSunColumn()
    {
      display.setFont(Layout::FONT_BODY);

      char hhmm[6];
      formatHHMM(sunriseEpoch, hhmm);
      drawSunRow(INDOOR_ICON_SUNRISE, hhmm, Layout::SUN_COL_CENTER_X, Layout::SUN_ROW1_BASELINE);

      formatHHMM(sunsetEpoch, hhmm);
      drawSunRow(INDOOR_ICON_SUNSET, hhmm, Layout::SUN_COL_CENTER_X, Layout::SUN_ROW2_BASELINE);

      drawTempRangeBar(Layout::SUN_COL_CENTER_X, Layout::SUN_ROW3_BASELINE);
      drawTempChart(Layout::SUN_COL_CENTER_X, Layout::TC_TOP_Y);
    }

    /**
     * Testo del badge di guasto della telemetria Tuya. Costante e non
     * letterale ripetuto perchè serve due volte, per misurarlo e per
     * stamparlo.
     */
    inline constexpr const char* TUYA_BADGE_TEXT = "TUYA ERR";

    /**
     * Disegna il badge di guasto della telemetria Tuya in rosso, seduto sul
     * bordo inferiore del riquadro Interno e allineato al suo margine destro.
     * Sul bordo superiore non entra: "Interno" in FONT_TITLE occupa 80 px e il
     * badge in FONT_SMALL 100, contro i 168 utili del riquadro.
     */
    inline void drawIndoorTuyaBadge()
    {
      display.setFont(Layout::FONT_SMALL);
      const int16_t width   = Graphics::textWidth(TUYA_BADGE_TEXT);
      const int16_t rightX  = Layout::INDOOR_X + Layout::INDOOR_W - Layout::TITLE_INSET;
      const int16_t borderY = Layout::BANNER_Y + Layout::BANNER_H - 1;
      display.fillRect(rightX - width - 4, borderY - 1, width + 8, 3, GxEPD_WHITE);
      Graphics::drawRightAligned(TUYA_BADGE_TEXT, rightX, borderY + Graphics::capHeight() / 2, GxEPD_RED);
    }

    /**
     * Disegna il banner meteo in fondo allo schermo: i riquadri Interno, Meteo
     * (blocco corrente e colonna sole) e Previsioni (tre blocchi affiancati),
     * con i titoli sul bordo superiore. Il fondo è già bianco, perchè il
     * template azzera il buffer a ogni pagina e sopra il banner disegna solo il
     * wallpaper, tagliato all'altezza della sua area.
     */
    inline void drawBanner()
    {
      display.setFont(Layout::FONT_TITLE);
      Graphics::drawFieldsetRect(Layout::INDOOR_X, Layout::BANNER_Y, Layout::INDOOR_W, Layout::BANNER_H,
                                 Layout::BOX_RADIUS, "Interno", Layout::TITLE_INSET, GxEPD_WHITE);
      Graphics::drawFieldsetRect(Layout::WEATHER_X, Layout::BANNER_Y, Layout::WEATHER_W, Layout::BANNER_H,
                                 Layout::BOX_RADIUS, "Meteo", Layout::TITLE_INSET, GxEPD_WHITE);
      Graphics::drawFieldsetRect(Layout::FORECAST_X, Layout::BANNER_Y, Layout::FORECAST_W, Layout::BANNER_H,
                                 Layout::BOX_RADIUS, "Previsioni", Layout::TITLE_INSET, GxEPD_WHITE);

      if (Tuya::hasFailed())
        drawIndoorTuyaBadge();

      renderIndoorBox();
      renderBlock(0, Layout::CURRENT_BLOCK_X, Layout::CURRENT_BLOCK_W);
      renderSunColumn();
      for (uint8_t i = 0; i < 3; ++i)
        renderBlock(1 + i, Layout::FORECAST_X + i * Layout::FORECAST_BLOCK_W, Layout::FORECAST_BLOCK_W);
    }

    /**
     * Stato della politica del refresh differenziale: ultimo frame, temperatura
     * che ha fatto scegliere il pieno per freddo, contatori del giorno.
     */
    inline PartialRefresh::State partialState;

    /**
     * Un loop paged completo (background + banner). Ogni pagina parte da un
     * buffer bianco, perchè firstPage() e nextPage() lo azzerano, e disegna solo
     * i riquadri che toccano la sua fascia: quelli con il titolo sul bordo
     * superiore si allargano in alto di STACK_GAP, perchè il titolo sporge sopra
     * il bordo. Il tempo misurato è il solo disegno, senza nextPage(), che
     * contiene SPI e refresh. Con il differenziale armato il driver confronta
     * qui il piano rosso e decide se il frame diventa un partial.
     */
    inline void renderPages(time_t calEpoch)
    {
      uint32_t drawUs = 0;
      uint16_t page = 0;
      display.firstPage();
      do
      {
        const uint32_t pageStartUs = micros();
        Graphics::beginPage(page++);
        drawTestBackground();
        if (Graphics::intersectsPage(Layout::CAL_Y - Layout::STACK_GAP, Layout::CAL_H + Layout::STACK_GAP))
          Calendar::draw(calEpoch);
        if (Graphics::intersectsPage(Layout::MAIL_Y - Layout::STACK_GAP, Layout::MAIL_H + Layout::STACK_GAP))
          Mail::draw();
        if (Graphics::intersectsPage(Layout::BANNER_Y - Layout::STACK_GAP,
                                     Layout::SCREEN_H - Layout::BANNER_Y + Layout::STACK_GAP))
          drawBanner();
        drawUs += micros() - pageStartUs;
      } while (display.nextPage());
      Graphics::endPages();
      LOGV("render", "disegno %lu ms in %u pagine", (unsigned long)(drawUs / 1000UL), (unsigned)page);
    }

    /**
     * Giorno locale come chiave intera per i contatori del log, -1 senza ora
     * valida: i contatori non si azzerano finchè l'orologio non è affidabile.
     */
    inline int32_t localDayKey()
    {
      if (!Clock::valido()) return -1;
      const time_t now = time(nullptr);
      struct tm local;
      localtime_r(&now, &local);
      return (int32_t)local.tm_year * 1000 + (int32_t)local.tm_yday;
    }

    /**
     * Compone un frame completo (background + banner) sul display usando il
     * pattern paged di GxEPD2. Un refresh pieno costa ~24 s misurati, un partial
     * B/N ~0,64 s: il partial è possibile solo se il piano rosso è identico a
     * quello sul vetro, e questo lo sa solo il driver, che lo verifica mentre
     * riceve le pagine (vedi la sezione REFRESH DIFFERENZIALE del driver e la
     * politica in PartialRefresh.h).
     *
     * Se la politica lo consente il frame si disegna una prima volta con il
     * differenziale armato. Se il driver lancia il partial il frame è finito; se
     * rifiuta perchè il rosso è cambiato non ha toccato il pannello e il frame si
     * disegna una seconda volta, in pieno. Se il driver non può armare (nessun
     * riferimento) il primo disegno è già pieno e non se ne fa un secondo. Con
     * DISPLAY_PARTIAL_REFRESH a 0 o con un pannello senza differenziale è
     * sempre un solo disegno pieno.
     */
    inline void renderFrame()
    {
      using Epd = decltype(display.epd2);
      display.setFullWindow();
      /**
       * Epoch di riferimento per il calendario: l'orologio di sistema quando è
       * sincronizzato, così il calendario non dipende dalla riuscita del
       * meteo; senza orologio l'ora dell'ultimo meteo, che è comunque reale;
       * senza nessuno dei due 0, e Calendar::draw() mostra il riquadro vuoto.
       */
      const time_t calEpoch = Clock::valido() ? time(nullptr)
                                              : (slots[0].valid ? slots[0].epoch : 0);

      const uint32_t startMs = millis();
      PartialRefresh::Reason decision = PartialRefresh::Reason::Disabled;
      PartialRefresh::Reason outcome = PartialRefresh::Reason::Disabled;
      bool armed = false;
      bool partialDone = false;
      uint16_t refusedMask = 0;

      if constexpr (PartialRefresh::isAvailable<Epd>())
      {
        const Indoor::Sample& sensor = Indoor::sample();
        const PartialRefresh::Inputs inputs = PartialRefresh::makeInputs(
            partialState, startMs, true, PartialRefresh::chain(display.epd2),
            Indoor::attivo(), sensor.valid, sensor.temperature);
        decision = PartialRefresh::decide(PartialRefresh::DEFAULT_PARAMS, inputs);
        if (decision == PartialRefresh::Reason::Partial)
        {
          // Va armato prima di firstPage(), che chiama setPaged() nel driver.
          armed = PartialRefresh::beginFrame(display.epd2);
          if (armed)
          {
            renderPages(calEpoch);
            partialDone = PartialRefresh::endFrame(display.epd2);
            // La mask si legge ora: il secondo disegno la ripulirebbe.
            if (!partialDone) refusedMask = PartialRefresh::refusedPages(display.epd2);
          }
        }
      }

      // Se il partial non è partito serve un disegno pieno: dopo un rifiuto è il
      // secondo, negli altri casi (politica contraria, nessun riferimento, driver
      // senza differenziale) è l'unico, perchè un frame non armato non si disegna
      // prima.
      if (!partialDone) renderPages(calEpoch);

      if constexpr (PartialRefresh::isAvailable<Epd>())
      {
        if (partialDone) outcome = PartialRefresh::Reason::Partial;
        else if (decision == PartialRefresh::Reason::Partial)
          outcome = (armed && refusedMask != 0) ? PartialRefresh::Reason::RedChanged
                                                : PartialRefresh::Reason::NoReference;
        else outcome = decision;

        const uint32_t endMs = millis();
        PartialRefresh::onFrameDone(partialState, endMs, decision, outcome, localDayKey());
        const PartialRefresh::DayCounters& day = partialState.day;
        if (partialDone)
          LOG("display", "partial %u/%u in %lu ms (oggi: %u partial, %u pieni, %u per rosso)",
              (unsigned)PartialRefresh::chain(display.epd2), (unsigned)PARTIAL_CHAIN_MAX,
              (unsigned long)(endMs - startMs), (unsigned)day.partial, (unsigned)day.full,
              (unsigned)day.redRefused);
        else if (outcome == PartialRefresh::Reason::RedChanged)
          LOG("display", "pieno: %s (pagine 0x%02X) in %lu ms (oggi: %u partial, %u pieni, %u per rosso)",
              PartialRefresh::reasonText(outcome), (unsigned)refusedMask,
              (unsigned long)(endMs - startMs), (unsigned)day.partial, (unsigned)day.full,
              (unsigned)day.redRefused);
        else
          LOG("display", "pieno: %s in %lu ms (oggi: %u partial, %u pieni, %u per rosso)",
              PartialRefresh::reasonText(outcome), (unsigned long)(endMs - startMs),
              (unsigned)day.partial, (unsigned)day.full, (unsigned)day.redRefused);
      }
    }

    /**
     * Invalida il meteo in cache quando l'osservazione corrente è più vecchia
     * di WEATHER_MAX_AGE_S: con OpenWeather in errore il banner passa ai "--"
     * invece di mostrare dati vecchi. Lo storico del grafico resta in memoria
     * e ricompare con il primo fetch riuscito. Senza orologio l'età non è
     * calcolabile e la cache resta com'è.
     */
    inline void expireIfStale()
    {
      if (!slots[0].valid || !Clock::valido()) return;
      const time_t age = time(nullptr) - slots[0].epoch;
      if (age <= (time_t)WEATHER_MAX_AGE_S) return;
      for (auto& s : slots) s.valid = false;
      sunriseEpoch       = 0;
      sunsetEpoch        = 0;
      dailyFeelsLikeMorn = NAN;
      dailyFeelsLikeEve  = NAN;
      LOG("OWM", "meteo vecchio di %ld min: mostro i placeholder", (long)(age / 60));
    }
  } // namespace detail

  /**
   * Vero se Env.h contiene una API key OpenWeather. È la precondizione del
   * task meteo nel .ino: senza chiave ogni slot spenderebbe un handshake per un
   * 401 certo.
   */
  inline bool isConfigured()
  {
    return Net::isCredentialSet(OWM_API_KEY);
  }

  // =========================================================================
  // API pubblica
  // =========================================================================

  /**
   * Inizializza stato e timer del modulo. Non fa rete, non disegna.
   * Va chiamata una sola volta in setup() dopo initDisplay().
   */
  inline void begin()
  {
    using namespace detail;
    for (auto& s : slots)
    {
      s.valid = false;
      s.epoch = 0;
      s.conditionId = 0;
      s.night       = false;
      // parametri accessori (riga precipitazioni)
      s.precipitation_prob = 0.0f;
      s.rain1h             = NAN;
      s.snow1h             = NAN;
    }
    // parametri accessori (riga pioggia, alba/tramonto, barra morn/eve)
    sunriseEpoch       = 0;
    sunsetEpoch        = 0;
    dailyFeelsLikeMorn = NAN;
    dailyFeelsLikeEve  = NAN;

    // Reset storico del mini-chart temperatura (igienico: BSS gia' a 0).
    tcHistCount     = 0;
    tcHistHead      = 0;
    tcHistLastEpoch = 0;

    if (!isConfigured())
      LOG("OWM", "API key assente in Env.h: meteo disattivato");
  }

  /**
   * Vero quando la cache contiene sia il meteo corrente sia almeno la prima
   * previsione futura. Sotto questa soglia il banner andrebbe disegnato con i
   * placeholder "--", quindi lo scheduler ritenta invece di attendere la
   * cadenza piena.
   */
  inline bool datiCompleti()
  {
    using namespace detail;
    return slots[0].valid && slots[1].valid;
  }

  /**
   * Scarica meteo corrente e previsioni. Presuppone il WiFi gia' connesso
   * (WL_CONNECTED): non accende ne' spegne la radio, e non decide quando
   * tocca a lui, che e' compito dello scheduler.
   *
   * In caso di successo registra il campione corrente nello storico del
   * mini-chart temperatura. Il dato compare nel frame dello stesso ciclo, che
   * lo scheduler disegna dopo i fetch.
   * @return true se la richiesta e' andata a buon fine.
   */
  inline bool runFetch()
  {
    using namespace detail;
    const bool ok = fetchOneCall();
    if (ok) recordHistory(slots[0].feelsLikeC, slots[0].epoch);
    return ok;
  }

  /**
   * Ridisegna lo schermo (background + banner). Nessuna rete.
   *
   * Non decide se sia il momento: lo scheduler la chiama a ogni ciclo della
   * fascia WiFi, dopo il sensore e i fetch dello stesso ciclo. Prima di
   * disegnare scarta il meteo troppo vecchio, vedi detail::expireIfStale().
   */
  inline void render()
  {
    using namespace detail;
    expireIfStale();
    renderFrame();
  }
}

#endif

#ifndef WEATHER_H
#define WEATHER_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <stdint.h>

#include <GxEPD2_3C.h>
#include "GxEPD2_097c_SOLUM_672x960/GxEPD2_097c_SOLUM_672x960.h"

#include <Fonts/FreeSans9pt7b.h>      // condizione IAQ + valore, terza riga indoor
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/Picopixel.h>         // font micro ~6 px, per pedice accuracy IAQ

#include "Env.h"
#include "icons.h"
#include "Graphics.h"
#include "Calendar.h"
#include "Indoor.h"

// ---------------------------------------------------------------------------
// Istanza del display (definita nello sketch .ino).
// ---------------------------------------------------------------------------
extern GxEPD2_3C<GxEPD2_097c_SOLUM_672x960, GxEPD2_097c_SOLUM_672x960::HEIGHT / 8> display;

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
 * slots[1..3] da `hourly[3/6/9]` (step 3h). Alcuni campi extra (sunrise,
 * sunset, rain.1h, pop, daily.feels_like.morn/eve) vengono memorizzati
 * per un uso futuro ma non ancora visualizzati.
 *
 * Il WiFi NON e' gestito qui: runFetch() presuppone che sia gia' connesso
 * (lo sketch .ino si occupa di wifiOn()/wifiOff() immediatamente intorno
 * alla chiamata). Cosi' la radio resta spenta fra un fetch e l'altro.
 *
 * Credenziali WiFi e API key OWM sono in Env.h. Posizione GPS (LAT/LON)
 * pure in Env.h. Il fuso orario e' gestito da Calendar::initTimezone()
 * (POSIX TZ in Calendar.h): localtime_r() applica CET/CEST in automatico.
 */
namespace Weather
{
  // Tipo di fetch pendente. Maschera di bit per poter combinare le richieste
  enum FetchKind : uint8_t
  {
    FETCH_NONE     = 0,
    FETCH_CURRENT_WEATHER  = 1 << 0,
    FETCH_FORECAST = 1 << 1,
    FETCH_BOTH     = FETCH_CURRENT_WEATHER | FETCH_FORECAST
  };

  // -------------------------------------------------------------------------
  // Stato, costanti di layout e helper interni. `detail` tiene tutto lo
  // scope interno fuori dalla API pubblica del modulo.
  // -------------------------------------------------------------------------
  namespace detail
  {
    // -----------------------------------------------------------------------
    // Intervalli di fetch (millisecondi). I valori derivano dai #define
    // dichiarati nello sketch .ino; i fallback qui sotto rendono l'header
    // autonomamente compilabile. One Call 3.0 restituisce corrente + hourly
    // + daily in un unica chiamata: un solo timer governa l'intero fetch.
    // -----------------------------------------------------------------------
    #ifndef WEATHER_FORECAST_FETCH_MIN
    #define WEATHER_FORECAST_FETCH_MIN 10
    #endif
    inline constexpr uint32_t INTERVAL_FORECAST =
        (uint32_t)WEATHER_FORECAST_FETCH_MIN * 60UL * 1000UL;  // default 10 min

    // -----------------------------------------------------------------------
    // Layout banner.
    // -----------------------------------------------------------------------

    /**
     * Colonna bianca a destra: ospita il calendario del mese (in alto) e la
     * lista eventi Outlook (sotto). Si estende dall'alto fino al top del
     * banner meteo.
     * Modifica 21/04/26: area immagine rimanente ridotta a 620x460. 
     */
    /** Sidebar bianca a destra.
     *  Modifica 22/04/26: wallpaper slideshow dimensione finale 620x440;
     *  sidebar quindi 340 px (960-620), alta 460 (fino al top del banner).
     *  La fascia orizzontale 440..460 x 0..620 resta bianca (fillScreen). */
    inline constexpr int16_t SIDEBAR_W      = 340;
    inline constexpr int16_t SIDEBAR_X      = 960 - SIDEBAR_W;  // = 620

    inline constexpr int16_t BANNER_Y       = 460;   // era 480 - top del banner (screen 960x672)
    inline constexpr int16_t BANNER_H       = 212;   // era 192 - margine extra per orario 18pt
    inline constexpr int16_t BANNER_W       = 960;

    /**
     * Layout del banner a 3 riquadri fieldset.
     * Modifica 22/04/26 (v8): Indoor ridotto a 154, Weather espanso a
     * 306 per lasciare piu' spazio alla sub-col sun (alba/tramonto).
     *   Indoor | Weather            | Forecast (3 slot)
     *    154  |  306 (meteo+sun)    |  470
     *   x=5   |  x=169              |  x=485
     * Inset 5 px laterali, gap 10 px. Totale: 5+154+10+306+10+470+5 = 960.
     */
    inline constexpr int16_t INDOOR_RR_X     = 5;
    inline constexpr int16_t INDOOR_RR_W     = 154;
    inline constexpr int16_t WEATHER_RR_X    = 169;
    inline constexpr int16_t WEATHER_RR_W    = 306;
    inline constexpr int16_t FORECAST_RR_X   = 485;
    inline constexpr int16_t FORECAST_RR_W   = 470;

    /**
     * Slot del meteo corrente: sezione sinistra del riquadro Weather.
     * Il resto (offset SUN_COL_OFFSET in poi) ospita la sub-colonna sun.
     * Modifica 22/04/26 (v8): blocco current spostato di 12 px a sinistra
     * rispetto al bordo del Weather RR (v7 era -6, qui -6 aggiuntivi) e
     * SUN_COL_OFFSET ridotto a 188 (era 194) per allargare la sub-col
     * sun verso sinistra -> piu' spazio per alba/tramonto.
     */
    inline constexpr int16_t BLOCK_CURRENT_X = WEATHER_RR_X - 12;
    inline constexpr int16_t BLOCK_CURRENT_W = 200;

    /**
     * Offset (relativo a WEATHER_RR_X) del bordo sinistro delle icone
     * della sub-colonna sun dentro il riquadro Weather.
     */
    inline constexpr int16_t SUN_COL_OFFSET  = 188;

    /** Slot forecast: 3 slot da ~156 px dentro il riquadro Forecast (470 px). */
    inline constexpr int16_t BLOCK_FC_W      = 156;
    inline constexpr int16_t BLOCK_FC0_X     = FORECAST_RR_X;                    // 485
    inline constexpr int16_t BLOCK_FC1_X     = BLOCK_FC0_X + BLOCK_FC_W;         // 641
    inline constexpr int16_t BLOCK_FC2_X     = BLOCK_FC1_X + BLOCK_FC_W;         // 797

    /**
     * Baseline dei testi dentro un blocco meteo (current / forecast).
     * Modifica 22/04/26 (v7): TEMP -2 (155->153) e TIME -2 (190->188)
     * per alzare leggermente la riga temperatura e di conseguenza
     * l'orario sia nel current che nei forecast.
     */
    inline constexpr int16_t ICON_Y         = BANNER_Y + 6;    // top icona (88x88)
    inline constexpr int16_t DESC_BASELINE  = BANNER_Y + 118;  // baseline description
    inline constexpr int16_t TEMP_BASELINE  = BANNER_Y + 153;  // baseline temperatura
    inline constexpr int16_t TIME_BASELINE  = BANNER_Y + 188;  // baseline orario

    /**
     * Baseline delle 4 righe dati condivise da Indoor (colonna unica)
     * e dalla sub-colonna sun nel riquadro Weather.
     * Modifica 22/04/26 (v6): spaziatura uniforme fra le 4 righe E
     * fra il bordo superiore/inferiore del rr. Con rr interno = 202 px
     * e altezza visiva testo ~27 px (FreeSans18pt ascent 22 + descent 5),
     * 5 spazi uguali danno G = (202-4*27)/5 = 19 px; spacing baseline =
     * 27+19 = 46. Baselines 46..184 con interlinee di 46 px.
     */
    inline constexpr int16_t INDOOR_ROW1_BASELINE = BANNER_Y + 46;
    inline constexpr int16_t INDOOR_ROW2_BASELINE = BANNER_Y + 92;
    inline constexpr int16_t INDOOR_ROW3_BASELINE = BANNER_Y + 138;
    inline constexpr int16_t INDOOR_ROW4_BASELINE = BANNER_Y + 184;

    /** Gap (px) fra icona e testo nelle righe della sub-colonna indoor. */
    inline constexpr int16_t INDOOR_ICON_GAP = 6;

    /**
     * Offset (relativo a INDOOR_RR_X) del bordo sinistro delle icone della
     * colonna unica dentro il riquadro Indoor (T, umidita', IAQ, pressione).
     */
    inline constexpr int16_t INDOOR_COL1_OFFSET = 10;    // colonna inizia a x=15

    /** Numero di immagini nella rotazione di background. */

    // -----------------------------------------------------------------------
    // Cache dati meteo.
    // -----------------------------------------------------------------------
    /**
     * Slot di dati meteo. Slot[0] = previsione corrente (orario = download),
     * slot[1..3] = 3 previsioni future (orario = epoch dell'API).
     */
    struct WeatherSlot
    {
      char    iconCode[4];        // es. "10d"
      char    description[48];    // es. "pioggia leggera" (lang=it)
      float   feelsLikeC;         // feels_like (temperatura percepita, °C)
      time_t  epoch;              // orario locale dell'osservazione/previsione
      bool    valid;

      // parametri aggiuntivi (memorizzati, non ancora mostrati)
      float   precipitation_prob; // hourly[].pop [0..1]; 0 per slots[0]
      float   rain1h;             // hourly[].rain.1h (mm, previsti per l'ora); NaN se assente
    };

    inline WeatherSlot slots[4];

    // parametri aggiuntivi (memorizzati, non ancora mostrati)
    inline time_t sunriseEpoch       = 0;     // current.sunrise (UTC epoch)
    inline time_t sunsetEpoch        = 0;     // current.sunset  (UTC epoch)
    inline float  dailyFeelsLikeMorn = NAN;   // daily[0].feels_like.morn (°C)
    inline float  dailyFeelsLikeEve  = NAN;   // daily[0].feels_like.eve  (°C)

    inline uint32_t lastForecastMs = 0;       // timestamp ultima chiamata One Call riuscita
    inline bool     firstRun       = true;    // forza un primo fetch appena possibile
    inline bool     needsRefresh   = false;
    inline bool     firstRenderDone = false;   // true dopo il primo renderFrame() riuscito

    // =======================================================================
    // Helper interni
    // =======================================================================

    /**
     * Sottrazione signed per confronto rollover-safe di millis().
     * @param now  valore attuale di millis()
     * @param last valore precedente
     * @param interval soglia in ms
     * @return true se sono trascorsi almeno `interval` ms
     * @since 20/04/26 Mattia Alesi
     */
    inline bool elapsed(uint32_t now, uint32_t last, uint32_t interval)
    {
      return (int32_t)(now - last) >= (int32_t)interval;
    }

    /**
     * Formatta un epoch UTC in "HH:MM" locale Europe/Rome (DST gestito
     * automaticamente dal TZ di sistema impostato da Calendar::initTimezone).
     * @param epoch epoch UTC (o 0 per "--:--")
     * @param out   buffer di almeno 6 byte
     * @modified 21/04/26 localtime_r al posto di gmtime_r(epoch+TZ_OFFSET)
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
      Serial.printf("[OWM] %-8s %s  %.1f C  icon=%s  \"%s\"\n",
                    tag, hhmm, s.feelsLikeC, s.iconCode, s.description);
    }

    /**
     * Esegue una GET HTTPS verso OpenWeatherMap e deserializza la risposta JSON
     * nel documento passato. Ritorna true solo se HTTP 200 e parsing ok.
     *
     * Nota: setInsecure() disabilita la validazione del certificato. Accettabile
     * per dati meteo pubblici; se in futuro si vuole pinning aggiungere root CA.
     */
    inline bool httpGetJson(const String& url, JsonDocument& doc)
    {
      WiFiClientSecure client;
      client.setInsecure();

      HTTPClient http;
      if (!http.begin(client, url))
      {
        Serial.println(F("[OWM] http.begin() failed"));
        return false;
      }
      int code = http.GET();
      if (code != 200)
      {
        Serial.printf("[OWM] fetch failed: http=%d\n", code);
        http.end();
        return false;
      }
      DeserializationError err = deserializeJson(doc, http.getStream());
      http.end();
      if (err)
      {
        Serial.printf("[OWM] json parse error: %s\n", err.c_str());
        return false;
      }
      return true;
    }

    /**
     * Variante di httpGetJson che applica un DeserializationOption::Filter
     * durante il parsing, cosi' che dal JSON ricevuto vengano conservati in
     * memoria solo i campi marcati `true` nel documento filter. Utile con
     * One Call 3.0 (payload ampio: current + hourly*48 + daily*8).
     */
    inline bool httpGetJsonFiltered(const String& url, JsonDocument& doc, const JsonDocument& filter)
    {
      WiFiClientSecure client;
      client.setInsecure();

      HTTPClient http;
      if (!http.begin(client, url))
      {
        Serial.println(F("[OWM] http.begin() failed"));
        return false;
      }
      int code = http.GET();
      if (code != 200)
      {
        Serial.printf("[OWM] fetch failed: http=%d\n", code);
        http.end();
        return false;
      }
      DeserializationError err = deserializeJson(doc, http.getStream(),
                                                 DeserializationOption::Filter(filter));
      http.end();
      if (err)
      {
        Serial.printf("[OWM] json parse error: %s\n", err.c_str());
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
     * Scarica meteo corrente + 3 previsioni orarie + dati giornalieri di oggi
     * in una singola chiamata all'endpoint One Call 3.0. Popola slots[0] da
     * `current`, slots[1..3] da hourly[3], hourly[6], hourly[9] (step 3h per
     * mantenere la cadenza della precedente /forecast?cnt=3).
     *
     * URL: exclude=minutely,alerts (scarta cio' che non serve; daily rimane
     * perche' ne leggiamo morn/eve). Il Filter ArduinoJson sotto restringe
     * ulteriormente i campi conservati in memoria.
     *
     * Oltre ai campi base memorizza alcuni parametri aggiuntivi (sunrise,
     * sunset, rain.1h previsto da hourly, pop, daily.feels_like.morn/eve)
     * non ancora mostrati.
     */
    inline bool fetchOneCall()
    {
      String url = String("https://api.openweathermap.org/data/3.0/onecall?lat=")
                 + String(LAT, 4) + "&lon=" + String(LON, 4)
                 + "&exclude=minutely,alerts"
                 + "&units=metric&lang=it&appid=" + OWM_API_KEY;

      // Filtro campi: memorizza solo quelli effettivamente utilizzati.
      JsonDocument filter;
      filter["current"]["dt"]                        = true;
      filter["current"]["feels_like"]                = true;
      filter["current"]["weather"][0]["icon"]        = true;
      filter["current"]["weather"][0]["description"] = true;
      filter["hourly"][0]["dt"]                        = true;
      filter["hourly"][0]["feels_like"]                = true;
      filter["hourly"][0]["weather"][0]["icon"]        = true;
      filter["hourly"][0]["weather"][0]["description"] = true;

      // parametri aggiuntivi
      filter["current"]["sunrise"]             = true;
      filter["current"]["sunset"]              = true;
      filter["hourly"][0]["pop"]               = true;
      filter["hourly"][0]["rain"]["1h"]        = true;
      filter["daily"][0]["feels_like"]["morn"] = true;
      filter["daily"][0]["feels_like"]["eve"]  = true;

      JsonDocument doc;
      if (!httpGetJsonFiltered(url, doc, filter)) return false;

      // slots[0] <- current
      JsonObjectConst cur = doc["current"].as<JsonObjectConst>();
      if (cur.isNull())
      {
        Serial.println(F("[OWM] onecall: current mancante"));
        return false;
      }
      WeatherSlot& s0 = slots[0];
      s0.feelsLikeC = cur["feels_like"] | NAN;
      copyStr(s0.iconCode,    sizeof(s0.iconCode),    cur["weather"][0]["icon"]        | "");
      copyStr(s0.description, sizeof(s0.description), cur["weather"][0]["description"] | "");
      s0.epoch = (time_t)(cur["dt"] | 0UL);
      if (s0.epoch == 0) s0.epoch = (time_t)(millis() / 1000);  // fallback: uptime
      s0.valid = !isnan(s0.feelsLikeC);

      // parametri aggiuntivi
      s0.precipitation_prob = 0.0f;                 // campo `pop` non presente in `current`
      // rain1h preso da hourly[0] (pioggia prevista per l'ora corrente),
      // non da current.rain (pioggia caduta nell'ultima ora).
      s0.rain1h = doc["hourly"][0]["rain"]["1h"] | NAN;
      sunriseEpoch = (time_t)(cur["sunrise"] | 0UL);
      sunsetEpoch  = (time_t)(cur["sunset"]  | 0UL);

      logSlot("current", s0);

      // slots[1..3] <- hourly[3], hourly[6], hourly[9]
      JsonArrayConst hourly = doc["hourly"].as<JsonArrayConst>();
      if (hourly.isNull())
      {
        Serial.println(F("[OWM] onecall: hourly mancante"));
        return s0.valid;
      }

      static const uint8_t offsets[3] = { 3, 6, 9 };
      for (int i = 0; i < 3; ++i)
      {
        JsonVariantConst item = hourly[offsets[i]];
        WeatherSlot& s = slots[1 + i];
        s.feelsLikeC = item["feels_like"] | NAN;
        copyStr(s.iconCode,    sizeof(s.iconCode),    item["weather"][0]["icon"]        | "");
        copyStr(s.description, sizeof(s.description), item["weather"][0]["description"] | "");
        s.epoch = (time_t)(item["dt"] | 0UL);
        s.valid = !isnan(s.feelsLikeC);

        // parametri aggiuntivi (memorizzati, non ancora mostrati)
        s.precipitation_prob = item["pop"]        | 0.0f;
        s.rain1h             = item["rain"]["1h"] | NAN;

        char tag[8];
        snprintf(tag, sizeof(tag), "fc[%d]", i);
        logSlot(tag, s);
      }

      // parametri aggiuntivi (memorizzati, non ancora mostrati)
      JsonObjectConst today = doc["daily"][0].as<JsonObjectConst>();
      if (!today.isNull())
      {
        dailyFeelsLikeMorn = today["feels_like"]["morn"] | NAN;
        dailyFeelsLikeEve  = today["feels_like"]["eve"]  | NAN;
      }

      return s0.valid;
    }

    // =======================================================================
    // Rendering
    // =======================================================================

    /**
     * Delega al .ino il disegno dell'immagine di background corrente.
     * Vedi drawTestBackground() in GxEPD2_1330c_GDEM133Z91.ino: il .ino
     * decide se mostrare l'immagine scaricata dal server cinema (se il
     * fetch ha avuto successo) o il fallback PROGMEM (se offline).
     */
    inline void drawBackground()
    {
      drawTestBackground();
    }

    /**
     * Disegna una stringa centrata orizzontalmente sulla colonna verticale
     * `centerX` con la baseline alla y specificata.
     */
    inline void drawCentered(const char* txt, int16_t centerX, int16_t baselineY, uint16_t color)
    {
      int16_t x1, y1;
      uint16_t w, h;
      display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
      display.setCursor(centerX - (int16_t)(w / 2) - x1, baselineY);
      display.setTextColor(color);
      display.print(txt);
    }

    /**
     * Disegna una temperatura nel formato "<num>°C". Il pallino ° e' un
     * cerchietto disegnato fra numero e "C" perche' i font FreeSans*pt7b
     * sono "7b" (ASCII 0x20..0x7E) e non contengono il glifo 0xB0.
     *
     * @param numStr     parte numerica gia' formattata (es. "22", "22.5", "-10.5").
     * @param anchorX    se centered=true e' il centro orizzontale del blocco
     *                   composito; se centered=false e' il bordo sinistro
     *                   del numero.
     * @param baselineY  baseline del testo (condivisa da numero e "C").
     * @param color      colore di numero, ° e "C".
     * @param degRadius  raggio del cerchietto ° (3 per Bold18pt, 2 per 18pt regular).
     * @param capHeight  cap-height approssimativa del font corrente, per
     *                   allineare ° all'apice di "C".
     * @param centered   true = centra il blocco su anchorX; false = parte
     *                   dal bordo sinistro anchorX.
     * @since 22/04/26
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
      // Centro verticale del cerchietto ~ all'altezza dell'apice di "C"
      // (baseline - capHeight + radius).
      display.drawCircle(x + degRadius, baselineY - capHeight + degRadius, degRadius, color);

      x += degW + gapAfter;
      display.setCursor(x - x1c, baselineY);
      display.print("C");
    }

    /**
     * Compone la stringa della riga description di un blocco meteo.
     * Quando la condizione di pioggia e' attiva (pop>0 o rain1h>0) la
     * description italiana viene sostituita o arricchita da valori
     * quantitativi (POP % e mm attesi nell'ultima ora):
     *   - slot 0 (current): OWM non fornisce pop per `current` quindi
     *     viene mantenuta la description e appeso " X.Xmm" solo se
     *     rain1h e' valorizzato.
     *   - slot 1..3 (forecast): la description viene sostituita da
     *     "<pop>% X.Xmm".
     * Altrimenti restituisce la description testuale invariata.
     *
     * @param slotIdx indice slot (0 = current, 1..3 = forecast).
     * @param s       slot meteo gia' popolato.
     * @param out     buffer di output.
     * @param outLen  dimensione del buffer out.
     * @since 22/04/26
     */
    inline void composeDescLine(uint8_t slotIdx, const WeatherSlot& s,
                                char* out, size_t outLen)
    {
      const bool hasRain = !isnan(s.rain1h) && s.rain1h > 0.0f;
      const bool hasPop  = s.precipitation_prob > 0.0f;

      if (!hasRain && !hasPop)
      {
        copyStr(out, outLen, s.description);
        return;
      }

      const float rain = hasRain ? s.rain1h : 0.0f;

      if (slotIdx == 0)
      {
        // current: description + " X.Xmm" solo se rain1h e' valorizzato.
        if (hasRain) snprintf(out, outLen, "%s %.1fmm", s.description, rain);
        else         copyStr(out, outLen, s.description);
      }
      else
      {
        // forecast: sostituzione con "POP% X.Xmm".
        snprintf(out, outLen, "%.0f%% %.1fmm",
                 s.precipitation_prob * 100.0f, rain);
      }
    }

    /**
     * Disegna un singolo blocco meteo del banner: icona, description,
     * temperatura percepita e orario. Tutti gli elementi sono centrati
     * sull'asse verticale del blocco (centerX = blockX + blockW/2).
     *
     * @param slotIdx  indice dello slot in `slots[]`: 0 = corrente, 1..3 = previsioni.
     * @param blockX   x (px) del margine sinistro del blocco.
     * @param blockW   larghezza (px) del blocco.
     * @param yOffset  shift verticale (px) applicato a tutte le baseline
     *                 del blocco. 0 per i forecast; per il current viene
     *                 passato un valore positivo per bilanciare lo spazio
     *                 verticale sopra/sotto dentro il riquadro Weather
     *                 (default 0 = comportamento invariato per forecast).
     * @modified 22/04/26 aggiunto yOffset per poter bilanciare il
     *                    blocco current senza muovere i forecast.
     */
    inline void renderBlock(uint8_t slotIdx, int16_t blockX, int16_t blockW,
                            int16_t yOffset = 0)
    {
      const int16_t centerX = blockX + blockW / 2;
      const WeatherSlot& s  = slots[slotIdx];

      // Icona centrata.
      const uint8_t* icon = iconFromCode(s.iconCode);
      display.drawBitmap(centerX - ICON_SIZE / 2, ICON_Y + yOffset, icon, ICON_SIZE, ICON_SIZE, GxEPD_BLACK);

      /** Description (nero, font tondo sans-serif). Se c'e' pioggia
       *  prevista (pop>0 o rain1h>0) la description testuale viene
       *  sostituita/arricchita con valori quantitativi — vedi
       *  composeDescLine(). */
      display.setFont(&FreeSans12pt7b);
      char descBuf[48];
      if (s.valid) composeDescLine(slotIdx, s, descBuf, sizeof(descBuf));
      else         strcpy(descBuf, "--");
      drawCentered(descBuf, centerX, DESC_BASELINE + yOffset, GxEPD_BLACK);

      /** Temperatura percepita (rosso, font tondo sans-serif grande bold).
       *  Modifica 22/04/26: composizione esplicita "<num>°C" con pallino
       *  disegnato come cerchietto (i font 7b non hanno il glifo °). */
      display.setFont(&FreeSansBold18pt7b);
      if (s.valid)
      {
        char numStr[8];
        snprintf(numStr, sizeof(numStr), "%.0f", s.feelsLikeC);
        // Bold18pt: raggio ° 3 px, cap-height ~22 px.
        drawTempWithDegree(numStr, centerX, TEMP_BASELINE + yOffset, GxEPD_RED, 3, 22, true);
      }
      else
      {
        drawCentered("--", centerX, TEMP_BASELINE + yOffset, GxEPD_RED);
      }

      // Orario (nero, font medio grande).
      char hhmm[6];
      formatHHMM(s.epoch, hhmm);
      display.setFont(&FreeSans18pt7b);
      drawCentered(hhmm, centerX, TIME_BASELINE + yOffset, GxEPD_BLACK);
    }

    /**
     * Disegna una riga della sub-colonna indoor: piccola icona + testo,
     * allineati a sinistra a partire da `startX`. Le 3 righe condividono
     * lo stesso startX cosi' che icone e dati risultino incolonnati.
     *
     * @param icon      bitmap 1bpp 20x20 PROGMEM (definiti in icons.h).
     * @param txt       stringa testo a destra dell'icona.
     * @param startX    x (px) del bordo sinistro dell'icona; il testo parte
     *                  a startX + INDOOR_ICON_SIZE + INDOOR_ICON_GAP.
     * @param baselineY baseline del testo (l'icona e' posizionata con il
     *                  bordo inferiore grosso modo allineato alla baseline).
     * @param color     colore del testo (icona sempre GxEPD_BLACK).
     * @since 21/04/26 Mattia Alesi
     */
    inline void drawIndoorRow(const uint8_t* icon, const char* txt,
                              int16_t startX, int16_t baselineY, uint16_t color)
    {
      // Icona allineata visivamente al testo: bordo inferiore ~6 px sotto la
      // baseline cosi' che il centro dell'icona coincida grosso modo col
      // centro delle x-height di FreeSans18pt7b (x-height ~14 px).
      const int16_t iconY = baselineY - INDOOR_ICON_SIZE + 6;
      display.drawBitmap(startX, iconY, icon, INDOOR_ICON_SIZE, INDOOR_ICON_SIZE, GxEPD_BLACK);

      // Testo a destra dell'icona; x1 compensa il bearing sinistro del font.
      int16_t  x1, y1;
      uint16_t tw, th;
      display.getTextBounds(txt, 0, 0, &x1, &y1, &tw, &th);
      display.setCursor(startX + INDOOR_ICON_SIZE + INDOOR_ICON_GAP - x1, baselineY);
      display.setTextColor(color);
      display.print(txt);
    }

    // =======================================================================
    // Barra temp-range (morn..eve) con indicatore current feels_like.
    //
    // Il canale giallo del pannello SOLUM 672x960 e' "out-of-band" rispetto
    // al template GxEPD2_3C (2 canali: black + red). Per disegnare giallo
    // usiamo le API del driver custom GxEPD2_097c_SOLUM_672x960:
    //   - writeImageYellow(bitmap, x, y, w, h, pgm)  -> cmd 0x28
    //   - preserveYellow(true)                        -> sopravvive al paged
    // Cifre dei numeri invece restano nere e sono disegnate normalmente con
    // drawXYZ dentro il loop paged.
    // @since 22/04/26
    // =======================================================================

    /** Dimensioni del buffer bitmap giallo della barra. Multiplo di 8 per
     *  byte-align (TRB_W/8 byte per riga, senza padding). */
    inline constexpr int16_t TRB_W              = 112;
    inline constexpr int16_t TRB_H              = 14;
    inline constexpr int16_t TRB_BYTES_PER_ROW  = TRB_W / 8;
    inline constexpr size_t  TRB_BUF_BYTES      = (size_t)TRB_BYTES_PER_ROW * TRB_H;

    /** Buffer 1bpp MSB-first per il canale 0x28 (bit=1 -> pixel giallo). */
    inline uint8_t trbYellowBuf[TRB_BUF_BYTES];

    /** Set pixel nel buffer giallo (no-op se fuori bounds). */
    inline void trbSetPx(int16_t x, int16_t y)
    {
      if (x < 0 || x >= TRB_W || y < 0 || y >= TRB_H) return;
      trbYellowBuf[y * TRB_BYTES_PER_ROW + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
    }

    /** Rettangolo pieno nel buffer giallo. */
    inline void trbFillRect(int16_t x, int16_t y, int16_t w, int16_t h)
    {
      for (int16_t yy = y; yy < y + h; ++yy)
        for (int16_t xx = x; xx < x + w; ++xx)
          trbSetPx(xx, yy);
    }

    /** Cerchio contorno (per il simbolo °). Bresenham midpoint. */
    inline void trbDrawCircle(int16_t cx, int16_t cy, int16_t r)
    {
      int16_t x = r, y = 0, err = 1 - x;
      while (y <= x)
      {
        trbSetPx(cx + x, cy + y); trbSetPx(cx + y, cy + x);
        trbSetPx(cx - x, cy + y); trbSetPx(cx - y, cy + x);
        trbSetPx(cx + x, cy - y); trbSetPx(cx + y, cy - x);
        trbSetPx(cx - x, cy - y); trbSetPx(cx - y, cy - x);
        y++;
        if (err < 0) err += 2*y + 1;
        else         { x--; err += 2*(y - x) + 1; }
      }
    }

    /** Triangolo isoscele punta-giu': base larga 2*halfBase in cima, vertice
     *  a (cx, y_top + height). */
    inline void trbFillTriangleDown(int16_t cx, int16_t y_top,
                                    int16_t halfBase, int16_t height)
    {
      for (int16_t dy = 0; dy < height; ++dy)
      {
        int16_t hw = halfBase - (halfBase * dy) / height;
        for (int16_t dx = -hw; dx <= hw; ++dx)
          trbSetPx(cx + dx, y_top + dy);
      }
      trbSetPx(cx, y_top + height);
    }

    /**
     * Centro orizzontale (display) delle sole cifre "HH:MM" della riga
     * sunset (esclude l'icona). Usato per centrare la barra temp-range
     * della Row 3 rispetto al testo dell'orario del tramonto sopra.
     *
     * Modifica 22/04/26 (v2): misura l'orario reale (sunsetEpoch) invece
     * del template "00:00" per gestire correttamente bearing variabili
     * delle cifre; aggiunto un piccolo offset -4 per bilanciare il peso
     * visivo asimmetrico delle label morn/eve della barra (che sono piu'
     * corte del testo "HH:MM" soprastante).
     * @since 22/04/26
     */
    inline int16_t sunsetRowCenterX(int16_t colX)
    {
      char hhmm[6];
      formatHHMM(sunsetEpoch, hhmm);   // es. "20:37"; "--:--" se epoch=0
      display.setFont(&FreeSans18pt7b);
      int16_t  x1, y1;
      uint16_t tw, th;
      display.getTextBounds(hhmm, 0, 0, &x1, &y1, &tw, &th);
      // Testo sunset inizia a colX + ICON + GAP, centro = start + tw/2,
      // meno un piccolo shift estetico verso sinistra.
      return colX + INDOOR_ICON_SIZE + INDOOR_ICON_GAP
             + (int16_t)tw / 2 - 4;
    }

    /**
     * Compila il buffer giallo della barra temp-range (barra + indicatore
     * triangolo) e lo scrive sul canale 0x28 del pannello via
     * writeImageYellow del driver custom. Setta preserveYellow(true)
     * cosi' il canale sopravvive al loop paged B+R.
     *
     * I cerchietti ° delle label sono disegnati in NERO dal paged
     * (drawTempRangeBarLabels), non nel buffer giallo.
     *
     * Va chiamata PRIMA di display.firstPage() in renderFrame().
     *
     * Layout nel buffer (TRB_W x TRB_H = 112x14):
     *   - barra: fillRect(barLeft, 5, barWidth, 4)  [4 px spessa, giallo]
     *   - triangolo: punta-giu' con vertice sulla barra (y=5)
     *
     * @param centerX centro orizzontale desiderato (in coord display) del
     *                punto medio della barra (allineato con il centro
     *                orizzontale della riga sunset sopra).
     * @param cellY   y assoluto sul display del pixel (0,0) del buffer.
     * @return true se la barra e' stata disegnata; false se dati non validi
     *         (in tal caso il canale 0x28 non viene toccato).
     * @since 22/04/26
     */
    inline bool drawTempRangeBarYellow(int16_t centerX, int16_t cellY)
    {
      if (!slots[0].valid) return false;
      const float morn = dailyFeelsLikeMorn;
      const float eve  = dailyFeelsLikeEve;
      const float cur  = slots[0].feelsLikeC;
      if (isnan(morn) || isnan(eve) || isnan(cur)) return false;

      memset(trbYellowBuf, 0, TRB_BUF_BYTES);

      // Misura larghezza delle label in FreeSans12pt7b (stessa logica di
      // drawTempRangeBarLabels cosi' i due rimangono allineati).
      // Modifica 22/04/26: font 9pt -> 12pt per le label della barra.
      display.setFont(&FreeSans12pt7b);
      char labL[8], labR[8];
      snprintf(labL, sizeof(labL), "%.0f", morn);
      snprintf(labR, sizeof(labR), "%.0f", eve);
      int16_t  x1, y1;
      uint16_t wL, hL, wR, hR;
      display.getTextBounds(labL, 0, 0, &x1, &y1, &wL, &hL);
      display.getTextBounds(labR, 0, 0, &x1, &y1, &wR, &hR);

      const int16_t PAD      = 2;
      const int16_t GAP      = 4;
      const int16_t DEG_GAP  = 2;
      const int16_t degR     = 2;
      const int16_t labSlotL = (int16_t)wL + DEG_GAP + 2*degR + 1;
      const int16_t labSlotR = (int16_t)wR + DEG_GAP + 2*degR + 1;

      const int16_t barLeft  = PAD + labSlotL + GAP;
      const int16_t barRight = TRB_W - PAD - labSlotR - GAP;
      const int16_t barWidth = barRight - barLeft;

      // Posizione assoluta: centra il punto medio della barra su centerX.
      const int16_t bufCenterX = (barLeft + barRight) / 2;
      const int16_t cellX      = centerX - bufCenterX;

      // Barra orizzontale gialla: spessa 4 px, occupa y=7..10 (centro 8.5).
      // Modifica 22/04/26 (v2): barY 6 -> 7 per far spazio al triangolo
      // ancora piu' grande (vertice sulla barra a y=7).
      const int16_t barY  = 7;
      const int16_t barTh = 4;
      trbFillRect(barLeft, barY, barWidth, barTh);

      // Indicatore triangolare punta-giu': base alta (y=0), vertice sulla
      // barra (y=7). Modifica 22/04/26 (v2): halfBase 4->5, height 6->7
      // per rendere la freccia ancora piu' visibile. Base 11 px, altezza 7.
      const float lo = fminf(morn, eve);
      const float hi = fmaxf(morn, eve);
      const float range = hi - lo;
      float ratio = (range < 0.5f) ? 0.5f : (cur - lo) / range;
      if (ratio < 0.0f) ratio = 0.0f;
      if (ratio > 1.0f) ratio = 1.0f;
      const int16_t cursorX = barLeft + (int16_t)(ratio * barWidth + 0.5f);
      trbFillTriangleDown(cursorX, /*y_top*/ 0, /*halfBase*/ 5, /*height*/ barY);

      // Scrive sul canale yellow e lo protegge durante paged.
      display.epd2.writeImageYellow(trbYellowBuf, cellX, cellY,
                                    TRB_W, TRB_H, /*pgm*/ false);
      display.epd2.preserveYellow(true);
      return true;
    }

    /**
     * Disegna le cifre nere (morn a sx, eve a dx) e i cerchietti ° neri
     * della barra temp-range. Va chiamata dentro il loop paged
     * (renderSunColumn) al posto del placeholder Row 3.
     *
     * Le decorazioni gialle (barra + triangolo) sono gia' sul canale 0x28
     * grazie a drawTempRangeBarYellow chiamata out-of-band prima di
     * firstPage().
     *
     * @param centerX    centro orizzontale (display) della barra — deve
     *                   coincidere con quello passato a drawTempRangeBarYellow.
     * @param baselineY  baseline delle cifre (= INDOOR_ROW3_BASELINE).
     * @since 22/04/26
     */
    inline void drawTempRangeBarLabels(int16_t centerX, int16_t baselineY)
    {
      if (!slots[0].valid) return;
      const float morn = dailyFeelsLikeMorn;
      const float eve  = dailyFeelsLikeEve;
      if (isnan(morn) || isnan(eve)) return;

      // Modifica 22/04/26: font 9pt -> 12pt per le label della barra.
      display.setFont(&FreeSans12pt7b);
      char labL[8], labR[8];
      snprintf(labL, sizeof(labL), "%.0f", morn);
      snprintf(labR, sizeof(labR), "%.0f", eve);
      int16_t  x1, y1;
      uint16_t wL, hL, wR, hR;
      display.getTextBounds(labL, 0, 0, &x1, &y1, &wL, &hL);
      display.getTextBounds(labR, 0, 0, &x1, &y1, &wR, &hR);

      const int16_t PAD      = 2;
      const int16_t GAP      = 4;
      const int16_t DEG_GAP  = 2;
      const int16_t degR     = 2;
      const int16_t labSlotL = (int16_t)wL + DEG_GAP + 2*degR + 1;
      const int16_t labSlotR = (int16_t)wR + DEG_GAP + 2*degR + 1;

      // Ricalcolo cellX (origine buffer giallo) con stessa logica di
      // drawTempRangeBarYellow: i due devono restare allineati.
      const int16_t barLeft    = PAD + labSlotL + GAP;
      const int16_t barRight   = TRB_W - PAD - labSlotR - GAP;
      const int16_t bufCenterX = (barLeft + barRight) / 2;
      const int16_t cellX      = centerX - bufCenterX;

      // Y del cerchietto ° nero: vicino all'apice del numero.
      const int16_t degY = baselineY - (int16_t)hL + degR + 1;

      display.setTextColor(GxEPD_BLACK);

      // Cifra sx + cerchietto ° nero.
      display.setCursor(cellX + PAD - x1, baselineY);
      display.print(labL);
      const int16_t degXL = cellX + PAD + (int16_t)wL + DEG_GAP + degR;
      display.drawCircle(degXL, degY, degR, GxEPD_BLACK);

      // Cifra dx + cerchietto ° nero.
      const int16_t rightLabX = cellX + TRB_W - PAD - labSlotR;
      display.setCursor(rightLabX - x1, baselineY);
      display.print(labR);
      const int16_t degXR = rightLabX + (int16_t)wR + DEG_GAP + degR;
      display.drawCircle(degXR, degY, degR, GxEPD_BLACK);
    }

    /**
     * Disegna il contenuto del riquadro Indoor: 1 colonna x 4 righe
     * con i dati del sensore BME680.
     *   1. temperatura   (rossa, "%.1f°C")
     *   2. umidita'      (nera,  "%.0f %%")
     *   3. qualita' aria (nera,  label + IAQ + pedice accuracy)
     *   4. pressione     (nera,  "%.0f hPa")
     *
     * Il titolo "Indoor" e' disegnato dal chiamante in stile fieldset sul
     * bordo superiore del riquadro (via Graphics::drawFieldsetRect).
     *
     * @param rrX  x (px) del bordo sinistro del riquadro Indoor.
     * @param rrW  larghezza (px) del riquadro Indoor (non usata).
     *
     * @since 22/04/26
     * @modified 22/04/26 (v3) ridotto a 1 colonna; colonna DX con
     *                    sunrise/sunset/placeholder spostata nel
     *                    riquadro Weather (vedi renderSunColumn).
     */
    inline void renderIndoorBox(int16_t rrX, int16_t /*rrW*/)
    {
      const Indoor::Sample& s = Indoor::sample();
      const int16_t col1X = rrX + INDOOR_COL1_OFFSET;

      display.setFont(&FreeSans18pt7b);
      char buf[16];

      /** Riga 1 - temperatura (rossa) con "°" disegnato come cerchietto. */
      {
        const int16_t iconY = INDOOR_ROW1_BASELINE - INDOOR_ICON_SIZE + 6;
        display.drawBitmap(col1X, iconY, INDOOR_ICON_TEMPERATURE,
                           INDOOR_ICON_SIZE, INDOOR_ICON_SIZE, GxEPD_BLACK);

        const int16_t textStartX = col1X + INDOOR_ICON_SIZE + INDOOR_ICON_GAP;
        if (s.valid)
        {
          snprintf(buf, sizeof(buf), "%.1f", s.temperature);
          drawTempWithDegree(buf, textStartX, INDOOR_ROW1_BASELINE, GxEPD_RED, 2, 20, false);
        }
        else
        {
          int16_t  x1, y1;
          uint16_t tw, th;
          display.getTextBounds("--", 0, 0, &x1, &y1, &tw, &th);
          display.setCursor(textStartX - x1, INDOOR_ROW1_BASELINE);
          display.setTextColor(GxEPD_RED);
          display.print("--");
        }
      }

      /** Riga 2 - umidita' (nera). */
      if (s.valid) snprintf(buf, sizeof(buf), "%.0f %%", s.humidity);
      else         strcpy(buf, "--");
      drawIndoorRow(INDOOR_ICON_HUMIDITY, buf, col1X, INDOOR_ROW2_BASELINE, GxEPD_BLACK);

      /** Riga 3 - qualita' aria: label + IAQ in FreeSans9pt7b + pedice
       *  accuracy in Picopixel. */
      if (s.valid)
      {
        char mainStr[24];
        snprintf(mainStr, sizeof(mainStr), "%s %.0f",
                 Indoor::iaqLabel(s.iaq), s.iaq);

        display.setFont(&FreeSans9pt7b);
        drawIndoorRow(INDOOR_ICON_AIR_QUALITY, mainStr, col1X, INDOOR_ROW3_BASELINE, GxEPD_BLACK);

        int16_t  x1, y1;
        uint16_t mw, mh;
        display.getTextBounds(mainStr, 0, 0, &x1, &y1, &mw, &mh);
        const int16_t subX = col1X + INDOOR_ICON_SIZE + INDOOR_ICON_GAP + (int16_t)mw + 2;

        char accStr[4];
        snprintf(accStr, sizeof(accStr), "%u", (unsigned)s.iaqAccuracy);
        display.setFont(&Picopixel);
        display.setCursor(subX, INDOOR_ROW3_BASELINE + 2);
        display.setTextColor(GxEPD_BLACK);
        display.print(accStr);

        display.setFont(&FreeSans18pt7b);
      }
      else
      {
        display.setFont(&FreeSans9pt7b);
        drawIndoorRow(INDOOR_ICON_AIR_QUALITY, "--", col1X, INDOOR_ROW3_BASELINE, GxEPD_BLACK);
        display.setFont(&FreeSans18pt7b);
      }

      /** Riga 4 - pressione atmosferica (nera, "XXXX hPa"). */
      if (s.valid) snprintf(buf, sizeof(buf), "%.0f hPa", s.pressure);
      else         strcpy(buf, "--");
      drawIndoorRow(INDOOR_ICON_PRESSURE, buf, col1X, INDOOR_ROW4_BASELINE, GxEPD_BLACK);
    }

    /**
     * Disegna la sub-colonna sun nel riquadro Weather, a destra del
     * blocco meteo corrente. 4 righe allineate verticalmente alle 4
     * righe del riquadro Indoor:
     *   1. sunrise     (nera, "HH:MM")
     *   2. sunset      (nera, "HH:MM")
     *   3. temp_range      (riservata, non disegnata)
     *   4. additional_range (riservata, non disegnata)
     *
     * @param weatherRrX  x (px) del bordo sinistro del riquadro Weather.
     * @since 22/04/26
     */
    inline void renderSunColumn(int16_t weatherRrX)
    {
      const int16_t colX = weatherRrX + SUN_COL_OFFSET;
      display.setFont(&FreeSans18pt7b);

      char hhmm[6];
      formatHHMM(sunriseEpoch, hhmm);
      drawIndoorRow(INDOOR_ICON_SUNRISE, hhmm, colX, INDOOR_ROW1_BASELINE, GxEPD_BLACK);

      formatHHMM(sunsetEpoch, hhmm);
      drawIndoorRow(INDOOR_ICON_SUNSET, hhmm, colX, INDOOR_ROW2_BASELINE, GxEPD_BLACK);

      /** Row 3: cifre + cerchietti ° (nero) della barra temp-range.
       *  Le decorazioni gialle (barra + triangolo) sono gia' sul canale
       *  0x28 grazie a drawTempRangeBarYellow chiamata prima di
       *  firstPage(). Il centro orizzontale e' allineato al centro della
       *  riga sunset sopra. */
      drawTempRangeBarLabels(sunsetRowCenterX(colX), INDOOR_ROW3_BASELINE);

      /** Row 4 riservata (additional_range): non disegnata. */
    }

    /**
     * Disegna il banner completo. Sfondo bianco + 3 riquadri arrotondati
     * in stile "fieldset" (titolo sul bordo superiore):
     *   - Indoor   : BME680 + sunrise/sunset (2 colonne x 4 righe)
     *   - Weather  : meteo corrente (1 slot)
     *   - Forecast : 3 previsioni future
     *
     * @modified 22/04/26 Indoor estratto in riquadro dedicato, titoli
     *                    fieldset, raggio bordi aumentato a 18.
     */
    inline void drawBanner()
    {
      // Sfondo banner bianco: cancella eventuale contenuto del background sotto.
      display.fillRect(0, BANNER_Y, BANNER_W, BANNER_H, GxEPD_WHITE);

      constexpr int16_t RR_INSET_Y = 5;
      constexpr int16_t RR_RADIUS  = 18;   // era 12, aumentato per bordi piu' morbidi
      constexpr int16_t TITLE_LEFT_OFFSET = 14;
      const int16_t rrTop    = BANNER_Y + RR_INSET_Y;
      const int16_t rrHeight = BANNER_H - 2 * RR_INSET_Y;

      // Titoli fieldset in FreeSans12pt7b (font impostato prima della
      // chiamata: drawFieldsetRect non setta font).
      display.setFont(&FreeSans12pt7b);

      Graphics::drawFieldsetRect(INDOOR_RR_X,   rrTop, INDOOR_RR_W,   rrHeight,
                                 RR_RADIUS, "Indoor",
                                 TITLE_LEFT_OFFSET, GxEPD_WHITE);
      Graphics::drawFieldsetRect(WEATHER_RR_X,  rrTop, WEATHER_RR_W,  rrHeight,
                                 RR_RADIUS, "Weather",
                                 TITLE_LEFT_OFFSET, GxEPD_WHITE);
      Graphics::drawFieldsetRect(FORECAST_RR_X, rrTop, FORECAST_RR_W, rrHeight,
                                 RR_RADIUS, "Forecast",
                                 TITLE_LEFT_OFFSET, GxEPD_WHITE);

      // Contenuti dei riquadri. Tutti i renderBlock usano yOffset
      // default 0 (baseline originali ICON_Y/DESC/TEMP/TIME invariate
      // sia per current che per forecast).
      renderIndoorBox(INDOOR_RR_X, INDOOR_RR_W);
      renderBlock(0, BLOCK_CURRENT_X, BLOCK_CURRENT_W);
      renderSunColumn(WEATHER_RR_X);
      renderBlock(1, BLOCK_FC0_X, BLOCK_FC_W);
      renderBlock(2, BLOCK_FC1_X, BLOCK_FC_W);
      renderBlock(3, BLOCK_FC2_X, BLOCK_FC_W);
    }

    /**
     * Compone un frame completo (background + banner) sul display usando il
     * pattern paged di GxEPD2. Ogni refresh costa ~22 s (il pannello non supporta
     * refresh parziale); il flag needsRefresh fa da debouncer per evitare refresh
     * ridondanti quando piu' eventi scattano ravvicinati.
     */
    inline void renderFrame()
    {
      display.setFullWindow();
      /**
       * Epoch di riferimento per il calendario: se il meteo corrente e' valido
       * (modifica 20/04/26: ripreso da slots[0] per garantire data reale),
       * lo usiamo come "ora"; altrimenti Calendar::draw ripieghera' su time().
       */
      time_t calEpoch = slots[0].valid ? slots[0].epoch : 0;

      /**
       * Pre-write del canale yellow (0x28) per la barra temp-range della
       * Row 3 nella sub-col sun: deve avvenire PRIMA di firstPage() e
       * setta preserveYellow(true) cosi' il canale sopravvive al loop
       * paged che gestisce solo black+red. Il driver custom auto-resetta
       * preserveYellow alla fine di refresh().
       * @since 22/04/26
       */
      {
        const int16_t colX       = WEATHER_RR_X + SUN_COL_OFFSET;
        const int16_t centerX    = sunsetRowCenterX(colX);   // allineato alla riga sunset sopra
        const int16_t trbCellY   = INDOOR_ROW3_BASELINE - 12;  // buffer alto 14, barra a y=5..8
        drawTempRangeBarYellow(centerX, trbCellY);
      }

      display.firstPage();
      do
      {
        display.fillScreen(GxEPD_WHITE);
        drawBackground();
        /**
         * Colonna placeholder bianca a destra (25%): copre l'immagine e
         * riserva lo spazio per elementi che verranno aggiunti piu' avanti.
         */
        display.fillRect(SIDEBAR_X, 0, SIDEBAR_W, BANNER_Y, GxEPD_WHITE);
        /**
         * Calendar::draw riempie di bianco il proprio riquadro: riduce di fatto
         * l'area visibile dell'immagine di background. Il riquadro calendario
         * e' posizionato subito a sinistra della sidebar. Il TZ applicato e'
         * quello impostato da Calendar::initTimezone() in setup().
         */
        Calendar::draw(calEpoch);
        drawBanner();
      } while (display.nextPage());
      // Nota: preserveYellow(false) viene gestito automaticamente dentro
      // refresh() del driver custom (chiamato dal template alla fine del
      // paged loop), quindi non serve resettarlo qui a mano.
    }
  } // namespace detail

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
      s.iconCode[0] = '\0';
      s.description[0] = '\0';
      // parametri aggiuntivi (memorizzati, non ancora mostrati)
      s.precipitation_prob = 0.0f;
      s.rain1h             = NAN;
    }
    // parametri aggiuntivi (memorizzati, non ancora mostrati)
    sunriseEpoch       = 0;
    sunsetEpoch        = 0;
    dailyFeelsLikeMorn = NAN;
    dailyFeelsLikeEve  = NAN;

    lastForecastMs = 0;
    firstRun       = true;
    // Primo refresh posticipato fino all'arrivo di meteo corrente + almeno
    // una previsione futura: evita il refresh sprecato (~22 s) col banner
    // a placeholder "--" al boot.
    needsRefresh    = false;
    firstRenderDone = false;
  }

  /**
   * Ritorna FETCH_BOTH se e' scaduto INTERVAL_FORECAST (pilotato da
   * WEATHER_FORECAST_FETCH_MIN), altrimenti FETCH_NONE. Funzione pura:
   * non modifica stato e non tocca il WiFi. Usato da loop() nel .ino
   * per decidere se accendere il WiFi.
   * One Call 3.0 restituisce corrente + previsioni in un unica chiamata,
   * quindi un solo timer governa entrambi i bit di FetchKind.
   */
  inline FetchKind pendingFetch()
  {
    using namespace detail;
    if (firstRun || elapsed(millis(), lastForecastMs, INTERVAL_FORECAST))
      return FETCH_BOTH;
    return FETCH_NONE;
  }

  /**
   * Esegue il fetch indicato. Presuppone che il WiFi sia gia' connesso
   * (WL_CONNECTED); non accende ne' spegne la radio. In caso di successo
   * aggiorna entrambi i timer (current + forecast) perche' One Call 3.0
   * restituisce i due gruppi di dati in un'unica risposta, e marca il
   * banner come "da ridisegnare". Ritorna true se la richiesta e' andata
   * a buon fine.
   */
  inline bool runFetch(FetchKind kind)
  {
    using namespace detail;
    if (kind == FETCH_NONE) return false;

    bool ok = fetchOneCall();
    if (ok)
    {
      lastForecastMs = millis();
      needsRefresh   = true;
    }
    /**
     * firstRun resta true finche' NON abbiamo sia meteo corrente che almeno
     * una previsione valida: cosi' pendingFetch() continua a ritornare
     * FETCH_BOTH e ripete al giro successivo senza dover attendere l'intero
     * INTERVAL_*. La retry resta comunque throttled dal timeout HTTP.
     */
    if (slots[0].valid && slots[1].valid) firstRun = false;
    return ok;
  }

  /**
   * Se serve, ridisegna lo schermo (background + banner). Nessuna rete.
   * Va chiamata ad ogni giro di loop().
   */
  inline void render()
  {
    using namespace detail;

    if (needsRefresh)
    {
      /**
       * Blocca il primo refresh finche' non arrivano meteo corrente (slots[0])
       * e almeno la prima previsione futura (slots[1]): evita un refresh
       * sprecato (~22 s) col banner a "--" al boot. Una volta disegnato il
       * primo frame i refresh successivi (rotazione bg, markDirty, ecc.)
       * proseguono anche se un dato torna transitoriamente invalido.
       */
      if (!firstRenderDone && (!slots[0].valid || !slots[1].valid))
        return;

      renderFrame();
      needsRefresh    = false;
      firstRenderDone = true;
    }
  }

  /**
   * Forza il refresh del display al prossimo giro di render(). Usato dai
   * moduli esterni (es. Indoor) per notificare l'arrivo di nuovi dati
   * da ridisegnare senza accoppiamento diretto al flag interno.
   * @since 21/04/26 Mattia Alesi
   */
  inline void markDirty()
  {
    detail::needsRefresh = true;
  }

  /**
   * Sblocca il gate del primo refresh anche se i dati meteo non sono ancora
   * arrivati (es. timeout WiFi, API OWM irraggiungibile). Il banner verra'
   * disegnato con i placeholder "--" al posto dei valori mancanti.
   * No-op se il primo refresh e' gia' stato eseguito: chiamate ripetute da
   * tentativi di connessione successivi non producono refresh inutili.
   */
  inline void forceFirstRender()
  {
    using namespace detail;
    if (firstRenderDone) return;
    needsRefresh    = true;
    firstRenderDone = true;
  }
}

#endif

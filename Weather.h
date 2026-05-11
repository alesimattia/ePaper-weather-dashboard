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
#include "icons.h"
#include "Graphics.h"
#include "Calendar.h"
#include "Indoor.h"

// ---------------------------------------------------------------------------
// Istanza del display (definita nello sketch .ino). Il tipo concreto del
// pannello (Layout::Panel) e' scelto dal dispatcher Layout.h in base al
// #define DISPLAY_VARIANT_* nello sketch.
// ---------------------------------------------------------------------------
extern GxEPD2_3C<Layout::Panel, Layout::Panel::HEIGHT / 8> display;

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
 * Il WiFi NON è gestito qui: runFetch() presuppone che sia gia' connesso
 * (lo sketch .ino si occupa di wifiOn()/wifiOff() immediatamente intorno
 * alla chiamata). Cosi' la radio resta spenta fra un fetch e l'altro.
 *
 * Credenziali WiFi e API key OWM sono in Env.h. Posizione GPS (LAT/LON)
 * pure in Env.h. Il fuso orario è gestito da Calendar::initTimezone()
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
    // Layout: tutte le costanti pixel (sidebar, banner, fieldset, baseline,
    // sub-col indoor/sun, slot forecast) vivono in Layout.h e sono accedute
    // come Layout::* nel rendering. La cascata banner -> baseline (ROW1..4,
    // ICON_Y, DESC/TEMP/TIME) e' calcolata dentro il Layout in funzione
    // di BANNER_Y, cosi' lo scaling al pannello 122c (BANNER_Y +96) si
    // propaga senza altre modifiche.
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
     * perchè ne leggiamo morn/eve). Il Filter ArduinoJson sotto restringe
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
     * Vedi drawTestBackground() in ePaper-weather-dashboard.ino: il .ino
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
     * Quando la condizione di pioggia è attiva (pop>0 o rain1h>0) la
     * description italiana viene sostituita o arricchita da valori
     * quantitativi (POP % e mm attesi nell'ultima ora):
     *   - slot 0 (current): OWM non fornisce pop per `current` quindi
     *     viene mantenuta la description e appeso " X.Xmm" solo se
     *     rain1h è valorizzato.
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
        // current: description + " X.Xmm" solo se rain1h è valorizzato.
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
     */
    inline void renderBlock(uint8_t slotIdx, int16_t blockX, int16_t blockW)
    {
      const int16_t centerX = blockX + blockW / 2;
      const WeatherSlot& s  = slots[slotIdx];

      // Icona centrata.
      const uint8_t* icon = iconFromCode(s.iconCode);
      display.drawBitmap(centerX - Layout::ICON_SIZE / 2, Layout::ICON_Y, icon,
                         Layout::ICON_SIZE, Layout::ICON_SIZE, GxEPD_BLACK);

      /** Description (nero, font tondo sans-serif). Se c'è pioggia
       *  prevista (pop>0 o rain1h>0) la description testuale viene
       *  sostituita/arricchita con valori quantitativi — vedi
       *  composeDescLine(). */
      display.setFont(Layout::FONT_BODY);
      char descBuf[48];
      if (s.valid) composeDescLine(slotIdx, s, descBuf, sizeof(descBuf));
      else         strcpy(descBuf, "--");
      drawCentered(descBuf, centerX, Layout::DESC_BASELINE, GxEPD_BLACK);

      /** Temperatura percepita (rosso, font tondo sans-serif grande bold).
       *  Composizione esplicita "<num>°C" con pallino disegnato come
       *  cerchietto (i font 7b non hanno il glifo °). */
      display.setFont(Layout::FONT_LARGE_BOLD);
      if (s.valid)
      {
        char numStr[8];
        snprintf(numStr, sizeof(numStr), "%.0f", s.feelsLikeC);
        // Bold18pt: raggio ° 3 px, cap-height effettiva ~18 px (era 22:
        // sovrastimata, faceva galleggiare il cerchietto sopra l'apice di "C").
        drawTempWithDegree(numStr, centerX, Layout::TEMP_BASELINE, GxEPD_RED, 3, 18, true);
      }
      else
      {
        drawCentered("--", centerX, Layout::TEMP_BASELINE, GxEPD_RED);
      }

      // Orario: font selezionato dal Layout (FONT_LARGE sul 097c, FONT_BODY
      // sul 122c per uniformare al font degli eventi calendario).
      char hhmm[6];
      formatHHMM(s.epoch, hhmm);
      display.setFont(Layout::FONT_TIME);
      drawCentered(hhmm, centerX, Layout::TIME_BASELINE, GxEPD_BLACK);
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
     * @param baselineY baseline del testo (l'icona è posizionata con il
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
      const int16_t iconY = baselineY - Layout::INDOOR_ICON_SIZE + 6;
      display.drawBitmap(startX, iconY, icon, Layout::INDOOR_ICON_SIZE, Layout::INDOOR_ICON_SIZE, GxEPD_BLACK);

      // Testo a destra dell'icona; x1 compensa il bearing sinistro del font.
      int16_t  x1, y1;
      uint16_t tw, th;
      display.getTextBounds(txt, 0, 0, &x1, &y1, &tw, &th);
      display.setCursor(startX + Layout::INDOOR_ICON_SIZE + Layout::INDOOR_ICON_GAP - x1, baselineY);
      display.setTextColor(color);
      display.print(txt);
    }

    /**
     * Variante centrata di drawIndoorRow: il blocco "icona + gap + testo"
     * viene centrato orizzontalmente su `centerX`. Usata dalle righe
     * sunrise/sunset della sub-col sun (le righe Indoor restano invece
     * incolonnate a sinistra).
     *
     * @param icon      bitmap 1bpp 20x20 PROGMEM.
     * @param txt       stringa testo a destra dell'icona.
     * @param centerX   centro orizzontale (display) del blocco icona+testo.
     * @param baselineY baseline del testo (icona allineata come in drawIndoorRow).
     * @param color     colore del testo (icona sempre GxEPD_BLACK).
     */
    inline void drawSunRow(const uint8_t* icon, const char* txt,
                           int16_t centerX, int16_t baselineY, uint16_t color)
    {
      int16_t  x1, y1;
      uint16_t tw, th;
      display.getTextBounds(txt, 0, 0, &x1, &y1, &tw, &th);
      const int16_t totalW = Layout::INDOOR_ICON_SIZE + Layout::INDOOR_ICON_GAP + (int16_t)tw;
      const int16_t startX = centerX - totalW / 2;

      const int16_t iconY = baselineY - Layout::INDOOR_ICON_SIZE + 6;
      display.drawBitmap(startX, iconY, icon, Layout::INDOOR_ICON_SIZE, Layout::INDOOR_ICON_SIZE, GxEPD_BLACK);

      display.setCursor(startX + Layout::INDOOR_ICON_SIZE + Layout::INDOOR_ICON_GAP - x1, baselineY);
      display.setTextColor(color);
      display.print(txt);
    }

    // =======================================================================
    // @widget slider-temp-range
    //
    // Barra giallo orizzontale con indicatore triangolo che mostra dove
    // cade la temperatura percepita corrente fra morn ed eve (Row 3 della
    // sub-col sun). Disegnata in 2 fasi:
    //   1. drawTempRangeBarYellow(): barra + triangolo sul canale yellow
    //      (cmd 0x28, out-of-band rispetto al paged loop B+R).
    //   2. drawTempRangeBarLabels(): cifre nere e cerchietti ° nel paged.
    //
    // Il canale giallo del pannello SOLUM 672x960 è "out-of-band" rispetto
    // al template GxEPD2_3C (2 canali: black + red). Per disegnare giallo
    // usiamo le API del driver custom (Layout::Panel):
    //   - writeImageYellow(bitmap, x, y, w, h, pgm)  -> cmd 0x28
    //   - preserveYellow(true)                        -> sopravvive al paged
    // @since 22/04/26
    // =======================================================================

    /** Dimensioni del buffer bitmap giallo della barra: vedi Layout::TRB_*. */
    inline constexpr size_t  TRB_BUF_BYTES = (size_t)Layout::TRB_BYTES_PER_ROW * Layout::TRB_H;

    /** Buffer 1bpp MSB-first per il canale 0x28 (bit=1 -> pixel giallo). */
    inline uint8_t trbYellowBuf[TRB_BUF_BYTES];

    /** Set pixel nel buffer giallo (no-op se fuori bounds). */
    inline void trbSetPx(int16_t x, int16_t y)
    {
      if (x < 0 || x >= Layout::TRB_W || y < 0 || y >= Layout::TRB_H) return;
      trbYellowBuf[y * Layout::TRB_BYTES_PER_ROW + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
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
     * @return true se la barra è stata disegnata; false se dati non validi
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

      // Misura larghezza delle label in FONT_BODY (stessa logica di
      // drawTempRangeBarLabels cosi' i due rimangono allineati).
      // Modifica 22/04/26: font 9pt -> 12pt per le label della barra.
      display.setFont(Layout::FONT_BODY);
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
      const int16_t barRight = Layout::TRB_W - PAD - labSlotR - GAP;
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
                                    Layout::TRB_W, Layout::TRB_H, /*pgm*/ false);
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
      display.setFont(Layout::FONT_BODY);
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
      const int16_t barRight   = Layout::TRB_W - PAD - labSlotR - GAP;
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
      const int16_t rightLabX = cellX + Layout::TRB_W - PAD - labSlotR;
      display.setCursor(rightLabX - x1, baselineY);
      display.print(labR);
      const int16_t degXR = rightLabX + (int16_t)wR + DEG_GAP + degR;
      display.drawCircle(degXR, degY, degR, GxEPD_BLACK);
    }

    // =======================================================================
    // @widget storico-temperature
    //
    // Mini-chart andamento temperatura percepita esterna sotto la slider
    // temp-range (Row 4 della sub-col sun).
    //
    // Range temporale: da (now - 3h) a slots[3].epoch (~now + 9h).
    // Tratto passato in nero, tratto previsione in rosso, pallino "ora" nero.
    // Disegnato interamente con canale B+R dentro il paged loop: nessuna
    // interferenza con il canale giallo della slider sovrastante.
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
     * a partire da `topY`. Tutte le decorazioni (curva, asse X, tick,
     * label HH:MM, label min/max, pallino) vengono disegnate sul canale
     * nero+rosso del display dentro il paged loop.
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
       *  Colore: nero se il tempo del campione e' < now, rosso altrimenti.
       *  Il cambio avviene esattamente nel pixel corrispondente a "now". */
      if (n >= 2)
      {
        for (size_t i = 0; i < n - 1; ++i)
        {
          const TempSample& p1 = pts[i];
          const TempSample& p2 = pts[i + 1];
          const TempSample& p0 = (i == 0)         ? p1 : pts[i - 1];
          const TempSample& p3 = (i + 2 >= n)     ? p2 : pts[i + 2];

          const int16_t x1 = mapX(p1.epoch);
          const int16_t x2 = mapX(p2.epoch);
          int16_t steps = x2 - x1;
          if (steps < 1) steps = 1;

          int16_t prevX = x1;
          int16_t prevY = mapY(p1.t);
          for (int16_t s = 1; s <= steps; ++s)
          {
            const float   u  = (float)s / (float)steps;
            const time_t  eu = p1.epoch + (time_t)((double)(p2.epoch - p1.epoch) * (double)u);
            const float   tu = catmull(p0.t, p1.t, p2.t, p3.t, u);
            const int16_t cx = x1 + (int16_t)(((long)(x2 - x1) * (long)s) / (long)steps);
            const int16_t cy = mapY(tu);
            const uint16_t color = (eu < now) ? GxEPD_BLACK : GxEPD_RED;
            display.drawLine(prevX, prevY, cx, cy, color);
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

      /** Label HH:MM (FONT_MICRO) sotto i tick, con clamp ai bordi del chart. */
      display.setFont(Layout::FONT_MICRO);
      display.setTextColor(GxEPD_BLACK);
      const int16_t labY = axisY + Layout::TC_AXIS_PAD_BOT - 1;
      auto drawHHMM = [&](time_t e, int16_t tickX, bool clampLeft, bool clampRight)
      {
        char buf[6];
        formatHHMM(e, buf);
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
      drawHHMM(e0,   chartL,     true,  false);
      drawHHMM(eMid, xMid,       false, false);
      drawHHMM(e1,   chartR - 1, false, true);

      /** Label min/max (FONT_MICRO) a sinistra del chart in colonna. */
      char numBuf[6];
      const int16_t labLeftX = centerX - Layout::TC_W / 2;
      snprintf(numBuf, sizeof(numBuf), "%.0f", tmax);
      display.setCursor(labLeftX, plotTop + 4);
      display.print(numBuf);
      snprintf(numBuf, sizeof(numBuf), "%.0f", tmin);
      display.setCursor(labLeftX, axisY - 1);
      display.print(numBuf);

      /** Pallino sulla temperatura corrente (sopra la curva). */
      display.fillCircle(xNow, mapY(slots[0].feelsLikeC), Layout::TC_DOT_R, GxEPD_BLACK);
    }

    /**
     * Disegna il contenuto del riquadro Indoor: 1 colonna x 4 righe
     * con i dati del sensore BME680.
     *   1. temperatura   (rossa, "%.1f°C")
     *   2. umidita'      (nera,  "%.0f %%")
     *   3. qualita' aria (nera,  label + IAQ + pedice accuracy)
     *   4. pressione     (nera,  "%.0f hPa")
     *
     * Il titolo "Indoor" è disegnato dal chiamante in stile fieldset sul
     * bordo superiore del riquadro (via Graphics::drawFieldsetRect).
     *
     * @param rrX  x (px) del bordo sinistro del riquadro Indoor.
     *
     * @since 22/04/26
     * @modified 22/04/26 (v3) ridotto a 1 colonna; colonna DX con
     *                    sunrise/sunset/placeholder spostata nel
     *                    riquadro Weather (vedi renderSunColumn).
     */
    inline void renderIndoorBox(int16_t rrX)
    {
      const Indoor::Sample& s = Indoor::sample();
      const int16_t col1X = rrX + Layout::INDOOR_COL1_OFFSET;

      display.setFont(Layout::FONT_LARGE);
      char buf[16];

      /** Riga 1 - temperatura (rossa) con "°" disegnato come cerchietto. */
      {
        const int16_t iconY = Layout::INDOOR_ROW1_BASELINE - Layout::INDOOR_ICON_SIZE + 6;
        display.drawBitmap(col1X, iconY, INDOOR_ICON_TEMPERATURE,
                           Layout::INDOOR_ICON_SIZE, Layout::INDOOR_ICON_SIZE, GxEPD_BLACK);

        const int16_t textStartX = col1X + Layout::INDOOR_ICON_SIZE + Layout::INDOOR_ICON_GAP;
        if (s.valid)
        {
          snprintf(buf, sizeof(buf), "%.1f", s.temperature);
          // FreeSans18pt7b: raggio ° 2 px, cap-height effettiva ~16 px (era 20:
          // sovrastimata, faceva galleggiare il cerchietto sopra l'apice di "C").
          drawTempWithDegree(buf, textStartX, Layout::INDOOR_ROW1_BASELINE, GxEPD_RED, 2, 16, false);
        }
        else
        {
          int16_t  x1, y1;
          uint16_t tw, th;
          display.getTextBounds("--", 0, 0, &x1, &y1, &tw, &th);
          display.setCursor(textStartX - x1, Layout::INDOOR_ROW1_BASELINE);
          display.setTextColor(GxEPD_RED);
          display.print("--");
        }
      }

      /** Riga 2 - umidita' (nera). */
      if (s.valid) snprintf(buf, sizeof(buf), "%.0f %%", s.humidity);
      else         strcpy(buf, "--");
      drawIndoorRow(INDOOR_ICON_HUMIDITY, buf, col1X, Layout::INDOOR_ROW2_BASELINE, GxEPD_BLACK);

      /** Riga 3 - qualita' aria: label + IAQ in FONT_SMALL + pedice
       *  accuracy in FONT_MICRO. */
      if (s.valid)
      {
        char mainStr[24];
        snprintf(mainStr, sizeof(mainStr), "%s %.0f",
                 Indoor::iaqLabel(s.iaq), s.iaq);

        display.setFont(Layout::FONT_SMALL);
        drawIndoorRow(INDOOR_ICON_AIR_QUALITY, mainStr, col1X, Layout::INDOOR_ROW3_BASELINE, GxEPD_BLACK);

        int16_t  x1, y1;
        uint16_t mw, mh;
        display.getTextBounds(mainStr, 0, 0, &x1, &y1, &mw, &mh);
        const int16_t subX = col1X + Layout::INDOOR_ICON_SIZE + Layout::INDOOR_ICON_GAP + (int16_t)mw + 2;

        char accStr[4];
        snprintf(accStr, sizeof(accStr), "%u", (unsigned)s.iaqAccuracy);
        display.setFont(Layout::FONT_MICRO);
        display.setCursor(subX, Layout::INDOOR_ROW3_BASELINE + 2);
        display.setTextColor(GxEPD_BLACK);
        display.print(accStr);

        display.setFont(Layout::FONT_LARGE);
      }
      else
      {
        display.setFont(Layout::FONT_SMALL);
        drawIndoorRow(INDOOR_ICON_AIR_QUALITY, "--", col1X, Layout::INDOOR_ROW3_BASELINE, GxEPD_BLACK);
        display.setFont(Layout::FONT_LARGE);
      }

      /** Riga 4 - pressione atmosferica (nera, "XXXX hPa"). */
      if (s.valid) snprintf(buf, sizeof(buf), "%.0f hPa", s.pressure);
      else         strcpy(buf, "--");
      drawIndoorRow(INDOOR_ICON_PRESSURE, buf, col1X, Layout::INDOOR_ROW4_BASELINE, GxEPD_BLACK);
    }

    /**
     * Disegna la sub-colonna sun nel riquadro Weather, a destra del
     * blocco meteo corrente. 4 righe centrate orizzontalmente sull'asse
     * Layout::SUN_COL_CENTER_X:
     *   1. sunrise    (nera, "HH:MM")
     *   2. sunset     (nera, "HH:MM")            avvicinata a Row 1
     *   3. temp_range (@widget slider-temp-range)
     *   4. mini-chart (@widget storico-temperature)
     *
     * @param weatherRrX  x (px) del bordo sinistro del riquadro Weather
     *                    (usato solo per compatibilita' di firma; la
     *                    centratura usa Layout::SUN_COL_CENTER_X).
     */
    inline void renderSunColumn(int16_t /*weatherRrX*/)
    {
      // FONT_BODY (12pt): stesso font dei titoli fieldset Indoor/Weather/Forecast,
      // per uniformita' visiva delle scritte HH:MM di alba e tramonto.
      display.setFont(Layout::FONT_BODY);

      char hhmm[6];
      formatHHMM(sunriseEpoch, hhmm);
      drawSunRow(INDOOR_ICON_SUNRISE, hhmm, Layout::SUN_COL_CENTER_X, Layout::SUN_ROW1_BASELINE, GxEPD_BLACK);

      formatHHMM(sunsetEpoch, hhmm);
      drawSunRow(INDOOR_ICON_SUNSET, hhmm, Layout::SUN_COL_CENTER_X, Layout::SUN_ROW2_BASELINE, GxEPD_BLACK);

      /** Row 3 — @widget slider-temp-range. Cifre + cerchietti ° (nero)
       *  della barra temp-range. Le decorazioni gialle (barra + triangolo)
       *  sono gia' sul canale 0x28 grazie a drawTempRangeBarYellow chiamata
       *  prima di firstPage(). */
      drawTempRangeBarLabels(Layout::SUN_COL_CENTER_X, Layout::SUN_ROW3_BASELINE);

      /** Row 4 — @widget storico-temperature. Mini-chart andamento
       *  temperatura percepita esterna, centrato sullo stesso asse della
       *  slider sovrastante. */
      drawTempChart(Layout::SUN_COL_CENTER_X, Layout::BANNER_Y + Layout::TC_TOP_OFFSET);
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
      display.fillRect(0, Layout::BANNER_Y, Layout::BANNER_W, Layout::BANNER_H, GxEPD_WHITE);

      const int16_t rrTop    = Layout::BANNER_Y + Layout::BANNER_RR_INSET_Y;
      const int16_t rrHeight = Layout::BANNER_H - 2 * Layout::BANNER_RR_INSET_Y;

      // Titoli fieldset in FONT_BODY (font impostato prima della
      // chiamata: drawFieldsetRect non setta font).
      display.setFont(Layout::FONT_BODY);

      Graphics::drawFieldsetRect(Layout::INDOOR_RR_X,   rrTop, Layout::INDOOR_RR_W,   rrHeight,
                                 Layout::BANNER_RR_RADIUS, "Indoor",
                                 Layout::BANNER_TITLE_LEFT_OFFSET, GxEPD_WHITE);
      Graphics::drawFieldsetRect(Layout::WEATHER_RR_X,  rrTop, Layout::WEATHER_RR_W,  rrHeight,
                                 Layout::BANNER_RR_RADIUS, "Weather",
                                 Layout::BANNER_TITLE_LEFT_OFFSET, GxEPD_WHITE);
      Graphics::drawFieldsetRect(Layout::FORECAST_RR_X, rrTop, Layout::FORECAST_RR_W, rrHeight,
                                 Layout::BANNER_RR_RADIUS, "Forecast",
                                 Layout::BANNER_TITLE_LEFT_OFFSET, GxEPD_WHITE);

      // Contenuti dei riquadri. Baseline ICON_Y/DESC/TEMP/TIME invariate
      // per current e forecast.
      renderIndoorBox(Layout::INDOOR_RR_X);
      renderBlock(0, Layout::BLOCK_CURRENT_X, Layout::BLOCK_CURRENT_W);
      renderSunColumn(Layout::WEATHER_RR_X);
      renderBlock(1, Layout::BLOCK_FC0_X, Layout::BLOCK_FC_W);
      renderBlock(2, Layout::BLOCK_FC1_X, Layout::BLOCK_FC_W);
      renderBlock(3, Layout::BLOCK_FC2_X, Layout::BLOCK_FC_W);
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
       * Epoch di riferimento per il calendario: se il meteo corrente è valido
       * (modifica 20/04/26: ripreso da slots[0] per garantire data reale),
       * lo usiamo come "ora"; altrimenti Calendar::draw ripieghera' su time().
       */
      time_t calEpoch = slots[0].valid ? slots[0].epoch : 0;

      /**
       * Pre-write del canale yellow (0x28) — @widget slider-temp-range.
       * Deve avvenire PRIMA di firstPage() e setta preserveYellow(true)
       * cosi' il canale sopravvive al loop paged che gestisce solo
       * black+red. Il driver custom auto-resetta preserveYellow alla
       * fine di refresh().
       * @since 22/04/26
       */
      {
        const int16_t trbCellY = Layout::SUN_ROW3_BASELINE + Layout::TRB_CELL_Y_OFFSET;
        drawTempRangeBarYellow(Layout::SUN_COL_CENTER_X, trbCellY);
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
        display.fillRect(Layout::SIDEBAR_X, 0, Layout::SIDEBAR_W, Layout::BANNER_Y, GxEPD_WHITE);
        /**
         * Calendar::draw riempie di bianco il proprio riquadro: riduce di fatto
         * l'area visibile dell'immagine di background. Il riquadro calendario
         * è posizionato subito a sinistra della sidebar. Il TZ applicato è
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

    // Reset storico del mini-chart temperatura (igienico: BSS gia' a 0).
    tcHistCount     = 0;
    tcHistHead      = 0;
    tcHistLastEpoch = 0;

    lastForecastMs = 0;
    firstRun       = true;
    // Primo refresh posticipato fino all'arrivo di meteo corrente + almeno
    // una previsione futura: evita il refresh sprecato (~22 s) col banner
    // a placeholder "--" al boot.
    needsRefresh    = false;
    firstRenderDone = false;
  }

  /**
   * Ritorna FETCH_BOTH se è scaduto INTERVAL_FORECAST (pilotato da
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
   * (WL_CONNECTED); non accende nè spegne la radio. In caso di successo
   * aggiorna entrambi i timer (current + forecast) perchè One Call 3.0
   * restituisce i due gruppi di dati in un'unica risposta, e marca il
   * banner come "da ridisegnare". Ritorna true se la richiesta è andata
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
      // Registra il campione corrente nello storico del mini-chart temperatura.
      recordHistory(slots[0].feelsLikeC, slots[0].epoch);
    }
    /**
     * firstRun resta true finchè NON abbiamo sia meteo corrente che almeno
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
       * Blocca il primo refresh finchè non arrivano meteo corrente (slots[0])
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
   * No-op se il primo refresh è gia' stato eseguito: chiamate ripetute da
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

#ifndef CALENDAR_H
#define CALENDAR_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <stdint.h>

#include <GxEPD2_3C.h>
#include "GxEPD2_097c_SOLUM_672x960/GxEPD2_097c_SOLUM_672x960.h"

#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>

#include "Env.h"
#include "Graphics.h"

// ---------------------------------------------------------------------------
// Istanza del display (definita nello sketch .ino). Va dichiarata a livello
// globale perche' il simbolo lato .ino non vive in alcun namespace.
// ---------------------------------------------------------------------------
extern GxEPD2_3C<GxEPD2_097c_SOLUM_672x960, GxEPD2_097c_SOLUM_672x960::HEIGHT / 8> display;

// ---------------------------------------------------------------------------
// Configurazione non-segreta del modulo. Non sta in Env.h perche' non
// sono credenziali: sono parametri di dominio che l'utente modifica
// direttamente qui.
// ---------------------------------------------------------------------------

/**
 * POSIX TZ string per Europe/Rome con gestione automatica del DST.
 *  - Standard: CET (UTC+1), da ultima domenica di ottobre alle 03:00 locali
 *    a ultima domenica di marzo.
 *  - Estiva:   CEST (UTC+2), da ultima domenica di marzo alle 02:00 UTC
 *    a ultima domenica di ottobre alle 03:00 locali.
 *
 * Applicata da Calendar::initTimezone() via setenv("TZ", ...) + tzset().
 * Dopo l'init, tutte le chiamate a localtime_r() nel progetto ritornano
 * componenti locali gia' corretti per il DST in corso.
 *
 * @since 21/04/26 Mattia Alesi
 */
#define CAL_POSIX_TZ            "CET-1CEST,M3.5.0,M10.5.0/3"

/**
 * Tenant Azure AD per Microsoft Graph. Usato nella URL del token
 * endpoint. "common" consente sia account personali Microsoft sia
 * account organizzativi; altrimenti si puo' mettere il GUID del
 * tenant specifico per limitare l'accesso.
 *
 * @since 21/04/26 Mattia Alesi
 */
#define CAL_MSGRAPH_TENANT_ID   "common"

/**
 * Modulo calendario header-only:
 *  - riquadro del mese corrente (in alto nella sidebar, bordi arrotondati);
 *  - lista dei prossimi eventi non ancora terminati, prelevati da due
 *    sorgenti indipendenti (Outlook via Microsoft Graph, fino a 5 eventi,
 *    + Google Calendar via Calendar API v3, fino a 5 eventi) e fuse in
 *    un unico elenco; il rendering mostra i 5 piu' vicini all'orario
 *    corrente (merge + insertion sort per startUtc crescente);
 *  - fetch OAuth2 refresh-token per ciascuna sorgente.
 *
 * Il disegno avviene dentro il loop paged firstPage()/nextPage() del
 * display e-paper (invocato da Weather::render). I fetch presuppongono
 * STA gia' connessa: la radio e' gestita dallo sketch .ino.
 *
 * API:
 *  - Calendar::initTimezone()    : applica CAL_POSIX_TZ al sistema;
 *  - Calendar::draw()            : riquadro mese corrente + lista eventi;
 *  - namespace Calendar::Outlook : scheduler/fetch eventi Microsoft Graph;
 *  - namespace Calendar::Google  : scheduler/fetch eventi Google Calendar.
 *
 * Usato da: Weather.h (render) e GxEPD2_1330c_GDEM133Z91.ino (fetch + init).
 *
 * @since 20/04/26 Mattia Alesi
 * @modified 21/04/26 integrazione Outlook (Microsoft Graph), round rect
 *                    cornice mese + cella oggi, layout sidebar 320 px.
 * @modified 21/04/26 aggiunto sottosistema Google Calendar; rendering
 *                    merge-ordinato dei primi MAX_EVENTS_DISPLAYED
 *                    eventi piu' vicini all'orario attuale.
 * @modified 21/04/26 localizzazione Europe/Rome via POSIX TZ con DST
 *                    automatico: rimosso parametro tzOffset dalle API,
 *                    tutti i componenti locali derivano da localtime_r().
 */
namespace Calendar
{
  /**
   * Numero massimo di eventi effettivamente renderizzati nella lista.
   * Le fonti (Outlook, Google) alimentano ciascuna una propria cache;
   * la lista mostrata e' il merge ordinato per data di inizio.
   */
  static constexpr uint8_t MAX_EVENTS_DISPLAYED = 5;

  namespace Outlook
  {
    /** Numero di eventi cacheati da questa sorgente. */
    static constexpr uint8_t MAX_EVENTS = 5;

    /**
     * Soglia di tentativi consecutivi falliti oltre la quale si interrompe
     * il retry immediato e si aspetta INTERVAL_FETCH_MS come un ciclo normale.
     * Evita hammering dell'endpoint Graph in caso di credenziali errate o
     * server irraggiungibile, specie durante la finestra OTA (loop ogni ~10ms).
     */
    static constexpr uint8_t MAX_FAILED_ATTEMPTS = 2;

    /**
     * Intervallo minimo fra due fetch consecutivi (ms). Deriva da
     * CAL_OUTLOOK_FETCH_MIN dichiarato nello sketch .ino; il fallback
     * rende l'header autonomamente compilabile.
     */
    #ifndef CAL_OUTLOOK_FETCH_MIN
    #define CAL_OUTLOOK_FETCH_MIN 20
    #endif
    static constexpr uint32_t INTERVAL_FETCH_MS =
        (uint32_t)CAL_OUTLOOK_FETCH_MIN * 60UL * 1000UL;  // default 20 min
  }

  namespace Google
  {
    /** Numero di eventi cacheati da questa sorgente. */
    static constexpr uint8_t MAX_EVENTS = 5;

    /** Vedi Outlook::MAX_FAILED_ATTEMPTS. */
    static constexpr uint8_t MAX_FAILED_ATTEMPTS = 2;

    /**
     * Intervallo minimo fra due fetch consecutivi (ms). Deriva da
     * CAL_GOOGLE_FETCH_MIN dichiarato nello sketch .ino; il fallback
     * rende l'header autonomamente compilabile.
     */
    #ifndef CAL_GOOGLE_FETCH_MIN
    #define CAL_GOOGLE_FETCH_MIN 20
    #endif
    static constexpr uint32_t INTERVAL_FETCH_MS =
        (uint32_t)CAL_GOOGLE_FETCH_MIN * 60UL * 1000UL;   // default 20 min
  }

  // -------------------------------------------------------------------------
  // Stato, layout e helper interni. Tutto dentro `detail` per non esporlo
  // fuori dal modulo.
  // -------------------------------------------------------------------------
  namespace detail
  {
    // -----------------------------------------------------------------------
    // Layout del riquadro mese (screen 960x672, sidebar x=640..960 w=320).
    // -----------------------------------------------------------------------
    // modificato 21/04/26: 650 -> 630 e 300 -> 320 per sfruttare la sidebar
    // allargata (area immagine 640x480 -> 620x460). Bordo destro invariato
    // (x=950). CAL_W=320 rende gridW=308 divisibile esattamente per 7 (44
    // px/cella) -> centratura orizzontale celle risolta.
    inline constexpr int16_t CAL_X   = 630;   // era 650
    inline constexpr int16_t CAL_Y   = 10;
    inline constexpr int16_t CAL_W   = 320;   // era 300
    inline constexpr int16_t CAL_H   = 200;
    // modificato 22/04/26: 8 -> 14 per bordi piu' morbidi, coerenti con
    // il raggio usato nel banner meteo.
    inline constexpr int16_t CAL_R   = 14;   // raggio round-rect cornice mese
    inline constexpr int16_t CELL_R  = 4;    // raggio round-rect cella oggi
    inline constexpr int16_t CAL_PAD = 6;
    // modificato 22/04/26: 34 -> 20. Il titolo mese ora e' disegnato in
    // stile fieldset sul bordo superiore (non piu' dentro) quindi serve
    // solo il margine per il cap-height del 18pt che sconfina nel
    // riquadro dalla linea del bordo (~10 px sopra, ~10 sotto).
    inline constexpr int16_t TITLE_H = 20;
    // modificato 21/04/26: 22 -> 26 per accomodare header giorni in
    // FreeSans12pt7b.
    inline constexpr int16_t HDR_H   = 26;
    // nuovo 21/04/26: margine verticale fra la linea orizzontale sotto
    // L M M G V S e la prima riga di celle giorno (prima era a filo).
    inline constexpr int16_t GRID_TOP_PAD = 3;

    // -----------------------------------------------------------------------
    // Layout area eventi (sotto al mese, fino al top del banner meteo y=480).
    // -----------------------------------------------------------------------
    inline constexpr int16_t EVT_X       = CAL_X;
    inline constexpr int16_t EVT_Y       = CAL_Y + CAL_H + 10;   // 220
    inline constexpr int16_t EVT_W       = CAL_W;                // 300
    // modificato 21/04/26: 250 -> 230 per stare in sidebar alta 460 (era 480).
    // Con CAL a 10..210 + 10 px gap -> events 220..450 + 10 px gap -> banner 460.
    inline constexpr int16_t EVT_H       = 230;                  // era 250
    inline constexpr int16_t EVT_ROW_H   = EVT_H / Calendar::MAX_EVENTS_DISPLAYED;  // 46 (era 50)
    inline constexpr int16_t EVT_SEP_PAD  = 15;   // inset orizzontale separatori
    inline constexpr int16_t EVT_PAD      = 8;    // padding interno (destra)
    /** Padding sinistro del testo titolo evento.
     *  Modifica 22/04/26: 8 -> 3 per avvicinare il titolo al bordo
     *  sinistro del riquadro eventi (date/ora a destra restano a EVT_PAD). */
    inline constexpr int16_t EVT_PAD_LEFT = 3;

    // -----------------------------------------------------------------------
    // Testi statici.
    // -----------------------------------------------------------------------
    inline const char* const MESI_IT[12] = {
      "Gennaio",  "Febbraio", "Marzo",    "Aprile",
      "Maggio",   "Giugno",   "Luglio",   "Agosto",
      "Settembre","Ottobre",  "Novembre", "Dicembre"
    };

    inline const char* const DOW_IT[7] = { "L", "M", "M", "G", "V", "S", "D" };

    inline constexpr int8_t DAYS_IN_MONTH[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };

    // -----------------------------------------------------------------------
    // Struct comune a tutte le sorgenti.
    // -----------------------------------------------------------------------
    struct CalEvent
    {
      char    title[48];
      time_t  startUtc;
      time_t  endUtc;
      bool    allDay;
      bool    valid;
    };

    // -----------------------------------------------------------------------
    // Cache per sorgente. Ciascun sottosistema aggiorna solo il proprio
    // array; il rendering legge entrambi e produce un merge ordinato per
    // startUtc crescente, ritagliando i primi MAX_EVENTS_DISPLAYED.
    // -----------------------------------------------------------------------
    inline CalEvent  outlookEvents[Calendar::Outlook::MAX_EVENTS];
    inline String    cachedOutlookToken;
    inline uint32_t  outlookTokenExpiresAtMs = 0;
    inline uint32_t  lastOutlookFetchMs      = 0;
    inline bool      outlookFirstFetch       = true;
    // Tentativi consecutivi falliti: al raggiungimento di MAX_FAILED_ATTEMPTS
    // il counter viene azzerato e lastOutlookFetchMs posticipato di INTERVAL_FETCH_MS.
    inline uint8_t   outlookFailedAttempts   = 0;

    inline CalEvent  googleEvents[Calendar::Google::MAX_EVENTS];
    inline String    cachedGoogleToken;
    inline uint32_t  googleTokenExpiresAtMs  = 0;
    inline uint32_t  lastGoogleFetchMs       = 0;
    inline bool      googleFirstFetch        = true;
    inline uint8_t   googleFailedAttempts    = 0;

    // =======================================================================
    // Helpers generici
    // =======================================================================

    /**
     * Ritorna true se l'anno e' bisestile (regola gregoriana).
     * @since 20/04/26 Mattia Alesi
     */
    inline bool isLeapY(int year)
    {
      return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    }

    /**
     * Converte componenti UTC (Y,M,D,h,m,s) in epoch UNIX.
     * @since 21/04/26 Mattia Alesi
     */
    inline time_t ymdhmsToUtcEpoch(int Y, int M, int D, int h, int m, int s)
    {
      static const int16_t DTM[12] = { 0,31,59,90,120,151,181,212,243,273,304,334 };
      int32_t days = 0;
      for (int y = 1970; y < Y; y++) days += isLeapY(y) ? 366 : 365;
      days += DTM[M - 1];
      if (M > 2 && isLeapY(Y)) days++;
      days += (D - 1);
      return (time_t)((int64_t)days * 86400LL + (int64_t)h * 3600 + m * 60 + s);
    }

    /**
     * Parsifica una stringa ISO 8601 con offset opzionale (Z, +HH:MM,
     * -HH:MM) e la converte in epoch UTC. Se non c'e' offset esplicito
     * interpreta come UTC (comportamento atteso per Outlook con
     * `Prefer: outlook.timezone=UTC`).
     *
     * Formati accettati:
     *   "2026-04-21T14:00:00"
     *   "2026-04-21T14:00:00Z"
     *   "2026-04-21T14:00:00+02:00"
     *   "2026-04-21T14:00:00.123Z"
     *
     * @since 21/04/26 Mattia Alesi
     */
    inline bool parseIsoDateTimeToUtc(const char* str, time_t& outUtc)
    {
      if (!str || !*str) return false;
      int Y=0, M=0, D=0, h=0, m=0, s=0;
      if (sscanf(str, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) < 3) return false;

      int offsetSec = 0;
      const char* t = strchr(str, 'T');
      const char* z = t ? strpbrk(t + 1, "Z+-") : nullptr;
      if (z && *z != 'Z')
      {
        int sign = (*z == '-') ? -1 : 1;
        int oh = 0, om = 0;
        if (sscanf(z + 1, "%d:%d", &oh, &om) >= 1)
        {
          offsetSec = sign * (oh * 3600 + om * 60);
        }
      }
      /**
       * ymdhmsToUtcEpoch() tratta i componenti come UTC; la stringa poteva
       * essere un "local time + offset", quindi sottraiamo l'offset per
       * ottenere l'epoch UTC reale.
       */
      outUtc = ymdhmsToUtcEpoch(Y, M, D, h, m, s) - offsetSec;
      return true;
    }

    /**
     * Parsifica "YYYY-MM-DD" (evento all-day) come mezzanotte UTC del giorno.
     * @since 21/04/26 Mattia Alesi
     */
    inline bool parseIsoDateToUtc(const char* str, time_t& outUtc)
    {
      if (!str || !*str) return false;
      int Y, M, D;
      if (sscanf(str, "%d-%d-%d", &Y, &M, &D) != 3) return false;
      outUtc = ymdhmsToUtcEpoch(Y, M, D, 0, 0, 0);
      return true;
    }

    /**
     * Disegna una stringa centrata orizzontalmente sulla colonna `centerX`
     * con la baseline alla y specificata, usando il font gia' impostato.
     * @since 20/04/26 Mattia Alesi
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
     * Disegna una stringa allineata a destra.
     * @since 21/04/26 Mattia Alesi
     */
    inline void drawRightAligned(const char* txt, int16_t rightX, int16_t baselineY, uint16_t color)
    {
      int16_t x1, y1;
      uint16_t w, h;
      display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
      display.setCursor(rightX - (int16_t)w - x1, baselineY);
      display.setTextColor(color);
      display.print(txt);
    }

    /**
     * Tronca la stringa in `buf` in-place finche' la sua resa non rientra
     * in `maxW` pixel. Se tronca inserisce ".." come indicatore finale.
     * @since 21/04/26 Mattia Alesi
     */
    inline void truncateToWidth(char* buf, int16_t maxW)
    {
      int16_t x1, y1;
      uint16_t w, h;
      display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
      if ((int16_t)w <= maxW) return;

      size_t n = strlen(buf);
      while (n > 0)
      {
        buf[--n] = 0;
        display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
        if ((int16_t)w <= maxW) break;
      }
      if (n >= 2) { buf[n - 1] = '.'; buf[n - 2] = '.'; }
    }

    /**
     * Ricava un epoch UTC "adesso" affidabile: RTC di sistema se sincronizzato,
     * altrimenti 0 come sentinella.
     * @since 21/04/26 Mattia Alesi
     */
    inline time_t nowUtcEpoch()
    {
      time_t t = time(nullptr);
      return (t > 1000000000) ? t : 0;
    }

    /**
     * Formatta un epoch UTC in stringa ISO 8601 "YYYY-MM-DDTHH:MM:SSZ".
     * @since 21/04/26 Mattia Alesi
     */
    inline void formatIsoUtc(time_t epoch, char* out, size_t outLen)
    {
      struct tm tm;
      gmtime_r(&epoch, &tm);
      snprintf(out, outLen, "%04d-%02d-%02dT%02d:%02d:%02dZ",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    /**
     * Refresh generico di un access token OAuth2 (grant_type=refresh_token).
     * Gestisce sia flussi senza client_secret (Microsoft client pubblico)
     * sia flussi con client_secret (Google desktop app).
     *
     * @param tokenUrl     endpoint token
     * @param clientId     client id
     * @param clientSecret client secret ("" per client pubblici)
     * @param refreshToken refresh token
     * @param scopeEncoded scope gia' URL-encoded ("" per ereditarlo)
     * @param outToken     access token ricevuto
     * @param outExpiresIn durata in secondi dichiarata dall'IdP
     * @since 21/04/26 Mattia Alesi
     */
    inline bool oauthRefreshGeneric(const char* tokenUrl,
                                    const char* clientId,
                                    const char* clientSecret,
                                    const char* refreshToken,
                                    const char* scopeEncoded,
                                    String& outToken,
                                    uint32_t& outExpiresIn)
    {
      WiFiClientSecure client;
      client.setInsecure();

      HTTPClient http;
      if (!http.begin(client, tokenUrl))
      {
        Serial.println(F("[OAuth] http.begin failed"));
        return false;
      }
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");

      String body;
      body.reserve(512);
      body  = "client_id=";       body += clientId;
      if (clientSecret && *clientSecret)
      {
        body += "&client_secret="; body += clientSecret;
      }
      body += "&grant_type=refresh_token";
      body += "&refresh_token=";  body += refreshToken;
      if (scopeEncoded && *scopeEncoded)
      {
        body += "&scope=";         body += scopeEncoded;
      }

      int code = http.POST(body);
      if (code != 200)
      {
        Serial.printf("[OAuth] token refresh failed: http=%d\n", code);
        http.end();
        return false;
      }

      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, http.getStream());
      http.end();
      if (err) { Serial.printf("[OAuth] token json parse: %s\n", err.c_str()); return false; }

      const char* tok = doc["access_token"] | "";
      if (!tok[0]) { Serial.println(F("[OAuth] access_token mancante")); return false; }
      outToken     = tok;
      outExpiresIn = doc["expires_in"] | 3600UL;
      return true;
    }

    // =======================================================================
    // Microsoft Graph (Outlook): token + fetch eventi
    // =======================================================================

    /**
     * Rinnova l'access token Microsoft Graph se scaduto (margine 60 s).
     * @since 21/04/26 Mattia Alesi
     */
    inline bool refreshOutlookToken()
    {
      uint32_t now = millis();
      if (cachedOutlookToken.length() > 0 && (int32_t)(outlookTokenExpiresAtMs - now) > 0)
        return true;

      String url = String("https://login.microsoftonline.com/") + CAL_MSGRAPH_TENANT_ID
                 + "/oauth2/v2.0/token";
      uint32_t expiresIn = 0;
      bool ok = oauthRefreshGeneric(
        url.c_str(), MSGRAPH_CLIENT_ID, "",
        MSGRAPH_REFRESH_TOKEN,
        "https%3A%2F%2Fgraph.microsoft.com%2FCalendars.Read%20offline_access",
        cachedOutlookToken, expiresIn);
      if (!ok) return false;

      outlookTokenExpiresAtMs = millis() + (expiresIn > 60 ? expiresIn - 60 : expiresIn) * 1000UL;
      Serial.printf("[Outlook] token OK, expires in %lus\n", (unsigned long)expiresIn);
      return true;
    }

    /**
     * GET /me/events con filtro `end/dateTime ge <now>`, top=MAX_EVENTS=5,
     * ordinato per inizio. Popola `outlookEvents[]`.
     * @since 21/04/26 Mattia Alesi
     */
    inline bool fetchOutlookEvents()
    {
      time_t now = nowUtcEpoch();
      char nowIso[24] = "1970-01-01T00:00:00Z";
      if (now > 0) formatIsoUtc(now, nowIso, sizeof(nowIso));

      WiFiClientSecure client;
      client.setInsecure();

      HTTPClient http;
      String url;
      url.reserve(360);
      url  = "https://graph.microsoft.com/v1.0/me/events?$top=";
      url += (int)Calendar::Outlook::MAX_EVENTS;
      url += "&$orderby=start/dateTime";
      url += "&$select=subject,start,end,isAllDay";
      url += "&$filter=end/dateTime%20ge%20'";
      url += nowIso;
      url += "'";

      if (!http.begin(client, url)) { Serial.println(F("[Outlook] events http.begin failed")); return false; }
      http.addHeader("Authorization", String("Bearer ") + cachedOutlookToken);
      http.addHeader("Prefer", "outlook.timezone=\"UTC\"");

      int code = http.GET();
      if (code != 200)
      {
        Serial.printf("[Outlook] events fetch failed: http=%d\n", code);
        http.end();
        if (code == 401) { cachedOutlookToken = ""; outlookTokenExpiresAtMs = 0; }
        return false;
      }

      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, http.getStream());
      http.end();
      if (err) { Serial.printf("[Outlook] events json parse: %s\n", err.c_str()); return false; }

      JsonArrayConst value = doc["value"].as<JsonArrayConst>();
      if (value.isNull()) { Serial.println(F("[Outlook] value mancante")); return false; }

      int n = 0;
      for (JsonVariantConst item : value)
      {
        if (n >= (int)Calendar::Outlook::MAX_EVENTS) break;

        const char* subject  = item["subject"]           | "(senza titolo)";
        const char* startStr = item["start"]["dateTime"] | "";
        const char* endStr   = item["end"]["dateTime"]   | "";
        bool        allDay   = item["isAllDay"]          | false;

        time_t startUtc = 0;
        if (!parseIsoDateTimeToUtc(startStr, startUtc)) continue;
        time_t endUtc = startUtc;
        parseIsoDateTimeToUtc(endStr, endUtc);

        CalEvent& e = outlookEvents[n];
        strncpy(e.title, subject, sizeof(e.title) - 1);
        e.title[sizeof(e.title) - 1] = 0;
        e.startUtc = startUtc;
        e.endUtc   = endUtc;
        e.allDay   = allDay;
        e.valid    = true;
        n++;
      }
      for (int i = n; i < (int)Calendar::Outlook::MAX_EVENTS; i++) outlookEvents[i].valid = false;

      Serial.printf("[Outlook] fetched %d events (end>='%s')\n", n, nowIso);
      return n > 0;
    }

    // =======================================================================
    // Google Calendar API v3: token + fetch eventi
    // =======================================================================

    /**
     * Rinnova l'access token Google se scaduto (margine 60 s).
     */
    inline bool refreshGoogleToken()
    {
      uint32_t now = millis();
      if (cachedGoogleToken.length() > 0 && (int32_t)(googleTokenExpiresAtMs - now) > 0)
        return true;

      uint32_t expiresIn = 0;
      /**
       * Google richiede client_secret anche per desktop app; scope
       * viene ereditato dal refresh token originale -> omesso.
       */
      bool ok = oauthRefreshGeneric(
        "https://oauth2.googleapis.com/token",
        GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET,
        GOOGLE_REFRESH_TOKEN, "",
        cachedGoogleToken, expiresIn);
      if (!ok) return false;

      googleTokenExpiresAtMs = millis() + (expiresIn > 60 ? expiresIn - 60 : expiresIn) * 1000UL;
      Serial.printf("[Google] token OK, expires in %lus\n", (unsigned long)expiresIn);
      return true;
    }

    /**
     * GET /calendar/v3/calendars/primary/events con timeMin=<now>,
     * maxResults=MAX_EVENTS=5, singleEvents=true, orderBy=startTime.
     * timeMin su Google tiene in lista anche gli eventi in corso (termine
     * nel futuro).
     */
    inline bool fetchGoogleEvents()
    {
      time_t now = nowUtcEpoch();
      char nowIso[24] = "1970-01-01T00:00:00Z";
      if (now > 0) formatIsoUtc(now, nowIso, sizeof(nowIso));

      WiFiClientSecure client;
      client.setInsecure();

      HTTPClient http;
      String url;
      url.reserve(360);
      url  = "https://www.googleapis.com/calendar/v3/calendars/primary/events?maxResults=";
      url += (int)Calendar::Google::MAX_EVENTS;
      url += "&orderBy=startTime&singleEvents=true&timeMin=";
      url += nowIso;
      // Percent-encode dei ':' nel timeMin (buona pratica verso proxy).
      url.replace(":", "%3A");

      if (!http.begin(client, url)) { Serial.println(F("[Google] events http.begin failed")); return false; }
      http.addHeader("Authorization", String("Bearer ") + cachedGoogleToken);

      int code = http.GET();
      if (code != 200)
      {
        Serial.printf("[Google] events fetch failed: http=%d\n", code);
        http.end();
        if (code == 401) { cachedGoogleToken = ""; googleTokenExpiresAtMs = 0; }
        return false;
      }

      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, http.getStream());
      http.end();
      if (err) { Serial.printf("[Google] events json parse: %s\n", err.c_str()); return false; }

      JsonArrayConst items = doc["items"].as<JsonArrayConst>();
      if (items.isNull()) { Serial.println(F("[Google] items mancante")); return false; }

      int n = 0;
      for (JsonVariantConst item : items)
      {
        if (n >= (int)Calendar::Google::MAX_EVENTS) break;

        const char* summary        = item["summary"]           | "(senza titolo)";
        const char* startDateTime  = item["start"]["dateTime"] | (const char*)nullptr;
        const char* startDate      = item["start"]["date"]     | (const char*)nullptr;
        const char* endDateTime    = item["end"]["dateTime"]   | (const char*)nullptr;
        const char* endDate        = item["end"]["date"]       | (const char*)nullptr;

        time_t startUtc = 0, endUtc = 0;
        bool allDay = false;

        if (startDateTime)
        {
          if (!parseIsoDateTimeToUtc(startDateTime, startUtc)) continue;
          endUtc = startUtc;
          if (endDateTime) parseIsoDateTimeToUtc(endDateTime, endUtc);
        }
        else if (startDate)
        {
          allDay = true;
          if (!parseIsoDateToUtc(startDate, startUtc)) continue;
          endUtc = startUtc + 86400;
          if (endDate) parseIsoDateToUtc(endDate, endUtc);
        }
        else
        {
          continue;
        }

        CalEvent& e = googleEvents[n];
        strncpy(e.title, summary, sizeof(e.title) - 1);
        e.title[sizeof(e.title) - 1] = 0;
        e.startUtc = startUtc;
        e.endUtc   = endUtc;
        e.allDay   = allDay;
        e.valid    = true;
        n++;
      }
      for (int i = n; i < (int)Calendar::Google::MAX_EVENTS; i++) googleEvents[i].valid = false;

      Serial.printf("[Google] fetched %d events (timeMin='%s')\n", n, nowIso);
      return n > 0;
    }

    // =======================================================================
    // Rendering: riquadro mese
    // =======================================================================

    /**
     * Disegna il riquadro del mese corrente (cornice arrotondata, titolo,
     * intestazione giorni, griglia 7x6, cella odierna in rosso con round-rect).
     */
    inline void drawMonthWidget(const struct tm& today)
    {
      const int todayDay   = today.tm_mday;
      const int todayMonth = today.tm_mon;
      const int todayYear  = today.tm_year + 1900;
      const int todayWday  = today.tm_wday;

      int daysInMonth = DAYS_IN_MONTH[todayMonth];
      if (todayMonth == 1 && isLeapY(todayYear)) daysInMonth = 29;

      int firstWdaySun = ((todayWday - (todayDay - 1)) % 7 + 7) % 7;
      int firstCol     = (firstWdaySun + 6) % 7;

      /** Sfondo bianco + cornice arrotondata in stile fieldset: il nome
       *  del mese e' incastrato sul bordo superiore interrotto (niente
       *  piu' titolo interno + linea separatrice).
       *  @modified 22/04/26 */
      display.fillRoundRect(CAL_X, CAL_Y, CAL_W, CAL_H, CAL_R, GxEPD_WHITE);
      display.setFont(&FreeSans18pt7b);
      Graphics::drawFieldsetRect(CAL_X, CAL_Y, CAL_W, CAL_H, CAL_R,
                                 MESI_IT[todayMonth],
                                 CAL_R + 4, GxEPD_WHITE);

      const int16_t gridX = CAL_X + CAL_PAD;
      const int16_t gridY = CAL_Y + TITLE_H;
      const int16_t gridW = CAL_W - 2 * CAL_PAD;
      const int16_t cellW = gridW / 7;
      const int16_t cellH = (CAL_H - TITLE_H - HDR_H - GRID_TOP_PAD - CAL_PAD) / 6;

      display.setFont(&FreeSans12pt7b);
      for (int i = 0; i < 7; i++)
      {
        int16_t cx = gridX + i * cellW + cellW / 2;
        drawCentered(DOW_IT[i], cx, gridY + HDR_H - 6, GxEPD_BLACK);
      }

      display.drawFastHLine(CAL_X + CAL_R, gridY + HDR_H,
                            CAL_W - 2 * CAL_R, GxEPD_BLACK);

      // modificato 21/04/26: + GRID_TOP_PAD per staccare la prima riga di
      // celle dalla linea orizzontale dei giorni L M M G V S.
      const int16_t gridTop = gridY + HDR_H + GRID_TOP_PAD;
      char buf[4];
      for (int d = 1; d <= daysInMonth; d++)
      {
        int idx = firstCol + (d - 1);
        int row = idx / 7;
        int col = idx % 7;
        int16_t cellX = gridX + col * cellW;
        int16_t cellY = gridTop + row * cellH;
        int16_t cx    = cellX + cellW / 2;
        // modificato 21/04/26: -5 -> -4 per centrare verticalmente il
        // numero del giorno dentro la cella (cellH=21, cap 12pt ~14).
        int16_t baseY = cellY + cellH - 4;

        snprintf(buf, sizeof(buf), "%d", d);

        if (d == todayDay)
        {
          // Badge rosso inset 2 px lateralmente e 1 px sopra/sotto: margine
          // visibile uniforme attorno al numero (aspetto da "badge" centrato
          // nella cella, non fill edge-to-edge).
          // @modified 21/04/26 Mattia Alesi
          display.fillRoundRect(cellX + 2, cellY, cellW - 4, cellH - 1, CELL_R, GxEPD_RED);
          drawCentered(buf, cx, baseY, GxEPD_WHITE);
        }
        else
        {
          drawCentered(buf, cx, baseY, GxEPD_BLACK);
        }
      }
    }

    // =======================================================================
    // Rendering: lista eventi (merge Outlook + Google)
    // =======================================================================

    /**
     * Disegna una singola riga di evento (o placeholder se `e.valid==false`).
     * Colore: rosso se l'evento cade nella data odierna locale, nero altrimenti.
     */
    inline void drawEventRow(int row, const CalEvent& e,
                             int todayY, int todayM, int todayD)
    {
      int16_t rowTop = EVT_Y + row * EVT_ROW_H;

      if (row > 0)
      {
        display.drawFastHLine(EVT_X + EVT_SEP_PAD, rowTop,
                              EVT_W - 2 * EVT_SEP_PAD, GxEPD_BLACK);
      }

      display.setFont(&FreeSans12pt7b);

      if (!e.valid)
      {
        display.setCursor(EVT_X + EVT_PAD_LEFT, rowTop + EVT_ROW_H / 2 + 6);
        display.setTextColor(GxEPD_BLACK);
        display.print("--");
        return;
      }

      /**
       * Localtime secondo TZ di sistema (CAL_POSIX_TZ applicato da initTimezone):
       * gestisce automaticamente il passaggio CET <-> CEST.
       */
      struct tm tmL;
      localtime_r(&e.startUtc, &tmL);

      bool isToday = (tmL.tm_year + 1900 == todayY) &&
                     (tmL.tm_mon + 1      == todayM) &&
                     (tmL.tm_mday         == todayD);
      uint16_t color = isToday ? GxEPD_RED : GxEPD_BLACK;

      char dateBuf[12];
      if (tmL.tm_year + 1900 == todayY)
        snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d", tmL.tm_mday, tmL.tm_mon + 1);
      else
        snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d/%02d",
                 tmL.tm_mday, tmL.tm_mon + 1, (tmL.tm_year + 1900) % 100);

      char timeBuf[10];
      if (e.allDay) snprintf(timeBuf, sizeof(timeBuf), "tutto gg");
      else          snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", tmL.tm_hour, tmL.tm_min);

      int16_t x1, y1;
      uint16_t dw, dh, tw, th;
      display.getTextBounds(dateBuf, 0, 0, &x1, &y1, &dw, &dh);
      display.getTextBounds(timeBuf, 0, 0, &x1, &y1, &tw, &th);
      int16_t rightColW = (int16_t)(dw > tw ? dw : tw);

      int16_t rightX    = EVT_X + EVT_W - EVT_PAD;
      int16_t titleMaxW = (EVT_W - EVT_PAD_LEFT - EVT_PAD) - rightColW - 8;

      char titleBuf[sizeof(e.title)];
      strncpy(titleBuf, e.title, sizeof(titleBuf) - 1);
      titleBuf[sizeof(titleBuf) - 1] = 0;
      truncateToWidth(titleBuf, titleMaxW);

      int16_t titleBaseline = rowTop + EVT_ROW_H / 2 + 6;
      display.setCursor(EVT_X + EVT_PAD_LEFT, titleBaseline);
      display.setTextColor(color);
      display.print(titleBuf);

      /**
       * Orario sopra, data sotto.
       * Modificato 21/04/26: baseline adattate a EVT_ROW_H=46 (era 50) + 12pt.
       */
      drawRightAligned(timeBuf, rightX, rowTop + 19, color);
      drawRightAligned(dateBuf, rightX, rowTop + EVT_ROW_H - 7, color);
    }

    /**
     * Merge delle cache Outlook + Google (fino a 5+5=10 eventi validi),
     * insertion sort ascendente per startUtc, disegno delle prime
     * MAX_EVENTS_DISPLAYED righe (le piu' vicine all'orario attuale).
     */
    inline void drawEventsList(const struct tm& today)
    {
      display.fillRect(EVT_X, EVT_Y, EVT_W, EVT_H, GxEPD_WHITE);

      constexpr int CAP = Calendar::Outlook::MAX_EVENTS + Calendar::Google::MAX_EVENTS;
      const CalEvent* merged[CAP];
      int count = 0;

      for (auto& ev : outlookEvents) if (ev.valid && count < CAP) merged[count++] = &ev;
      for (auto& ev : googleEvents)  if (ev.valid && count < CAP) merged[count++] = &ev;

      // Insertion sort ascendente su startUtc (lista piccola, max 10).
      for (int i = 1; i < count; i++)
      {
        const CalEvent* key = merged[i];
        int j = i - 1;
        while (j >= 0 && merged[j]->startUtc > key->startUtc)
        {
          merged[j + 1] = merged[j];
          j--;
        }
        merged[j + 1] = key;
      }

      const int todayY = today.tm_year + 1900;
      const int todayM = today.tm_mon + 1;
      const int todayD = today.tm_mday;

      for (int i = 0; i < (int)Calendar::MAX_EVENTS_DISPLAYED; i++)
      {
        if (i < count)
        {
          drawEventRow(i, *merged[i], todayY, todayM, todayD);
        }
        else
        {
          CalEvent empty = {};
          drawEventRow(i, empty, todayY, todayM, todayD);
        }
      }
    }
  } // namespace detail

  // =========================================================================
  // API pubblica
  // =========================================================================

  /**
   * Applica la stringa POSIX TZ (CAL_POSIX_TZ) al processo: dopo questa
   * chiamata tutte le localtime_r() ritornano componenti in Europe/Rome
   * con DST automatico. Idempotente.
   *
   * Va chiamata in setup() PRIMA di qualsiasi altro modulo che formatti
   * orari locali (Weather::begin, Calendar::Outlook::begin, ecc.).
   */
  inline void initTimezone()
  {
    /**
     * Applica la stringa POSIX di Europe/Rome al processo. Da qui in
     * avanti localtime_r() gestisce DST in automatico.
     */
    setenv("TZ", CAL_POSIX_TZ, 1);
    tzset();
  }

  /**
   * Disegna il riquadro calendario del mese corrente (in alto a destra
   * dentro la sidebar) e la lista dei prossimi eventi (merge di Outlook +
   * Google) nello spazio sottostante, fino al banner meteo.
   *
   * I componenti locali vengono calcolati via localtime_r(), che
   * applica il TZ impostato da initTimezone().
   *
   * @param utcEpoch epoch UTC di riferimento (0 = usa time(nullptr))
   */
  inline void draw(time_t utcEpoch)
  {
    if (utcEpoch == 0) utcEpoch = time(nullptr);

    // Componenti locali derivati dal TZ di sistema (CAL_POSIX_TZ).
    struct tm tmNow;
    localtime_r(&utcEpoch, &tmNow);

    detail::drawMonthWidget(tmNow);
    detail::drawEventsList(tmNow);
  }

  /**
   * Sottosistema Outlook (Microsoft Graph). Cache dei prossimi
   * MAX_EVENTS eventi del calendario Outlook non ancora terminati;
   * refresh periodico via refresh-token + GET /me/events.
   */
  namespace Outlook
  {
    /** Azzera la cache. Una tantum in setup(). */
    inline void begin()
    {
      using namespace detail;
      for (auto& e : outlookEvents) { e.valid = false; e.title[0] = 0; e.startUtc = 0; e.endUtc = 0; e.allDay = false; }
      cachedOutlookToken      = "";
      outlookTokenExpiresAtMs = 0;
      lastOutlookFetchMs      = 0;
      outlookFirstFetch       = true;
      outlookFailedAttempts   = 0;
    }

    /** True se la cache e' scaduta o mai valorizzata: serve un nuovo fetch. */
    inline bool pendingFetch()
    {
      using namespace detail;
      if (outlookFirstFetch) return true;
      uint32_t now = millis();
      return (int32_t)(now - lastOutlookFetchMs) >= (int32_t)INTERVAL_FETCH_MS;
    }

    /**
     * Refresh del token + GET degli eventi. Presuppone STA connessa.
     * Le credenziali arrivano da Env.h (MSGRAPH_* define).
     *
     * Su fallimento incrementa un counter di tentativi consecutivi: dopo
     * MAX_FAILED_ATTEMPTS (2) "consuma" lo slot fissando lastOutlookFetchMs
     * al now e disattivando outlookFirstFetch, cosi' pendingFetch() tornera'
     * true solo dopo INTERVAL_FETCH_MS (evita hammering durante OTA).
     *
     * @return true se almeno un evento valido e' entrato in cache.
     */
    inline bool runFetch()
    {
      using namespace detail;
      bool ok = refreshOutlookToken() && fetchOutlookEvents();
      if (ok)
      {
        lastOutlookFetchMs    = millis();
        outlookFirstFetch     = false;
        outlookFailedAttempts = 0;
        return true;
      }
      if (++outlookFailedAttempts >= MAX_FAILED_ATTEMPTS)
      {
        lastOutlookFetchMs    = millis();
        outlookFirstFetch     = false;
        outlookFailedAttempts = 0;
      }
      return false;
    }
  }

  /**
   * Sottosistema Google Calendar (Calendar API v3). Cache dei prossimi
   * MAX_EVENTS eventi del calendario "primary" non ancora terminati;
   * refresh periodico via refresh-token + GET /calendar/v3/calendars/
   * primary/events.
   *
   * Le credenziali arrivano da Env.h (GOOGLE_CLIENT_ID,
   * GOOGLE_CLIENT_SECRET, GOOGLE_REFRESH_TOKEN). Scope richiesto:
   * https://www.googleapis.com/auth/calendar.readonly
   */
  namespace Google
  {
    /** Azzera la cache. Una tantum in setup(). */
    inline void begin()
    {
      using namespace detail;
      for (auto& e : googleEvents) { e.valid = false; e.title[0] = 0; e.startUtc = 0; e.endUtc = 0; e.allDay = false; }
      cachedGoogleToken      = "";
      googleTokenExpiresAtMs = 0;
      lastGoogleFetchMs      = 0;
      googleFirstFetch       = true;
      googleFailedAttempts   = 0;
    }

    /** True se la cache e' scaduta o mai valorizzata: serve un nuovo fetch. */
    inline bool pendingFetch()
    {
      using namespace detail;
      if (googleFirstFetch) return true;
      uint32_t now = millis();
      return (int32_t)(now - lastGoogleFetchMs) >= (int32_t)INTERVAL_FETCH_MS;
    }

    /**
     * Refresh del token + GET degli eventi. Presuppone STA connessa.
     * Su fallimento: stesso meccanismo di Outlook::runFetch() (MAX_FAILED_ATTEMPTS
     * tentativi consecutivi, poi attesa di INTERVAL_FETCH_MS).
     * @return true se almeno un evento valido e' entrato in cache.
     */
    inline bool runFetch()
    {
      using namespace detail;
      bool ok = refreshGoogleToken() && fetchGoogleEvents();
      if (ok)
      {
        lastGoogleFetchMs    = millis();
        googleFirstFetch     = false;
        googleFailedAttempts = 0;
        return true;
      }
      if (++googleFailedAttempts >= MAX_FAILED_ATTEMPTS)
      {
        lastGoogleFetchMs    = millis();
        googleFirstFetch     = false;
        googleFailedAttempts = 0;
      }
      return false;
    }
  }
}

#endif

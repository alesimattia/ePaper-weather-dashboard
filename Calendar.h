#ifndef CALENDAR_H
  #define CALENDAR_H

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
#include "Net.h"
#include "Graphics.h"

// ---------------------------------------------------------------------------
// Istanza del display (definita nello sketch .ino). Va dichiarata a livello
// globale perchè il simbolo lato .ino non vive in alcun namespace.
// Il tipo concreto del pannello e' scelto dal dispatcher Layout.h.
// ---------------------------------------------------------------------------
extern GxEPD2_3C<Layout::Panel, Layout::PAGE_HEIGHT> display;

// ---------------------------------------------------------------------------
// Configurazione non-segreta del modulo. Non sta in Env.h perchè non
// sono credenziali: sono parametri di dominio che l'utente modifica
// direttamente qui.
// ---------------------------------------------------------------------------

/**
 * Il fuso orario NON sta qui: e' dell'orologio di sistema, quindi vive in
 * Clock.h come CLOCK_POSIX_TZ e lo applica Clock::begin(). Tutte le
 * localtime_r() di questo file presuppongono quella chiamata, fatta in setup().
 */

/**
 * Tenant Azure AD per Microsoft Graph. Usato nella URL del token
 * endpoint. "common" consente sia account personali Microsoft sia
 * account organizzativi; altrimenti si puo' mettere il GUID del
 * tenant specifico per limitare l'accesso.
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
 * STA gia' connessa: la radio è gestita dallo sketch .ino.
 *
 * API:
 *  - Calendar::draw()            : riquadro mese corrente + lista eventi;
 *  - namespace Calendar::Outlook : scheduler/fetch eventi Microsoft Graph;
 *  - namespace Calendar::Google  : scheduler/fetch eventi Google Calendar.
 *
 * Usato da: Weather.h (render) e ePaper-weather-dashboard.ino (fetch + init).
 */
namespace Calendar
{
  /**
   * Numero di eventi mostrati: il massimo fra 7, 6 e 5 che lascia ogni riga
   * alta almeno MIN_EVT_ROW_H. Sotto i 45 px l'orario in alto e la data in
   * basso si avvicinano al titolo fino a toccarlo. Con la geometria attuale
   * il 9.7" mostra 5 righe da 46 px e il 12.2" 7 righe da 46 px.
   *
   * Le sorgenti (Outlook, Google) alimentano ciascuna una propria cache
   * da MAX_EVENTS=5; la lista mostrata e' il merge ordinato dei piu'
   * vicini all'orario corrente, ritagliato a MAX_EVENTS_DISPLAYED.
   */
  static constexpr int16_t MIN_EVT_ROW_H = 45;
  static constexpr uint8_t MAX_EVENTS_DISPLAYED =
      (Layout::EVT_H / 7 >= MIN_EVT_ROW_H) ? 7 :
      (Layout::EVT_H / 6 >= MIN_EVT_ROW_H) ? 6 : 5;
  static_assert(Layout::EVT_H / 5 >= MIN_EVT_ROW_H,
                "Calendar: l'area eventi non contiene nemmeno 5 righe da MIN_EVT_ROW_H");

  namespace Outlook
  {
    /** Numero di eventi cacheati da questa sorgente. */
    static constexpr uint8_t MAX_EVENTS = 5;

  }

  namespace Google
  {
    /** Numero di eventi cacheati da questa sorgente. */
    static constexpr uint8_t MAX_EVENTS = 5;

  }

  // -------------------------------------------------------------------------
  // Stato, layout e helper interni. Tutto dentro `detail` per non esporlo
  // fuori dal modulo.
  // -------------------------------------------------------------------------
  namespace detail
  {
    // -----------------------------------------------------------------------
    // Layout: tutte le costanti pixel (riquadro mese, area eventi) vivono
    // in Layout.h e sono accedute come Layout::* nel rendering. EVT_ROW_H
    // dipende dal numero di eventi visualizzati (Calendar-specific) quindi
    // resta calcolato qui dal Layout::EVT_H.
    // -----------------------------------------------------------------------
    inline constexpr int16_t EVT_ROW_H = Layout::EVT_H / Calendar::MAX_EVENTS_DISPLAYED;

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

    inline CalEvent  googleEvents[Calendar::Google::MAX_EVENTS];
    inline String    cachedGoogleToken;
    inline uint32_t  googleTokenExpiresAtMs  = 0;

    // =======================================================================
    // Helpers generici
    // =======================================================================

    inline bool isLeapY(int year)
    {
      return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    }

    /**
     * Converte componenti UTC (Y,M,D,h,m,s) in epoch UNIX.
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
     * Parse di una stringa ISO 8601 con offset opzionale (Z, +HH:MM,
     * -HH:MM) e la converte in epoch UTC. Se non c'è offset esplicito
     * interpreta come UTC (comportamento atteso per Outlook con
     * `Prefer: outlook.timezone=UTC`).
     *
     * Formati accettati:
     *   "2026-04-21T14:00:00"
     *   "2026-04-21T14:00:00Z"
     *   "2026-04-21T14:00:00+02:00"
     *   "2026-04-21T14:00:00.123Z"
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
     * Parse "YYYY-MM-DD" (evento all-day) come mezzanotte UTC del giorno.
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
     * Ricava un epoch UTC "adesso" affidabile: RTC di sistema se sincronizzato,
     * altrimenti 0 come sentinella.
     */
    inline time_t nowUtcEpoch()
    {
      time_t t = time(nullptr);
      return (t >= TIME_VALID_EPOCH_MIN) ? t : 0;
    }

    /**
     * Formatta un epoch UTC in stringa ISO 8601 "YYYY-MM-DDTHH:MM:SSZ".
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
     * RIUSO ESTERNO: Mail.h la raggiunge attraverso refreshGoogleToken(), con
     * cui condivide GOOGLE_CLIENT_ID/SECRET/REFRESH_TOKEN e la cache del
     * token: il refresh_token deve quindi essere stato emesso con scope
     * unificati calendar.readonly + gmail.readonly.
     *
     * @param tokenUrl     endpoint token
     * @param clientId     client id
     * @param clientSecret client secret ("" per client pubblici)
     * @param refreshToken refresh token
     * @param scopeEncoded scope gia' URL-encoded ("" per ereditarlo)
     * @param outToken     access token ricevuto
     * @param outExpiresIn durata in secondi dichiarata dall'IdP
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
      HTTPClient http;
      if (!Net::beginHttps(http, client, tokenUrl))
      {
        LOG("OAuth", "http.begin failed");
        return false;
      }
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");

      String body;
      body.reserve(512);
      body  = "client_id=";       body += clientId;
      if (clientSecret && *clientSecret)
        (body += "&client_secret=") += clientSecret;
      body += "&grant_type=refresh_token";
      body += "&refresh_token=";  body += refreshToken;
      if (scopeEncoded && *scopeEncoded)
        (body += "&scope=") += scopeEncoded;

      int code = http.POST(body);
      if (code != 200)
      {
        String resp = http.getString();
        LOG("OAuth", "token refresh failed: http=%d body=%s",
            code, resp.c_str());
        http.end();
        return false;
      }

      // Solo i due campi usati: la risposta Microsoft contiene anche un nuovo
      // refresh_token di oltre 1 KB che il documento non deve tenere.
      JsonDocument filter;
      filter["access_token"] = true;
      filter["expires_in"]   = true;

      JsonDocument doc;
      Net::BodyStream response(http.getStream());
      DeserializationError err = deserializeJson(doc, response, DeserializationOption::Filter(filter));
      http.end();
      if (err){
        LOG("OAuth", "token json parse: %s", err.c_str());
        return false; 
      }

      const char* tok = doc["access_token"] | "";
      if (!tok[0]){ 
        LOG("OAuth", "access_token mancante"); 
        return false; 
      }
      outToken     = tok;
      outExpiresIn = doc["expires_in"] | 3600UL;
      return true;
    }

    // =======================================================================
    // Microsoft Graph (Outlook): token + fetch eventi
    // =======================================================================

    /**
     * Rinnova l'access token Microsoft Graph se scaduto (margine 60 s).
     */
    inline bool refreshOutlookToken()
    {
      uint32_t now = millis();
      if (cachedOutlookToken.length() > 0 && (int32_t)(outlookTokenExpiresAtMs - now) > 0)
        return true;

      static const char* const TOKEN_URL =
        "https://login.microsoftonline.com/" CAL_MSGRAPH_TENANT_ID "/oauth2/v2.0/token";
      uint32_t expiresIn = 0;
      bool ok = oauthRefreshGeneric(
        TOKEN_URL, MSGRAPH_CLIENT_ID, "",
        MSGRAPH_REFRESH_TOKEN,
        "https%3A%2F%2Fgraph.microsoft.com%2FCalendars.Read%20offline_access",
        cachedOutlookToken, expiresIn);
      if (!ok) 
        return false;

      outlookTokenExpiresAtMs = millis() + (expiresIn > 60 ? expiresIn - 60 : expiresIn) * 1000UL;
      LOG("Outlook", "token OK, expires in %lus", (unsigned long)expiresIn);
      return true;
    }

    /**
     * GET /me/events con filtro `end/dateTime ge <now>`, top=MAX_EVENTS=5,
     * ordinato per inizio. Popola `outlookEvents[]` e invalida gli slot
     * residui, quindi la cache rispecchia sempre la risposta del server.
     * Ritorna true quando HTTP e parsing riescono, anche senza eventi:
     * false e' riservato ai guasti, cosi' il chiamante sa che la cache
     * precedente e' ancora quella buona.
     */
    inline bool fetchOutlookEvents()
    {
      time_t now = nowUtcEpoch();
      char nowIso[24] = "1970-01-01T00:00:00Z";
      if (now > 0) formatIsoUtc(now, nowIso, sizeof(nowIso));

      WiFiClientSecure client;
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

      if (!Net::beginHttps(http, client, url)) { LOG("Outlook", "events http.begin failed"); return false; }
      http.addHeader("Authorization", String("Bearer ") + cachedOutlookToken);
      http.addHeader("Prefer", "outlook.timezone=\"UTC\"");

      int code = http.GET();
      if (code != 200)
      {
        LOG("Outlook", "events fetch failed: http=%d", code);
        http.end();
        if (code == 401) { 
          cachedOutlookToken = ""; 
          outlookTokenExpiresAtMs = 0; 
        }
        return false;
      }

      // Solo i campi usati: Graph aggiunge comunque id, @odata.etag e timeZone,
      // circa 1 KB su cinque eventi che il documento non deve tenere.
      JsonDocument filter;
      filter["value"][0]["subject"]             = true;
      filter["value"][0]["start"]["dateTime"]   = true;
      filter["value"][0]["end"]["dateTime"]     = true;
      filter["value"][0]["isAllDay"]            = true;

      JsonDocument doc;
      Net::BodyStream body(http.getStream());
      DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
      http.end();
      if (err) { LOG("Outlook", "events json parse: %s", err.c_str()); return false; }

      JsonArrayConst value = doc["value"].as<JsonArrayConst>();
      if (value.isNull()) { LOG("Outlook", "value mancante"); return false; }

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
        Graphics::foldToFontCharset(e.title);
        e.startUtc = startUtc;
        e.endUtc   = endUtc;
        e.allDay   = allDay;
        e.valid    = true;
        n++;
      }
      for (int i = n; i < (int)Calendar::Outlook::MAX_EVENTS; i++) 
        outlookEvents[i].valid = false;

      LOG("Outlook", "fetched %d events (end>='%s')", n, nowIso);
      // Successo anche con n == 0: HTTP e parsing sono riusciti, quindi
      // outlookEvents[] rispecchia il calendario e un'agenda vuota e' un
      // risultato valido, non un errore da ritentare.
      return true;
    }

    // =======================================================================
    // Google Calendar API v3: token + fetch eventi
    //
    // SHARED CON MAIL: cachedGoogleToken e googleTokenExpiresAtMs sono la
    // SOLA cache di access_token Google del progetto. Mail.h
    // (Mail::detail::refreshToken / Mail::detail::bearer) riusa questa
    // cache invece di mantenerne una propria: 1 sola POST al token
    // endpoint per ciclo, backoff coerente, accoppiamento accettato perche'
    // condividono lo stesso refresh_token e gli stessi scope.
    // =======================================================================

    /**
     * Rinnova l'access token Google se scaduto (margine 60 s).
     * Aggiorna SOLO cachedGoogleToken e googleTokenExpiresAtMs: nessuna
     * struttura specifica Mail viene toccata, il modulo Mail legge le stesse
     * variabili dopo che questa funzione torna true.
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
      if (!ok) 
        return false;

      googleTokenExpiresAtMs = millis() + (expiresIn > 60 ? expiresIn - 60 : expiresIn) * 1000UL;
      LOG("Google", "token OK, expires in %lus", (unsigned long)expiresIn);
      return true;
    }

    /**
     * GET /calendar/v3/calendars/primary/events con timeMin=<now>,
     * maxResults=MAX_EVENTS=5, singleEvents=true, orderBy=startTime.
     * timeMin su Google tiene in lista anche gli eventi in corso (termine
     * nel futuro). Stesso contratto di ritorno di fetchOutlookEvents():
     * true su risposta valida, anche vuota; false solo sui guasti.
     */
    inline bool fetchGoogleEvents()
    {
      time_t now = nowUtcEpoch();
      char nowIso[24] = "1970-01-01T00:00:00Z";
      if (now > 0) 
        formatIsoUtc(now, nowIso, sizeof(nowIso));

      WiFiClientSecure client;
      HTTPClient http;

      // Percent-encode dei ':' del solo timeMin: applicato all'URL intero
      // toglierebbe anche quello di "https:", e begin() non troverebbe più il
      // protocollo.
      String timeMin = nowIso;
      timeMin.replace(":", "%3A");

      String url;
      url.reserve(360);
      url  = "https://www.googleapis.com/calendar/v3/calendars/primary/events?maxResults=";
      url += (int)Calendar::Google::MAX_EVENTS;
      url += "&orderBy=startTime&singleEvents=true&timeMin=";
      url += timeMin;
      // Solo i campi usati: un evento con descrizione o partecipanti lunghi
      // altrimenti esaurirebbe la memoria del parser a ogni tentativo.
      url += "&fields=items(summary,start,end)";

      if (!Net::beginHttps(http, client, url)) { LOG("Google", "events http.begin failed"); return false; }
      http.addHeader("Authorization", String("Bearer ") + cachedGoogleToken);

      int code = http.GET();
      if (code != 200)
      {
        LOG("Google", "events fetch failed: http=%d", code);
        http.end();
        if (code == 401) { cachedGoogleToken = ""; googleTokenExpiresAtMs = 0; }
        return false;
      }

      JsonDocument doc;
      Net::BodyStream body(http.getStream());
      DeserializationError err = deserializeJson(doc, body);
      http.end();
      if (err) { LOG("Google", "events json parse: %s", err.c_str()); return false; }

      // Con fields= un calendario senza eventi può omettere items: con HTTP 200
      // e JSON valido è un'agenda vuota, non un guasto.
      JsonArrayConst items = doc["items"].as<JsonArrayConst>();

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
        Graphics::foldToFontCharset(e.title);
        e.startUtc = startUtc;
        e.endUtc   = endUtc;
        e.allDay   = allDay;
        e.valid    = true;
        n++;
      }
      for (int i = n; i < (int)Calendar::Google::MAX_EVENTS; i++) googleEvents[i].valid = false;

      LOG("Google", "fetched %d events (timeMin='%s')", n, nowIso);
      // Successo anche con n == 0, per gli stessi motivi di fetchOutlookEvents().
      return true;
    }

    // =======================================================================
    // Rendering: scheda della sidebar (mese, griglia, filetto, eventi)
    // =======================================================================

    /**
     * Disegna la cornice della scheda con il titolo sul bordo superiore e il
     * filetto che separa la griglia del mese dagli eventi. Il fondo è già
     * bianco: il template azzera il buffer a ogni pagina e nella colonna
     * destra non disegna nient'altro.
     */
    inline void drawCard(const char* title)
    {
      display.setFont(Layout::FONT_TITLE);
      Graphics::drawFieldsetRect(Layout::CAL_X, Layout::CAL_Y, Layout::CAL_W, Layout::CAL_H,
                                 Layout::BOX_RADIUS, title, Layout::TITLE_INSET, GxEPD_WHITE);
      display.drawFastHLine(Layout::CAL_X + Layout::BOX_PAD, Layout::CAL_DIVIDER_Y,
                            Layout::CAL_W - 2 * Layout::BOX_PAD, GxEPD_BLACK);
    }

    /**
     * Disegna la griglia del mese corrente dentro la scheda: intestazione dei
     * giorni, filetto, sei righe di settimane da lunedì e cella odierna in
     * rosso. Sei righe bastano sempre: un mese di 31 giorni che inizia di
     * sabato occupa sei settimane.
     */
    inline void drawMonthGrid(const struct tm& today)
    {
      const int todayDay   = today.tm_mday;
      const int todayMonth = today.tm_mon;
      const int todayYear  = today.tm_year + 1900;
      const int todayWday  = today.tm_wday;

      int daysInMonth = DAYS_IN_MONTH[todayMonth];
      if (todayMonth == 1 && isLeapY(todayYear)) daysInMonth = 29;

      int firstWdaySun = ((todayWday - (todayDay - 1)) % 7 + 7) % 7;
      int firstCol     = (firstWdaySun + 6) % 7;

      const int16_t gridX = Layout::CAL_X + Layout::CAL_PAD;
      const int16_t gridW = Layout::CAL_W - 2 * Layout::CAL_PAD;
      const int16_t cellW = gridW / 7;
      const int16_t cellH = Layout::CAL_CELL_H;

      display.setFont(Layout::FONT_BODY);
      if (Graphics::intersectsPage(Layout::CAL_GRID_Y, Layout::CAL_HDR_H))
      {
        for (int i = 0; i < 7; i++)
        {
          int16_t cx = gridX + i * cellW + cellW / 2;
          Graphics::drawCentered(DOW_IT[i], cx, Layout::CAL_GRID_Y + Layout::CAL_HDR_BASELINE, GxEPD_BLACK);
        }
      }

      display.drawFastHLine(Layout::CAL_X + Layout::BOX_PAD, Layout::CAL_GRID_Y + Layout::CAL_HDR_H,
                            Layout::CAL_W - 2 * Layout::BOX_PAD, GxEPD_BLACK);

      const int16_t gridTop = Layout::CAL_GRID_Y + Layout::CAL_HDR_H + Layout::CAL_GRID_TOP_PAD;
      char buf[4];
      for (int d = 1; d <= daysInMonth; d++)
      {
        int idx = firstCol + (d - 1);
        int row = idx / 7;
        int col = idx % 7;
        int16_t cellX = gridX + col * cellW;
        int16_t cellY = gridTop + row * cellH;
        int16_t cx    = cellX + cellW / 2;
        // Numero e badge rosso stanno dentro la cella: fuori pagina non si formatta nemmeno.
        if (!Graphics::intersectsPage(cellY, cellH)) continue;

        snprintf(buf, sizeof(buf), "%d", d);

        /**
         * Baseline dall'altezza del numero, che per le cifre coincide con la
         * cap height: il numero resta centrato nella cella e il badge rosso
         * simmetrico attorno a lui.
         */
        int16_t  tx1, ty1;
        uint16_t tw, th;
        display.getTextBounds(buf, 0, 0, &tx1, &ty1, &tw, &th);
        const int16_t baseY = cellY + (cellH + (int16_t)th) / 2;

        if (d == todayDay)
        {
          display.fillRoundRect(cellX + 2, cellY + 1, cellW - 4, cellH - 2, Layout::CAL_CELL_R, GxEPD_RED);
          Graphics::drawCentered(buf, cx, baseY, GxEPD_WHITE);
        }
        else
        {
          Graphics::drawCentered(buf, cx, baseY, GxEPD_BLACK);
        }
      }
    }

    // =======================================================================
    // Rendering: lista eventi (merge Outlook + Google)
    // =======================================================================

    /**
     * Disegna una singola riga di evento (o placeholder se `e.valid==false`).
     * Colore: rosso se l'evento cade nella data odierna locale, nero altrimenti;
     * con todayY a 0, cioè senza ora valida, nessun evento risulta di oggi.
     */
    inline void drawEventRow(int row, const CalEvent& e,
                             int todayY, int todayM, int todayD)
    {
      const int16_t rowTop        = Layout::EVT_Y + row * EVT_ROW_H;
      const int16_t titleBaseline = rowTop + EVT_ROW_H / 2 + Layout::EVT_TITLE_FROM_MIDDLE;

      // Filetto, testi e discendenti stanno dentro la riga: fuori pagina non c'è niente da fare.
      if (!Graphics::intersectsPage(rowTop, EVT_ROW_H)) return;

      if (row > 0)
        display.drawFastHLine(Layout::EVT_X, rowTop, Layout::EVT_W, GxEPD_BLACK);

      display.setFont(Layout::FONT_BODY);

      if (!e.valid)
      {
        Graphics::drawLeftAligned("--", Layout::EVT_X, titleBaseline, GxEPD_BLACK);
        return;
      }

      /**
       * Localtime secondo il TZ di sistema, applicato da Clock::begin():
       * gestisce automaticamente il passaggio CET <-> CEST.
       */
      struct tm tmL;
      localtime_r(&e.startUtc, &tmL);

      bool isToday = (tmL.tm_year + 1900 == todayY) &&
                     (tmL.tm_mon + 1      == todayM) &&
                     (tmL.tm_mday         == todayD);
      uint16_t color = isToday ? GxEPD_RED : GxEPD_BLACK;

      // strftime() e non snprintf(): formatta i campi di struct tm, quindi il
      // compilatore non stima date da 11 cifre e non dà falsi -Wformat-truncation.
      char dateBuf[12];
      strftime(dateBuf, sizeof(dateBuf), (tmL.tm_year + 1900 == todayY) ? "%d/%m" : "%d/%m/%y", &tmL);

      char timeBuf[10];
      if (e.allDay) snprintf(timeBuf, sizeof(timeBuf), "tutto gg");
      else          strftime(timeBuf, sizeof(timeBuf), "%H:%M", &tmL);

      const int16_t dateW     = Graphics::textWidth(dateBuf);
      const int16_t timeW     = Graphics::textWidth(timeBuf);
      const int16_t rightColW = dateW > timeW ? dateW : timeW;
      const int16_t rightX    = Layout::EVT_X + Layout::EVT_W;

      char titleBuf[sizeof(e.title)];
      strncpy(titleBuf, e.title, sizeof(titleBuf) - 1);
      titleBuf[sizeof(titleBuf) - 1] = 0;
      Graphics::truncateToWidth(titleBuf, Layout::EVT_W - rightColW - Layout::EVT_RIGHT_GAP);
      Graphics::drawLeftAligned(titleBuf, Layout::EVT_X, titleBaseline, color);

      // Orario in alto e data in basso nella colonna destra della riga.
      Graphics::drawRightAligned(timeBuf, rightX, rowTop + Layout::EVT_TIME_BASELINE, color);
      Graphics::drawRightAligned(dateBuf, rightX, rowTop + EVT_ROW_H - Layout::EVT_DATE_FROM_BOTTOM, color);
    }

    /** Vero se l'evento è già terminato. Senza ora valida nessun evento lo è:
     *  meglio una lista con eventi passati che una lista svuotata a torto. */
    inline bool hasEnded(const CalEvent& ev, time_t nowUtc)
    {
      return nowUtc >= TIME_VALID_EPOCH_MIN && ev.endUtc <= nowUtc;
    }

    /**
     * Merge delle cache Outlook + Google (fino a 5+5=10 eventi validi),
     * insertion sort ascendente per startUtc, disegno delle prime
     * MAX_EVENTS_DISPLAYED righe (le piu' vicine all'orario attuale).
     * Gli eventi già terminati si saltano: la cache resta quella dell'ultimo
     * fetch riuscito, e con i fetch in errore li mostrerebbe come prossimi.
     */
    inline void drawEventsList(const struct tm& today, time_t nowUtc)
    {
      constexpr int CAP = Calendar::Outlook::MAX_EVENTS + Calendar::Google::MAX_EVENTS;
      const CalEvent* merged[CAP];
      int count = 0;

      for (auto& ev : outlookEvents)
        if (ev.valid && !hasEnded(ev, nowUtc) && count < CAP) merged[count++] = &ev;
      for (auto& ev : googleEvents)
        if (ev.valid && !hasEnded(ev, nowUtc) && count < CAP) merged[count++] = &ev;

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

      const bool timeValid = nowUtc >= TIME_VALID_EPOCH_MIN;
      const int todayY = timeValid ? today.tm_year + 1900 : 0;
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
   * Disegna la scheda della sidebar: il mese corrente come titolo, la sua
   * griglia e sotto il filetto la lista dei prossimi eventi (merge di
   * Outlook + Google), fino al banner meteo.
   *
   * I componenti locali vengono calcolati via localtime_r(), che
   * applica il TZ impostato da Clock::begin().
   *
   * Senza un'ora valida la scheda ha il titolo "--" e nessuna griglia: prima
   * di SNTP time() conta l'uptime dal 1970, e disegnarla mostrerebbe
   * "Gennaio 1970".
   *
   * @param utcEpoch epoch UTC di riferimento (0 = usa time(nullptr))
   */
  inline void draw(time_t utcEpoch)
  {
    if (utcEpoch == 0) utcEpoch = time(nullptr);

    // Componenti locali derivati dal TZ di sistema (Clock::begin()).
    struct tm tmNow;
    localtime_r(&utcEpoch, &tmNow);

    if (utcEpoch >= TIME_VALID_EPOCH_MIN)
    {
      detail::drawCard(detail::MESI_IT[tmNow.tm_mon]);
      detail::drawMonthGrid(tmNow);
    }
    else
    {
      detail::drawCard("--");
    }
    detail::drawEventsList(tmNow, utcEpoch);
  }

  /**
   * Sottosistema Outlook (Microsoft Graph). Cache dei prossimi
   * MAX_EVENTS eventi del calendario Outlook non ancora terminati;
   * refresh periodico via refresh-token + GET /me/events.
   */
  namespace Outlook
  {
    /**
     * Vero se Env.h contiene credenziali Microsoft vere e non quelle del
     * template. È la precondizione del task outlook nel .ino: senza, ogni slot
     * spenderebbe un handshake per un rifiuto certo.
     */
    inline bool isConfigured()
    {
      return Net::isCredentialSet(MSGRAPH_CLIENT_ID) && Net::isCredentialSet(MSGRAPH_REFRESH_TOKEN);
    }

    /** Azzera la cache. Una tantum in setup(). */
    inline void begin()
    {
      using namespace detail;
      for (auto& e : outlookEvents) { e.valid = false; e.title[0] = 0; e.startUtc = 0; e.endUtc = 0; e.allDay = false; }
      cachedOutlookToken      = "";
      outlookTokenExpiresAtMs = 0;
      if (!isConfigured())
        LOG("Outlook", "credenziali assenti in Env.h: calendario Outlook disattivato");
    }

    /**
     * Refresh del token + GET degli eventi. Presuppone STA connessa.
     * Le credenziali arrivano da Env.h (MSGRAPH_* define).
     *
     * Cadenza, ritenti e backoff sono dello scheduler: qui si esegue e basta.
     * @return true se la risposta e' stata acquisita, anche senza eventi.
     */
    inline bool runFetch()
    {
      return detail::refreshOutlookToken() && detail::fetchOutlookEvents();
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
   *
   * INTERAZIONE CON MAIL: Calendar::Google e Mail::* condividono lo stesso
   * GOOGLE_REFRESH_TOKEN e lo stesso access_token cached
   * (Calendar::detail::cachedGoogleToken). Le cadenze di fetch sono pero'
   * INDIPENDENTI: sono due task distinti dello scheduler, con la propria
   * cadenza e i propri tentativi. Il refresh del token e' invece
   * implicitamente condiviso, perche' ricade su una sola cache: chi dei due
   * gira per primo lo paga, e per questo la tabella mette mail prima di
   * Google.
   */
  namespace Google
  {
    /**
     * Vero se Env.h contiene credenziali Google vere e non quelle del
     * template. Vale anche per Mail, che usa le stesse credenziali.
     */
    inline bool isConfigured()
    {
      return Net::isCredentialSet(GOOGLE_CLIENT_ID) &&
             Net::isCredentialSet(GOOGLE_CLIENT_SECRET) &&
             Net::isCredentialSet(GOOGLE_REFRESH_TOKEN);
    }

    /** Azzera la cache. Una tantum in setup(). */
    inline void begin()
    {
      using namespace detail;
      for (auto& e : googleEvents) { e.valid = false; e.title[0] = 0; e.startUtc = 0; e.endUtc = 0; e.allDay = false; }
      cachedGoogleToken      = "";
      googleTokenExpiresAtMs = 0;
      if (!isConfigured())
        LOG("Google", "credenziali assenti in Env.h: calendario Google disattivato");
    }

    /**
     * Refresh del token + GET degli eventi. Presuppone STA connessa.
     * Stesso contratto di Outlook::runFetch(): esegue e basta, il quando lo
     * decide lo scheduler.
     * @return true se la risposta e' stata acquisita, anche senza eventi.
     */
    inline bool runFetch()
    {
      return detail::refreshGoogleToken() && detail::fetchGoogleEvents();
    }
  }
}

#endif

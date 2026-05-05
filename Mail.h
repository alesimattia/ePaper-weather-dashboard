#ifndef MAIL_H
#define MAIL_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <stdint.h>

#include "Env.h"
#include "Calendar.h" // riusa Calendar::detail::refreshGoogleToken / cachedGoogleToken

// ===========================================================================
// Configurazione (override-abili dal .ino prima di #include "Mail.h")
// ===========================================================================

/**
 * Cadenza fetch in minuti, INDIPENDENTE da CAL_GOOGLE_FETCH_MIN del calendario.
 * Tipicamente >= CAL_GOOGLE_FETCH_MIN per non moltiplicare i risvegli WiFi.
 */
#ifndef MAIL_GOOGLE_FETCH_MIN
  #define MAIL_GOOGLE_FETCH_MIN 15
#endif

/** Numero massimo di mail da scaricare/cachare. */
#ifndef MAIL_MAX_MESSAGES
  #define MAIL_MAX_MESSAGES 5
#endif

/**
 * 0 = ultime N in INBOX (lette o no, default).
 * 1 = ultime N non lette in INBOX (Gmail interpreta piu' labelIds come AND).
 */
#ifndef MAIL_ONLY_UNREAD
  #define MAIL_ONLY_UNREAD 0
#endif

/** Scope OAuth Gmail (gia' URL-encoded). */
#ifndef MAIL_GMAIL_SCOPE
  #define MAIL_GMAIL_SCOPE "https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fgmail.readonly"
#endif

/** Host base API Gmail (REST). */
#ifndef MAIL_GMAIL_HOST
  #define MAIL_GMAIL_HOST "https://gmail.googleapis.com"
#endif

/**
 * Endpoint batch Gmail: una sola POST multipart/mixed che impacchetta piu'
 * sub-request HTTP. Sostituisce N GET singoli con 1 sola connessione TLS.
 */
#ifndef MAIL_GMAIL_BATCH_URL
  #define MAIL_GMAIL_BATCH_URL "https://gmail.googleapis.com/batch/gmail/v1"
#endif

/** Lunghezza massima del campo "From" (mittente). */
#ifndef MAIL_SENDER_LEN
  #define MAIL_SENDER_LEN 64
#endif

/**
 * Lunghezza massima del campo "Subject". Cap a 60 char totali ('\0' incluso).
 * Subject piu' lunghi vengono troncati con strncpy + terminazione esplicita.
 */
#ifndef MAIL_SUBJECT_LEN
  #define MAIL_SUBJECT_LEN 60
#endif

/**
 * Tempo massimo (ms) end-to-end di Mail::runFetch(). Evita che un fetch mail
 * lento consumi la finestra WiFi a danno dei fetch calendario successivi:
 * oltre la soglia interrompe la fase metadata e lascia in cache le mail
 * gia' parseate (cache parziale, NON e' un errore).
 */
#ifndef MAIL_FETCH_BUDGET_MS
  #define MAIL_FETCH_BUDGET_MS 10000UL
#endif

/** Stesso budget di tentativi consecutivi falliti di Calendar. */
#ifndef MAX_CALENDAR_ATTEMPTS
  #define MAX_CALENDAR_ATTEMPTS 2
#endif

namespace Mail
{
  /**
   * Snapshot di una mail in cache (solo metadati).
   * Header "From" e "Subject" sono troncati ai limiti dei buffer.
   * receivedUtc viene da internalDate Gmail (epoch UTC, autoritativo).
   */
  struct MailMessage
  {
    char   sender [MAIL_SENDER_LEN];
    char   subject[MAIL_SUBJECT_LEN];
    time_t receivedUtc;
    bool   unread;
    bool   valid;
  };

  static constexpr size_t MAX_MESSAGES = MAIL_MAX_MESSAGES;

  namespace detail
  {
    /** Cache messaggi. Slot non popolati hanno valid=false. */
    inline MailMessage messages[MAX_MESSAGES];
    inline size_t      messagesCount  = 0;
    /**
     * NB: niente cache token locale. Mail riusa Calendar::detail::cachedGoogleToken
     * (un solo refresh per ciclo, backoff condiviso).
     */
    inline uint32_t lastFetchMs    = 0;
    inline bool     firstFetch     = true;
    /** Conta solo errori di rete sulle GET list/batch (non sul refresh). */
    inline uint8_t  failedAttempts = 0;

    /** Lookup non firmato del Bearer condiviso con Calendar::Google. */
    inline const String &bearer()
    {
      return Calendar::detail::cachedGoogleToken;
    }

    /**
     * Wrapper sul refresh condiviso. Se Calendar ha gia' un token valido in
     * questo ciclo, e' un no-op e ritorna true subito (no handshake TLS).
     */
    inline bool refreshToken()
    {
      return Calendar::detail::refreshGoogleToken();
    }

    /**
     * Estrae il valore di "boundary=" dall'header Content-Type
     * "multipart/mixed; boundary=...". Restituisce stringa vuota se assente.
     */
    inline String extractBoundary(const String &contentType)
    {
      int idx = contentType.indexOf("boundary=");
      if (idx < 0) return String();
      String b = contentType.substring(idx + 9);
      // boundary puo' essere quotato o terminato da ';' o spazi
      if (b.startsWith("\""))
      {
        int end = b.indexOf('"', 1);
        if (end > 0) return b.substring(1, end);
        return String();
      }
      int sep = b.indexOf(';');
      if (sep > 0) b = b.substring(0, sep);
      b.trim();
      return b;
    }

    /**
     * Costruisce il body multipart/mixed per la batch request.
     * Ogni sub-request e' una GET messages.get con format=metadata e
     * fields= che limita la risposta a internalDate, labelIds, headers.
     * Bearer NON ripetuto: il batch endpoint applica l'header esterno.
     */
    inline void buildBatchBody(const String ids[], size_t n,
                               const char *boundary, String &body)
    {
      body = "";
      body.reserve(2048);
      for (size_t i = 0; i < n; ++i)
      {
        body += "--";
        body += boundary;
        body += "\r\n";
        body += "Content-Type: application/http\r\n";
        body += "Content-ID: <item-";
        body += (unsigned)i;
        body += ">\r\n\r\n";
        body += "GET /gmail/v1/users/me/messages/";
        body += ids[i];
        body += "?format=metadata"
                "&metadataHeaders=From&metadataHeaders=Subject&metadataHeaders=Date"
                "&fields=internalDate%2ClabelIds%2Cpayload%2Fheaders%28name%2Cvalue%29\r\n\r\n";
      }
      body += "--";
      body += boundary;
      body += "--\r\n";
    }

    /**
     * Parsea una singola sub-response del batch (gia' separata dai boundary
     * esterni) e popola out. Il blocco contiene:
     *   <part headers>\r\n\r\n<inner HTTP status + headers>\r\n\r\n<JSON body>
     * Ritorna true solo se ha estratto un JSON valido con almeno un header.
     */
    inline bool parseBatchPart(const String &part, MailMessage &out)
    {
      // Scarta gli header della parte (Content-Type: application/http, ecc.)
      int p1 = part.indexOf("\r\n\r\n");
      if (p1 < 0) return false;
      // Scarta status line + header HTTP della sub-response
      int p2 = part.indexOf("\r\n\r\n", p1 + 4);
      if (p2 < 0) return false;
      String json = part.substring(p2 + 4);
      json.trim(); // rimuove eventuali \r\n residui prima del prossimo boundary

      // Filter ArduinoJson: scarta in fase di parse tutto cio' che non serve
      JsonDocument filter;
      filter["internalDate"]                   = true;
      filter["labelIds"]                       = true;
      filter["payload"]["headers"][0]["name"]  = true;
      filter["payload"]["headers"][0]["value"] = true;

      JsonDocument doc;
      DeserializationError err =
          deserializeJson(doc, json, DeserializationOption::Filter(filter));
      if (err)
      {
        Serial.printf("[Mail] sub-response json parse: %s\n", err.c_str());
        return false;
      }

      // internalDate e' una stringa di millisecondi epoch UTC
      const char *internalDateMs = doc["internalDate"] | "";
      out.receivedUtc = 0;
      if (internalDateMs[0])
      {
        unsigned long long ms = strtoull(internalDateMs, nullptr, 10);
        out.receivedUtc = (time_t)(ms / 1000ULL);
      }

      // labelIds[]: cerca "UNREAD"
      out.unread = false;
      JsonArrayConst labels = doc["labelIds"].as<JsonArrayConst>();
      if (!labels.isNull())
      {
        for (JsonVariantConst lbl : labels)
        {
          const char *s = lbl.as<const char *>();
          if (s && strcmp(s, "UNREAD") == 0)
          {
            out.unread = true;
            break;
          }
        }
      }

      // headers[]: estrae From e Subject (case-insensitive sul name)
      out.sender[0]  = '\0';
      out.subject[0] = '\0';
      JsonArrayConst headers = doc["payload"]["headers"].as<JsonArrayConst>();
      if (headers.isNull()) return false;
      for (JsonVariantConst h : headers)
      {
        const char *name  = h["name"]  | "";
        const char *value = h["value"] | "";
        if (!name[0] || !value[0]) continue;
        if (strcasecmp(name, "From") == 0)
        {
          strncpy(out.sender, value, MAIL_SENDER_LEN - 1);
          out.sender[MAIL_SENDER_LEN - 1] = '\0';
        }
        else if (strcasecmp(name, "Subject") == 0)
        {
          strncpy(out.subject, value, MAIL_SUBJECT_LEN - 1);
          out.subject[MAIL_SUBJECT_LEN - 1] = '\0';
        }
      }
      out.valid = (out.sender[0] || out.subject[0]);
      return out.valid;
    }

    /**
     * GET autenticata users/me/messages?... popola ids[] con fino a
     * MAIL_MAX_MESSAGES id. Usa fields=messages(id) per ridurre il payload.
     * Ritorna true anche se la lista e' vuota (outCount=0).
     */
    inline bool fetchMessageIds(String ids[], size_t &outCount)
    {
      outCount = 0;

      WiFiClientSecure client;
      client.setInsecure();

      HTTPClient http;
      String url;
      url.reserve(240);
      url  = MAIL_GMAIL_HOST;
      url += "/gmail/v1/users/me/messages?maxResults=";
      url += (int)MAIL_MAX_MESSAGES;
      url += "&labelIds=INBOX";
#if MAIL_ONLY_UNREAD
      url += "&labelIds=UNREAD";
#endif
      url += "&fields=messages(id)";

      if (!http.begin(client, url))
      {
        Serial.println(F("[Mail] list http.begin failed"));
        return false;
      }
      http.addHeader("Authorization", String("Bearer ") + bearer());

      int code = http.GET();
      if (code != 200)
      {
        Serial.printf("[Mail] list fetch failed: http=%d\n", code);
        http.end();
        // 401: invalida il token condiviso cosi' Calendar lo rinegozia
        if (code == 401)
        {
          Calendar::detail::cachedGoogleToken = "";
          Calendar::detail::googleTokenExpiresAtMs = 0;
        }
        return false;
      }

      JsonDocument filter;
      filter["messages"][0]["id"] = true;

      JsonDocument doc;
      DeserializationError err =
          deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
      http.end();
      if (err)
      {
        Serial.printf("[Mail] list json parse: %s\n", err.c_str());
        return false;
      }

      JsonArrayConst arr = doc["messages"].as<JsonArrayConst>();
      if (arr.isNull())
      {
        // inbox vuota o nessun match: non e' un errore
        Serial.println(F("[Mail] list vuota (nessuna mail)"));
        return true;
      }
      for (JsonVariantConst m : arr)
      {
        if (outCount >= MAX_MESSAGES) break;
        const char *id = m["id"] | "";
        if (!id[0]) continue;
        ids[outCount++] = id;
      }
      Serial.printf("[Mail] fetched %u message ids\n", (unsigned)outCount);
      return true;
    }

    /**
     * Una sola POST multipart al batch endpoint. Riduce N GET metadata
     * a 1 handshake TLS. Sub-response per-mail possono fallire singolarmente
     * senza bloccare le altre (best-effort: outCount puo' essere < n).
     */
    inline bool fetchMessagesBatch(const String ids[], size_t n,
                                   MailMessage outMsgs[], size_t &outCount)
    {
      outCount = 0;
      if (n == 0) return true;

      WiFiClientSecure client;
      client.setInsecure();

      HTTPClient http;
      if (!http.begin(client, MAIL_GMAIL_BATCH_URL))
      {
        Serial.println(F("[Mail] batch http.begin failed"));
        return false;
      }

      static const char *kReqBoundary = "batch_epd";
      http.addHeader("Authorization", String("Bearer ") + bearer());
      http.addHeader("Content-Type",
                     String("multipart/mixed; boundary=") + kReqBoundary);

      // Raccogliamo Content-Type della response per estrarre il boundary
      const char *headerKeys[] = {"Content-Type"};
      http.collectHeaders(headerKeys, 1);

      String body;
      buildBatchBody(ids, n, kReqBoundary, body);

      int code = http.POST(body);
      if (code != 200)
      {
        Serial.printf("[Mail] batch fetch failed: http=%d\n", code);
        http.end();
        if (code == 401)
        {
          Calendar::detail::cachedGoogleToken = "";
          Calendar::detail::googleTokenExpiresAtMs = 0;
        }
        return false;
      }

      String respCT = http.header("Content-Type");
      String respBoundary = extractBoundary(respCT);
      if (respBoundary.length() == 0)
      {
        Serial.println(F("[Mail] batch: boundary mancante in Content-Type"));
        http.end();
        return false;
      }

      // Body completo: ~3-4 KB con i fields= filter applicati. Sicuro su ESP32.
      String resp = http.getString();
      http.end();
      if (resp.length() == 0)
      {
        Serial.println(F("[Mail] batch: response vuota"));
        return false;
      }

      // Split sui boundary "--<boundary>"; ignora preamble e closing "--<boundary>--"
      String sep = String("--") + respBoundary;
      int searchFrom = 0;
      // Salta tutto prima della prima occorrenza del boundary
      int firstBoundary = resp.indexOf(sep);
      if (firstBoundary < 0)
      {
        Serial.println(F("[Mail] batch: nessun boundary nel body"));
        return false;
      }
      searchFrom = firstBoundary + sep.length();

      while (searchFrom < (int)resp.length() && outCount < MAX_MESSAGES)
      {
        // Closing boundary "--": fine multipart
        if (resp.length() >= (unsigned)searchFrom + 2 &&
            resp[searchFrom] == '-' && resp[searchFrom + 1] == '-')
          break;
        int nextBoundary = resp.indexOf(sep, searchFrom);
        if (nextBoundary < 0) break;

        String part = resp.substring(searchFrom, nextBoundary);
        MailMessage m{};
        if (parseBatchPart(part, m))
        {
          outMsgs[outCount++] = m;
        }
        searchFrom = nextBoundary + sep.length();
      }

      Serial.printf("[Mail] batch parsed %u/%u messaggi\n",
                    (unsigned)outCount, (unsigned)n);
      return true;
    }
  } // namespace detail

  /** Azzera la cache. Una tantum in setup(). */
  inline void begin()
  {
    using namespace detail;
    for (size_t i = 0; i < MAX_MESSAGES; ++i) messages[i].valid = false;
    messagesCount  = 0;
    lastFetchMs    = 0;
    firstFetch     = true;
    failedAttempts = 0;
  }

  /** Numero di mail attualmente in cache (0..MAX_MESSAGES). */
  inline size_t count() { return detail::messagesCount; }

  /**
   * Accesso read-only allo slot i (i < count()). Pensato per la futura UI;
   * il chiamante deve verificare count() prima.
   */
  inline const MailMessage &at(size_t i) { return detail::messages[i]; }

  /**
   * true al primo fetch oppure se sono passati MAIL_GOOGLE_FETCH_MIN minuti
   * dall'ultimo. Stesso pattern di Calendar::Google::pendingFetch().
   */
  inline bool pendingFetch()
  {
    using namespace detail;
    if (firstFetch) return true;
    uint32_t elapsed = millis() - lastFetchMs;
    return elapsed >= ((uint32_t)MAIL_GOOGLE_FETCH_MIN * 60UL * 1000UL);
  }

  /**
   * Scarica le ultime MAIL_MAX_MESSAGES mail (1 GET list + 1 POST batch).
   * Best-effort: WiFi assente / inbox vuota / errori sub-response NON
   * propagano errori al chiamante; il flusso software prosegue normalmente.
   * Wall-clock budget MAIL_FETCH_BUDGET_MS: oltre la soglia interrompe la
   * fase metadata.
   *
   * Richiamato da loop() del .ino subito prima dei fetch calendario.
   * Ritorna true se la cache e' stata aggiornata con successo (anche con 0 mail).
   */
  inline bool runFetch()
  {
    using namespace detail;

    uint32_t t0 = millis();

    if (!refreshToken())
    {
      Serial.println(F("[Mail] refresh token KO, skip fetch"));
      /**
       * Anche su refresh fallito incrementiamo failedAttempts: durante la
       * finestra OTA il loop() gira ogni ~10ms e senza questo backoff
       * Mail martellerebbe il token endpoint. La cache token e' condivisa
       * con Calendar::Google, ma il loro backoff e' a livello del rispettivo
       * runFetch(): ognuno deve gestire il proprio.
       */
      failedAttempts++;
      if (failedAttempts >= MAX_CALENDAR_ATTEMPTS)
      {
        failedAttempts = 0;
        lastFetchMs = millis();
        firstFetch = false;
      }
      return false;
    }
    if ((millis() - t0) >= MAIL_FETCH_BUDGET_MS)
    {
      Serial.println(F("[Mail] budget esaurito sul refresh, skip"));
      return false;
    }

    String ids[MAX_MESSAGES];
    size_t n = 0;
    if (!fetchMessageIds(ids, n))
    {
      failedAttempts++;
      if (failedAttempts >= MAX_CALENDAR_ATTEMPTS)
      {
        failedAttempts = 0;
        lastFetchMs = millis(); // posticipa retry di MAIL_GOOGLE_FETCH_MIN
        firstFetch = false;
      }
      return false;
    }

    if (n == 0)
    {
      // Inbox vuota: cache azzerata, NON e' un errore.
      for (size_t i = 0; i < MAX_MESSAGES; ++i) messages[i].valid = false;
      messagesCount = 0;
      lastFetchMs   = millis();
      firstFetch    = false;
      failedAttempts = 0;
      Serial.println(F("[Mail] cache azzerata (inbox vuota)"));
      return true;
    }

    if ((millis() - t0) >= MAIL_FETCH_BUDGET_MS)
    {
      Serial.println(F("[Mail] budget esaurito prima del batch, skip"));
      return false;
    }

    // Azzera la cache prima del batch: se il batch fallisce, cache rimane vuota
    // (preferibile a mostrare dati stale di un fetch precedente).
    for (size_t i = 0; i < MAX_MESSAGES; ++i) messages[i].valid = false;
    messagesCount = 0;

    if (!fetchMessagesBatch(ids, n, messages, messagesCount))
    {
      failedAttempts++;
      if (failedAttempts >= MAX_CALENDAR_ATTEMPTS)
      {
        failedAttempts = 0;
        lastFetchMs = millis();
        firstFetch = false;
      }
      return false;
    }

    lastFetchMs    = millis();
    firstFetch     = false;
    failedAttempts = 0;

    // Log riassuntivo per diagnosi (i campi rispettano i cap MAIL_*_LEN)
    for (size_t i = 0; i < messagesCount; ++i)
    {
      const MailMessage &m = messages[i];
      Serial.printf("[Mail] %u%s From=%s Subject=%s ts=%lld\n",
                    (unsigned)i, m.unread ? " *" : "  ",
                    m.sender, m.subject, (long long)m.receivedUtc);
    }
    return true;
  }
} // namespace Mail

#endif // MAIL_H

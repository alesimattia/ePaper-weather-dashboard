// Guardia diversa da MAIL_H, che è il nome della costante Layout::MAIL_H.
#ifndef MAIL_MODULE_H
#define MAIL_MODULE_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <stdint.h>

#include "Env.h"
#include "Timings.h"
#include "Log.h"
#include "Net.h"
#include "Calendar.h" // riusa Calendar::detail::refreshGoogleToken / cachedGoogleToken
                      // e include transitivamente Layout.h + display globale
#include "icons.h"    // INDOOR_ICON_MAIL (busta 20x20)

// ===========================================================================
// Configurazione (override-abili dal .ino prima di #include "Mail.h")
// ===========================================================================

/**
 * Numero massimo di mail da scaricare e tenere in cache. Il default coincide
 * con le celle del riquadro Posta, Layout::MAIL_MAX (4 sul 097c, 6 sul 122c).
 * Un override dal .ino con #define MAIL_MAX_MESSAGES N cambia solo quante
 * mail si scaricano: il riquadro ne disegna al massimo Layout::MAIL_MAX.
 */
#ifndef MAIL_MAX_MESSAGES
  #define MAIL_MAX_MESSAGES Layout::MAIL_MAX
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
        LOG("Mail", "sub-response json parse: %s", err.c_str());
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
          /**
           * Estrae SOLO l'indirizzo email dal valore "Nome Cognome <user@x>"
           * (richiesta utente: niente nome, solo email). Se il valore non
           * contiene < >, si assume sia gia' un indirizzo nudo e si copia
           * tutto. Robust su quote / spazi: cerca l'ultima coppia '<'..'>'
           * per gestire correttamente nomi che contengono '<' letterali.
           */
          const char *lt = strrchr(value, '<');
          const char *gt = strrchr(value, '>');
          if (lt && gt && gt > lt + 1)
          {
            size_t n = (size_t)(gt - lt - 1);
            if (n >= MAIL_SENDER_LEN) n = MAIL_SENDER_LEN - 1;
            memcpy(out.sender, lt + 1, n);
            out.sender[n] = '\0';
          }
          else
          {
            strncpy(out.sender, value, MAIL_SENDER_LEN - 1);
            out.sender[MAIL_SENDER_LEN - 1] = '\0';
          }
        }
        else if (strcasecmp(name, "Subject") == 0)
        {
          strncpy(out.subject, value, MAIL_SUBJECT_LEN - 1);
          out.subject[MAIL_SUBJECT_LEN - 1] = '\0';
        }
      }
      Graphics::foldToFontCharset(out.sender);
      Graphics::foldToFontCharset(out.subject);
      out.valid = (out.sender[0] || out.subject[0]);
      return out.valid;
    }

    /**
     * GET autenticata users/me/messages?... popola ids[] con fino a
     * MAIL_MAX_MESSAGES id. Usa fields=messages(id) per ridurre il payload.
     * Ritorna true anche se la lista e' vuota (outCount=0).
     *
     * Client e richiesta arrivano da runFetch() e restano aperti per il batch,
     * che va sullo stesso host: una sola negoziazione TLS per fetch. Il corpo
     * si legge con getString() perchè Gmail in HTTP/1.1 risponde chunked, e
     * lo stream grezzo conterrebbe le intestazioni dei chunk.
     */
    inline bool fetchMessageIds(HTTPClient &http, WiFiClientSecure &client,
                                String ids[], size_t &outCount)
    {
      outCount = 0;

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
        LOG("Mail", "list http.begin failed");
        return false;
      }
      http.addHeader("Authorization", String("Bearer ") + bearer());

      int code = http.GET();
      if (code != 200)
      {
        LOG("Mail", "list fetch failed: http=%d", code);
        http.end();
        // 401: invalida il token condiviso cosi' Calendar lo rinegozia
        if (code == 401)
        {
          Calendar::detail::cachedGoogleToken = "";
          Calendar::detail::googleTokenExpiresAtMs = 0;
        }
        return false;
      }

      const String body = http.getString();
      http.end();

      JsonDocument filter;
      filter["messages"][0]["id"] = true;

      JsonDocument doc;
      DeserializationError err =
          deserializeJson(doc, body, DeserializationOption::Filter(filter));
      if (err)
      {
        LOG("Mail", "list json parse: %s", err.c_str());
        return false;
      }

      JsonArrayConst arr = doc["messages"].as<JsonArrayConst>();
      if (arr.isNull())
      {
        // inbox vuota o nessun match: non e' un errore
        LOG("Mail", "list vuota (nessuna mail)");
        return true;
      }
      for (JsonVariantConst m : arr)
      {
        if (outCount >= MAX_MESSAGES) break;
        const char *id = m["id"] | "";
        if (!id[0]) continue;
        ids[outCount++] = id;
      }
      LOG("Mail", "fetched %u message ids", (unsigned)outCount);
      return true;
    }

    /**
     * Una sola POST multipart al batch endpoint. Riduce N GET metadata
     * a 1 handshake TLS. Sub-response per-mail possono fallire singolarmente
     * senza bloccare le altre (best-effort: outCount puo' essere < n).
     * Riusa la connessione aperta da fetchMessageIds() quando il server l'ha
     * tenuta viva; altrimenti HTTPClient si riconnette da sè.
     */
    inline bool fetchMessagesBatch(HTTPClient &http, WiFiClientSecure &client,
                                   const String ids[], size_t n,
                                   MailMessage outMsgs[], size_t &outCount)
    {
      outCount = 0;
      if (n == 0) return true;

      if (!http.begin(client, MAIL_GMAIL_BATCH_URL))
      {
        LOG("Mail", "batch http.begin failed");
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
        LOG("Mail", "batch fetch failed: http=%d", code);
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
        LOG("Mail", "batch: boundary mancante in Content-Type");
        http.end();
        return false;
      }

      // Body completo: ~3-4 KB con i fields= filter applicati. Sicuro su ESP32.
      String resp = http.getString();
      http.end();
      if (resp.length() == 0)
      {
        LOG("Mail", "batch: response vuota");
        return false;
      }

      // Split sui boundary "--<boundary>"; ignora preamble e closing "--<boundary>--"
      String sep = String("--") + respBoundary;
      int searchFrom = 0;
      // Salta tutto prima della prima occorrenza del boundary
      int firstBoundary = resp.indexOf(sep);
      if (firstBoundary < 0)
      {
        LOG("Mail", "batch: nessun boundary nel body");
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

      LOG("Mail", "batch parsed %u/%u messaggi",
          (unsigned)outCount, (unsigned)n);
      return true;
    }
  } // namespace detail

  /** Vero se le credenziali Google di Env.h sono configurate: Mail usa le
   *  stesse di Calendar::Google. È la precondizione del task mail nel .ino. */
  inline bool isConfigured() { return Calendar::Google::isConfigured(); }

  /** Azzera la cache. Una tantum in setup(). */
  inline void begin()
  {
    using namespace detail;
    for (size_t i = 0; i < MAX_MESSAGES; ++i) messages[i].valid = false;
    messagesCount = 0;
    if (!isConfigured())
      LOG("Mail", "credenziali Google assenti in Env.h: posta disattivata");
  }

  // ==========================================================================
  // Rendering UI
  // ==========================================================================

  namespace detail
  {
    /**
     * Disegna una mail nella sua cella: mittente e oggetto a sinistra, orario
     * e data a destra, e la busta rossa accanto al mittente quando la mail non
     * è letta. Le celle si riempiono per colonna, dall'alto in basso, così la
     * lettura resta cronologica: la cache tiene in testa la mail più recente.
     */
    inline void drawMailCell(int idx, const MailMessage& m)
    {
      const int16_t col    = idx / Layout::MAIL_ROWS_PER_COL;
      const int16_t row    = idx % Layout::MAIL_ROWS_PER_COL;
      const int16_t cellX  = Layout::MAIL_X + col * Layout::MAIL_COL_W;
      const int16_t rowTop = Layout::MAIL_BODY_Y + row * Layout::MAIL_ROW_H;
      const int16_t left   = cellX + Layout::BOX_PAD;
      const int16_t right  = cellX + Layout::MAIL_COL_W - Layout::BOX_PAD;
      const int16_t line1  = rowTop + Layout::MAIL_LINE1_BASELINE;
      const int16_t line2  = line1 + Layout::MAIL_LINE_GAP;

      if (row > 0)
        display.drawFastHLine(left, rowTop, right - left, GxEPD_BLACK);

      display.setFont(Layout::FONT_BODY);
      if (!m.valid)
      {
        Graphics::drawLeftAligned("--", left, line1, GxEPD_BLACK);
        return;
      }

      // Senza internalDate l'orario non è noto: "--" invece di una data inventata.
      char timeBuf[8], dateBuf[8];
      if (m.receivedUtc > 0)
      {
        struct tm tmL;
        localtime_r(&m.receivedUtc, &tmL);
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", tmL.tm_hour, tmL.tm_min);
        snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d", tmL.tm_mday, tmL.tm_mon + 1);
      }
      else
      {
        strcpy(timeBuf, "--");
        strcpy(dateBuf, "--");
      }
      const int16_t timeW     = Graphics::textWidth(timeBuf);
      const int16_t dateW     = Graphics::textWidth(dateBuf);
      const int16_t rightColW = timeW > dateW ? timeW : dateW;
      Graphics::drawRightAligned(timeBuf, right, line1, GxEPD_BLACK);
      Graphics::drawRightAligned(dateBuf, right, line2, GxEPD_BLACK);

      const int16_t textMaxW    = right - left - rightColW - Layout::MAIL_RIGHT_GAP;
      const int16_t iconReserve = m.unread ? (Layout::SMALL_ICON_SIZE + Layout::SMALL_ICON_GAP) : 0;

      char senderBuf[MAIL_SENDER_LEN];
      strncpy(senderBuf, m.sender, sizeof(senderBuf) - 1);
      senderBuf[sizeof(senderBuf) - 1] = 0;
      Graphics::truncateToWidth(senderBuf, textMaxW - iconReserve);
      Graphics::drawLeftAligned(senderBuf, left, line1, GxEPD_BLACK);

      if (m.unread)
      {
        const int16_t iconX = left + Graphics::textWidth(senderBuf) + Layout::SMALL_ICON_GAP;
        display.drawBitmap(iconX, Graphics::smallIconY(line1), INDOOR_ICON_MAIL,
                           Layout::SMALL_ICON_SIZE, Layout::SMALL_ICON_SIZE, GxEPD_RED);
      }

      char subjectBuf[MAIL_SUBJECT_LEN];
      strncpy(subjectBuf, m.subject, sizeof(subjectBuf) - 1);
      subjectBuf[sizeof(subjectBuf) - 1] = 0;
      display.setFont(Layout::FONT_SMALL);
      Graphics::truncateToWidth(subjectBuf, textMaxW);
      Graphics::drawLeftAligned(subjectBuf, left, line2, GxEPD_BLACK);
    }
  } // namespace detail

  /**
   * Disegna il riquadro "Posta" con le Layout::MAIL_MAX celle della griglia,
   * separate da un divisore verticale fra le colonne e da filetti fra le
   * righe. Le celle oltre le mail in cache mostrano "--"; con una cache più
   * grande della griglia (MAIL_MAX_MESSAGES ridefinito) si disegnano solo le
   * prime, invece di uscire dal riquadro.
   *
   * Va chiamata dentro il loop paged di Weather::renderFrame(), fra
   * Calendar::draw e drawBanner: l'area è già bianca grazie a fillScreen.
   */
  inline void draw()
  {
    display.setFont(Layout::FONT_TITLE);
    Graphics::drawFieldsetRect(Layout::MAIL_X, Layout::MAIL_Y, Layout::MAIL_W, Layout::MAIL_H,
                               Layout::BOX_RADIUS, "Posta", Layout::TITLE_INSET, GxEPD_WHITE);
    const int16_t gridH = Layout::MAIL_ROWS_PER_COL * Layout::MAIL_ROW_H;
    display.drawFastVLine(Layout::MAIL_X + Layout::MAIL_COL_W, Layout::MAIL_BODY_Y + 4, gridH - 8, GxEPD_BLACK);

    const MailMessage empty{};
    for (size_t i = 0; i < (size_t)Layout::MAIL_MAX; ++i)
    {
      const MailMessage& m = (i < detail::messagesCount) ? detail::messages[i] : empty;
      detail::drawMailCell((int)i, m);
    }
  }

  /**
   * Scarica le ultime MAIL_MAX_MESSAGES mail (1 GET list + 1 POST batch).
   *
   * GARANZIE DI RESILIENZA (richieste utente):
   *   - Nessun fallimento di Mail propaga errori al chiamante: WiFi caduto,
   *     server Gmail unreachable, token revocato, HTTP 4xx/5xx, JSON malformato,
   *     budget scaduto, OOM su String/JSON -> ritorno semplicemente false e
   *     il loop() del .ino prosegue con i fetch calendario / refresh display.
   *   - La cache esistente NON viene corrotta da un fetch fallito: viene
   *     riscritta solo se l'intero flusso (refresh+list+batch) ha successo.
   *     Su transitorio (es. WiFi che cade tra list e batch) la cache
   *     precedente resta intatta.
   *   - Mai chiamate ricorsive, mai blocchi senza timeout: connessione e
   *     letture hanno quello di HTTPClient, l'handshake TLS_HANDSHAKE_TIMEOUT_S,
   *     e MAIL_FETCH_BUDGET_MS limita il tempo totale fra un passo e l'altro.
   *
   * Presuppone il WiFi gia' connesso e non decide quando tocca a lui: cadenza,
   * ritenti e backoff sono dello scheduler, che chiama questa funzione solo
   * quando lo slot e' aperto.
   * Ritorna true se la cache e' stata aggiornata con successo (anche con 0 mail).
   */
  inline bool runFetch()
  {
    using namespace detail;

    uint32_t t0 = millis();

    if (!refreshToken())
    {
      LOG("Mail", "refresh token KO, skip fetch");
      return false;
    }
    if ((millis() - t0) >= MAIL_FETCH_BUDGET_MS)
    {
      // Budget scaduto: applichiamo lo stesso backoff degli errori di rete
      // altrimenti il loop OTA (10ms) rifa subito il refresh ad ogni iter.
      LOG("Mail", "budget esaurito sul refresh, skip");
      return false;
    }

    // Un solo client e una sola richiesta per lista e batch, in HTTP/1.1 con
    // keep-alive: le due chiamate vanno allo stesso host e la seconda riusa
    // la negoziazione TLS della prima.
    WiFiClientSecure client;
    Net::prepareTlsClient(client);
    HTTPClient http;
    http.setReuse(true);

    String ids[MAX_MESSAGES];
    size_t n = 0;
    if (!fetchMessageIds(http, client, ids, n))
    {
      // Cache NON toccata: lo stato precedente viene preservato finche'
      // un fetch successivo non ha successo.
      return false;
    }

    if (n == 0)
    {
      // Inbox vuota: cache azzerata, NON e' un errore. Confermato dal server.
      for (size_t i = 0; i < MAX_MESSAGES; ++i) messages[i].valid = false;
      messagesCount = 0;
      LOG("Mail", "cache azzerata (inbox vuota)");
      return true;
    }

    if ((millis() - t0) >= MAIL_FETCH_BUDGET_MS)
    {
      // Budget scaduto dopo la list: stesso backoff per evitare re-list
      // continuo nel loop OTA (10ms).
      LOG("Mail", "budget esaurito prima del batch, skip");
      return false;
    }

    /**
     * Buffer temporaneo: il batch scrive qui, e SOLO se ha successo
     * sovrascriviamo la cache "live". Su transitorio (WiFi cade tra list
     * e batch, batch HTTP 5xx, parse error) la cache precedente resta
     * intatta e la futura UI continua a mostrare le ultime mail valide.
     * Costo sullo stack: MAX_MESSAGES copie di MailMessage, circa 140 byte l'una.
     */
    MailMessage tmp[MAX_MESSAGES] = {};
    size_t tmpCount = 0;
    if (!fetchMessagesBatch(http, client, ids, n, tmp, tmpCount))
    {
      // Batch fallito: cache esistente NON sovrascritta, resta valido
      // l'ultimo snapshot.
      return false;
    }

    /**
     * Sanity check: la list ha restituito N>0 ma il batch non ha prodotto
     * nessun MailMessage parsato. Significa che TUTTE le sub-response
     * sono fallite il parse (es. API change Gmail, JSON malformato).
     * Trattiamo come transitorio: preserviamo la cache esistente invece
     * di azzerarla. Senza questa guardia, una rotta protocollare lato
     * server cancellerebbe le mail valide gia' in cache.
     */
    if (tmpCount == 0)
    {
      LOG("Mail", "batch: 0 parsati su lista non vuota, cache preservata");
      return false;
    }

    /**
     * Commit atomico: copiamo il buffer temporaneo nella cache live solo
     * adesso, quando il batch ha prodotto almeno una mail parsata.
     */
    for (size_t i = 0; i < MAX_MESSAGES; ++i)
      messages[i] = (i < tmpCount) ? tmp[i] : MailMessage{};
    messagesCount = tmpCount;

    // Log riassuntivo per diagnosi (i campi rispettano i cap MAIL_*_LEN)
    for (size_t i = 0; i < messagesCount; ++i)
    {
      const MailMessage &m = messages[i];
      LOGV("Mail", "%u%s From=%s Subject=%s ts=%lld",
           (unsigned)i, m.unread ? " *" : "  ",
           m.sender, m.subject, (long long)m.receivedUtc);
    }
    return true;
  }
} // namespace Mail

#endif // MAIL_MODULE_H

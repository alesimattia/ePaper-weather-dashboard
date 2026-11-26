#ifndef NET_H
#define NET_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <mbedtls/ssl.h>
#include <ssl_client.h>
#include <string.h>

#include "Timings.h"
#include "Log.h"
#include "TlsSessionCache.h"

/**
 * Verifica del certificato sulle connessioni che portano segreti (token OAuth,
 * Bearer, token di routefinder, appid). A 0 tutte le HTTPS tornano insecure:
 * è l'uscita di emergenza se una catena non risultasse coperta dal bundle.
 */
#ifndef TLS_VERIFY
#define TLS_VERIFY 1
#endif

/**
 * Ripresa delle sessioni TLS con i ticket: una connessione ripresa salta
 * certificato e scambio di chiavi, cioè la parte CPU-bound dell'handshake che
 * a radio accesa costa più di un secondo. A 0 ogni connessione fa
 * l'handshake completo.
 */
#ifndef TLS_SESSION_RESUME
#define TLS_SESSION_RESUME 1
#endif

/** Host ricordati e byte per sessione serializzata: parametri della sessione e
 *  ticket del server, senza il certificato (vedi TlsClient::storeSession()).
 *  Gli slot coprono le coppie (host, verifica) con ticket di un giorno: cinema
 *  insecure, routefinder insecure e verificato, token e API di Google e di
 *  Microsoft. Così una sessione non viene sostituita prima di servire. La
 *  cache è memoria statica sottratta all'heap, che durante l'handshake
 *  completo verificato di OpenWeather, con il buffer dell'immagine allocato,
 *  ha pochi KB di margine. */
#ifndef TLS_SESSION_SLOTS
#define TLS_SESSION_SLOTS 7
#endif
#ifndef TLS_SESSION_BYTES
#define TLS_SESSION_BYTES 512
#endif

/**
 * Configurazione unica delle connessioni HTTPS dei produttori di rete e
 * riconoscimento delle credenziali lasciate al valore del template.
 *
 * Esiste perchè le stesse regole valgono in più punti di tre moduli:
 * certificato verificato dove passano segreti, sessioni riprese, handshake con
 * un tetto, risposte non chunked, corpo letto attendendo le pause fra i record
 * TLS (BodyStream) entro una scadenza totale. Tenerle qui fa sì che un
 * produttore aggiunto in futuro le erediti da sè.
 */
namespace Net
{
  namespace detail
  {
    /** Sessioni riprese, in memoria statica: sopravvivono al light sleep e si
     *  perdono al riavvio, quando la prima connessione fa l'handshake completo. */
    inline TlsSessionCache<TLS_SESSION_SLOTS, TLS_SESSION_BYTES> sessions;
  }

  /**
   * Client TLS dei produttori: un WiFiClientSecure che ricorda se verificare il
   * certificato e riprende la sessione dell'ultima connessione allo stesso host.
   * Il core non espone la ripresa, quindi la connessione parte in plain start,
   * che prepara il contesto mbedTLS senza handshake, e la sessione salvata si
   * applica prima di avviarlo. Un ticket che il server non accetta fa ripiegare
   * mbedTLS da sè sull'handshake completo.
   */
  class TlsClient : public WiFiClientSecure
  {
  public:
    using WiFiClientSecure::connect;

    /** Modo di verifica scelto da prepareTlsClient(), che fa anche da chiave della cache. */
    void setVerify(bool verify) { verifyPeer = verify; }
    bool verify() const { return verifyPeer; }

    /** Connessione chiamata da HTTPClient: TCP e contesto mbedTLS, sessione
     *  salvata, handshake, salvataggio della sessione nuova. */
    int connect(const char* host, uint16_t port, int32_t timeout) override
    {
#if TLS_SESSION_RESUME
      setPlainStart();
      if (!WiFiClientSecure::connect(host, port, timeout)) return 0;

      size_t savedLength = 0;
      const uint8_t* saved = detail::sessions.find(host, verifyPeer, savedLength);
      if (saved != nullptr)
      {
        mbedtls_ssl_session session;
        mbedtls_ssl_session_init(&session);
        if (mbedtls_ssl_session_load(&session, saved, savedLength) == 0)
          mbedtls_ssl_set_session(&sslclient->ssl_ctx, &session);
        mbedtls_ssl_session_free(&session);
      }

      // ssl_starttls_handshake() e non startTLS(), che non registra l'errore:
      // senza, lastError() non saprebbe dire perchè l'handshake è fallito.
      // L'heap minimo si misura durante l'handshake perchè è lì il picco: i
      // buffer mbedTLS si allocano dentro, e un handshake completo verificato
      // ci aggiunge la catena del server e la verifica RSA.
      heap_caps_monitor_local_minimum_free_size_start();
      const uint32_t startMs = millis();
      const int ret = ssl_starttls_handshake(sslclient.get());
      const uint32_t minFree = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
      heap_caps_monitor_local_minimum_free_size_stop();
      sslclient->last_error = ret;
      if (ret < 0)
      {
        LOGV("tls", "%s: handshake %s fallito, heap minimo %lu", host,
             verifyPeer ? "verificato" : "insecure", (unsigned long)minFree);
        detail::sessions.forget(host, verifyPeer);
        stop();
        return 0;
      }
      _stillinPlainStart = false;
      releasePeerChain();
      const size_t stored = storeSession(host);
      LOGV("tls", "%s: handshake %s %lu ms, %s, heap minimo %lu, sessione salvata %u byte", host,
           verifyPeer ? "verificato" : "insecure", (unsigned long)(millis() - startMs),
           saved != nullptr ? "sessione offerta" : "completo", (unsigned long)minFree, (unsigned)stored);
      return 1;
#else
      return WiFiClientSecure::connect(host, port, timeout);
#endif
    }

  private:
    /**
     * Libera la catena del server che CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
     * tiene in heap per tutta la connessione (con OpenWeather quattro
     * certificati, circa 10 KB, proprio mentre si legge il corpo JSON). A
     * handshake concluso non serve più: ssl_starttls_handshake() ha già
     * controllato l'esito, che resta nella sessione (verify_result), e il
     * client non rinegozia. Senza catena anche la sessione salvata da
     * storeSession() non contiene il certificato.
     */
    void releasePeerChain()
    {
      mbedtls_ssl_session* live = sslclient->ssl_ctx.MBEDTLS_PRIVATE(session);
      if (live == nullptr) return;
      mbedtls_x509_crt*& chain = live->MBEDTLS_PRIVATE(peer_cert);
      if (chain == nullptr) return;
      mbedtls_x509_crt_free(chain);
      mbedtls_free(chain);
      chain = nullptr;
    }

    /**
     * Serializza la sessione appena negoziata nello slot dell'host. Il
     * certificato non c'è più (releasePeerChain()): la ripresa non lo riceve nè
     * lo verifica, e l'esito della verifica originale viaggia nella sessione
     * (verify_result), quindi mbedtls_ssl_get_verify_result() dopo una ripresa
     * resta quello vero. Senza certificato una sessione sta in
     * TLS_SESSION_BYTES, una frazione di quanto servirebbe con il certificato,
     * e la differenza resta all'heap.
     *
     * Si salvano solo le sessioni con un ticket: senza, la ripresa dipende
     * dalla cache del server, che dura meno delle cadenze dei produttori
     * (OpenWeather non riprende già dopo 5 minuti), e lo slot sarebbe sprecato.
     * Una sessione più grande dello slot non si salva e la prossima connessione
     * fa l'handshake completo.
     * @return i byte salvati, 0 se la sessione non è stata salvata.
     */
    size_t storeSession(const char* host)
    {
      size_t length = 0;
      mbedtls_ssl_session session;
      mbedtls_ssl_session_init(&session);
      if (mbedtls_ssl_get_session(&sslclient->ssl_ctx, &session) == 0 &&
          session.MBEDTLS_PRIVATE(ticket_len) > 0)
      {
        uint8_t* buffer = detail::sessions.reserve(host, verifyPeer);
        if (buffer != nullptr)
        {
          if (mbedtls_ssl_session_save(&session, buffer, detail::sessions.capacity(), &length) != 0)
            length = 0;
          detail::sessions.commit(length);
        }
      }
      else
        detail::sessions.forget(host, verifyPeer);
      mbedtls_ssl_session_free(&session);
      return length;
    }

    bool verifyPeer = true;
  };

  /**
   * Prepara un client TLS: certificato verificato con il bundle CA del core,
   * lo stesso di Tuya, oppure insecure dove non passano segreti; handshake
   * limitato a TLS_HANDSHAKE_TIMEOUT_S, perchè il default del core (120 s)
   * fermerebbe tutti i produttori successivi. Il bundle non controlla le date
   * (CONFIG_MBEDTLS_HAVE_TIME_DATE spento), quindi non serve l'orologio.
   */
  inline void prepareTlsClient(TlsClient& client, bool verify)
  {
    const bool verifyPeer = verify && TLS_VERIFY;
    client.setVerify(verifyPeer);
    if (verifyPeer) client.useBuiltinCACertBundle();
    else client.setInsecure();
    client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
  }

  /**
   * Prepara client e richiesta e apre l'URL. HTTP/1.0 impedisce al server di
   * rispondere chunked, che Google e Microsoft Graph usano in HTTP/1.1 e che
   * deserializeJson() sullo stream grezzo non sa leggere.
   * Usato da Weather.h, Calendar.h e dai download del .ino.
   */
  inline bool beginHttps(HTTPClient& http, TlsClient& client, const String& url, bool verify = true)
  {
    prepareTlsClient(client, verify);
    http.useHTTP10(true);
    return http.begin(client, url);
  }

  /** Logga il motivo TLS di una richiesta fallita prima della risposta HTTP,
   *  che il core altrimenti tace con il debug spento. */
  inline void logTlsFailure(const char* what, TlsClient& client, int code)
  {
    if (code >= 0) return;
    char reason[96];
    const int error = client.lastError(reason, sizeof(reason));
    if (error != 0) LOG("tls", "%s: %s (%d)", what, reason, error);
  }

  /**
   * Corpo di una risposta HTTPS letto come Stream, a blocchi e attendendo le
   * pause fra i record TLS invece di scambiarle per la fine del corpo.
   *
   * Il client TLS del core ha il socket non bloccante: appena i dati già
   * decifrati finiscono, read() torna -1 e readBytes() si ferma con quanto ha
   * letto, anche zero byte. I server mandano il corpo a blocchi separati da
   * pause, quindi un ciclo di readBytes() vedrebbe una fine anticipata e
   * deserializeJson() un IncompleteInput. Qui una lettura vuota con la
   * connessione aperta si attende con delay(1). Due limiti la chiudono: il
   * timeout dello stream senza dati, che HTTPClient imposta alla connessione
   * (5 s di default, oppure il valore passato a http.setTimeout()), e la
   * scadenza totale HTTP_BODY_DEADLINE_MS, senza la quale un server che manda
   * un byte ogni pochi secondi terrebbe fermo il loop per sempre. Ogni blocco
   * ricevuto alimenta il watchdog del loop, perchè è progresso.
   *
   * Il buffer da 128 byte serve ad ArduinoJson, che legge un carattere alla
   * volta: senza, ogni carattere costerebbe due letture mbedTLS. Le letture
   * grandi (i blocchi da 1 KB delle immagini) vanno direttamente nella destinazione.
   * Usato da Weather.h, Calendar.h e dai download del .ino.
   */
  class BodyStream : public Stream
  {
  public:
    explicit BodyStream(Client& source) : source_(source), startMs_(millis()) {}

    int available() override { return (int)(len_ - pos_) + source_.available(); }
    int read() override { return fill() ? buf_[pos_++] : -1; }
    int peek() override { return fill() ? buf_[pos_] : -1; }
    size_t write(uint8_t) override { return 0; }

    using Stream::readBytes;

    /**
     * Legge fino a length byte e torna meno solo a connessione chiusa, su
     * errore TLS, dopo il timeout dello stream senza dati o alla scadenza totale.
     */
    size_t readBytes(char* buffer, size_t length) override
    {
      size_t done = 0;
      while (done < length)
      {
        if (pos_ < len_)
        {
          size_t n = min(len_ - pos_, length - done);
          memcpy(buffer + done, buf_ + pos_, n);
          pos_ += n;
          done += n;
        }
        else if (length - done >= sizeof(buf_))
        {
          size_t n = waitRead(buffer + done, length - done);
          if (n == 0)
            break;
          done += n;
        }
        else if (!fill())
          break;
      }
      return done;
    }

  private:
    /**
     * Legge quanto è già arrivato, attendendo se non è arrivato niente.
     * Torna 0 a connessione chiusa, su errore TLS (available() del core chiude
     * la connessione), dopo il timeout dello stream senza dati o alla scadenza totale.
     */
    size_t waitRead(char* dst, size_t n)
    {
      uint32_t t0 = millis();
      for (;;)
      {
        size_t got = source_.readBytes(dst, n);
        if (got > 0)
        {
          esp_task_wdt_reset();
          return got;
        }
        const uint32_t now = millis();
        if (!source_.connected() || now - t0 >= source_.getTimeout() ||
            now - startMs_ >= HTTP_BODY_DEADLINE_MS)
          return 0;
        delay(1);
      }
    }

    /** Ricarica il buffer se è vuoto; false se non arriva più niente. */
    bool fill()
    {
      if (pos_ < len_)
        return true;
      pos_ = 0;
      len_ = waitRead((char*)buf_, sizeof(buf_));
      return len_ > 0;
    }

    Client& source_;
    const uint32_t startMs_;
    uint8_t buf_[128];
    size_t pos_ = 0;
    size_t len_ = 0;
  };

  /**
   * Legge al più size - 1 byte del corpo e termina la stringa, per i log dei
   * corpi d'errore: http.getString() leggerebbe senza limite un corpo
   * qualunque. Usato da Calendar.h.
   */
  inline size_t readBodyPrefix(HTTPClient& http, char* buffer, size_t size)
  {
    if (buffer == nullptr || size == 0) return 0;
    BodyStream body(http.getStream());
    const size_t n = body.readBytes(buffer, size - 1);
    buffer[n] = '\0';
    return n;
  }

  /**
   * Legge esattamente total byte del corpo a blocchi da 1 KB e li passa a
   * sink(data, length), che ritorna false per interrompere. Sta in una
   * funzione a sè, non inline, così il blocco da 1 KB occupa lo stack solo
   * durante la lettura e non durante l'handshake del chiamante, che è il punto
   * più profondo del loop. Ritorna i byte consegnati al sink.
   * Usato dai download del .ino.
   */
  template <typename Sink>
  __attribute__((noinline)) uint32_t readBodyInto(HTTPClient& http, uint32_t total, Sink sink)
  {
    BodyStream body(http.getStream());
    uint8_t chunk[1024];
    uint32_t received = 0;
    while (received < total)
    {
      const uint32_t remaining = total - received;
      const size_t wanted = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
      const size_t got = body.readBytes(chunk, wanted);
      if (got != wanted || !sink(chunk, got))
        return received + (got == wanted ? 0 : (uint32_t)got);
      received += got;
    }
    return received;
  }

  /**
   * Vero se il valore di Env.h non è vuoto e non è un placeholder del template,
   * che iniziano tutti con "paste_" o con "0000000": è lo stesso criterio di
   * google_oauth_setup.py e msgraph_oauth_setup.py, applicato come prefisso.
   * Usato dalle guardie isConfigured() dei moduli e da Tuya.h.
   */
  inline bool isCredentialSet(const char* value)
  {
    return value != nullptr && value[0] != '\0' &&
           strncmp(value, "paste_", 6) != 0 &&
           strncmp(value, "0000000", 7) != 0;
  }
}

#endif // NET_H

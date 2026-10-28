#ifndef NET_H
#define NET_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <string.h>

#include "Timings.h"

/**
 * Configurazione unica delle connessioni HTTPS dei produttori di rete e
 * riconoscimento delle credenziali lasciate al valore del template.
 *
 * Esiste perchè le stesse quattro regole valgono in più punti di quattro moduli:
 * certificato non verificato, handshake con un tetto, risposte non chunked,
 * corpo letto attendendo le pause fra i record TLS (BodyStream).
 * Tenerle qui fa sì che un produttore aggiunto in futuro le erediti da sè.
 */
namespace Net
{
  /**
   * Prepara un client TLS: certificato non verificato, come in tutti i fetch
   * dei dati pubblici, e handshake limitato a TLS_HANDSHAKE_TIMEOUT_S, perchè il
   * default del core (120 s) fermerebbe tutti i produttori successivi.
   * Usato direttamente da Mail.h, che riusa la connessione in HTTP/1.1.
   */
  inline void prepareTlsClient(WiFiClientSecure& client)
  {
    client.setInsecure();
    client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
  }

  /**
   * Prepara client e richiesta e apre l'URL. HTTP/1.0 impedisce al server di
   * rispondere chunked, che Google e Microsoft Graph usano in HTTP/1.1 e che
   * deserializeJson() sullo stream grezzo non sa leggere.
   * Usato da Weather.h, Calendar.h e dal download cinema del .ino.
   */
  inline bool beginHttps(HTTPClient& http, WiFiClientSecure& client, const String& url)
  {
    prepareTlsClient(client);
    http.useHTTP10(true);
    return http.begin(client, url);
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
   * connessione aperta si attende con delay(1); il limite è il timeout dello
   * stream senza dati, che HTTPClient imposta alla connessione (5 s di default,
   * oppure il valore passato a http.setTimeout()).
   *
   * Il buffer da 128 byte serve ad ArduinoJson, che legge un carattere alla
   * volta: senza, ogni carattere costerebbe due letture mbedTLS. Le letture
   * grandi (i piani del cinema) vanno direttamente nella destinazione.
   * Usato da Weather.h, Calendar.h e dal download cinema del .ino.
   */
  class BodyStream : public Stream
  {
  public:
    explicit BodyStream(Client& source) : source_(source) {}

    int available() override { return (int)(len_ - pos_) + source_.available(); }
    int read() override { return fill() ? buf_[pos_++] : -1; }
    int peek() override { return fill() ? buf_[pos_] : -1; }
    size_t write(uint8_t) override { return 0; }

    using Stream::readBytes;

    /**
     * Legge fino a length byte e torna meno solo a connessione chiusa, su
     * errore TLS o dopo il timeout dello stream senza dati.
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
     * la connessione) o dopo il timeout dello stream senza dati.
     */
    size_t waitRead(char* dst, size_t n)
    {
      uint32_t t0 = millis();
      for (;;)
      {
        size_t got = source_.readBytes(dst, n);
        if (got > 0)
          return got;
        if (!source_.connected() || millis() - t0 >= source_.getTimeout())
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
    uint8_t buf_[128];
    size_t pos_ = 0;
    size_t len_ = 0;
  };

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

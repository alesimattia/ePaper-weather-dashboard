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
 * Esiste perchè le stesse tre regole valevano in sette punti di quattro moduli:
 * certificato non verificato, handshake con un tetto, risposte non chunked.
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

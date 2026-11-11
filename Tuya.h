#ifndef TUYA_H
#define TUYA_H

#include <Arduino.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <esp_tls.h>
#include <esp_crt_bundle.h>
#include <mbedtls/md.h>

#include "Env.h"
#include "Timings.h"
#include "Log.h"
#include "Clock.h"
#include "Indoor.h"
#include "Net.h"

/**
 * Modulo header-only di telemetria verso il cloud Tuya, protocollo TuyaLink.
 *
 * Pubblica le cinque grandezze del BME680 lette da Indoor.h su un unico topic
 * MQTT, così che il dispositivo, una volta associato con il QR code generato
 * dalla console Tuya, mostri i dati nell'app Android Smart Life.
 *
 * Flusso di una pubblicazione, sincrono e con una sessione MQTT per messaggio:
 *   1. autenticazione: username e password sono derivati dal DeviceSecret con
 *      HMAC-SHA256 su un timestamp Unix, quindi serve l'orologio sincronizzato;
 *   2. connessione TLS 1.2 al broker della propria region, porta 8883, e CONNECT;
 *   3. publish in QoS 1 su tylink/{deviceId}/thing/property/report;
 *   4. attesa del PUBACK e chiusura della connessione TLS. Con
 *      TUYA_REQUEST_ACK si attende anche la conferma applicativa del cloud,
 *      che è l'unico modo per accorgersi di un device model non allineato.
 *
 * Tuya dichiara offline il dispositivo nell'istante in cui la connessione si
 * chiude, con un DISCONNECT MQTT (motivo client_disconnect) ma anche con una
 * semplice chiusura TCP (client_closed), e l'app Smart Life nasconde i valori
 * di un dispositivo offline. Se invece la connessione resta aperta e muta, il
 * broker considera il dispositivo online per 1,5 volte il keepalive dall'ultimo
 * traffico. Per questo la connessione NON si chiude a fine publish: resta
 * aperta, senza nessun PINGREQ, con un keepalive di TUYA_KEEPALIVE_S dichiarato
 * nel CONNECT, e si rilascia con releaseSession() solo dopo che lo scheduler ha
 * spento la radio, quando nessun pacchetto può più uscire. Il broker vede
 * allora solo silenzio, e il dispositivo resta online fino al publish
 * successivo.
 *
 * Per questo il client MQTT è scritto qui e non è esp-mqtt del core: la
 * connessione deve restare aperta senza nessun task che la serva, e fermare il
 * client con esp_mqtt_client_stop() invia un DISCONNECT, senza nessuna API
 * pubblica che lo eviti. Il sottoinsieme di
 * MQTT 3.1.1 che serve è piccolo (CONNECT, SUBSCRIBE, PUBLISH in QoS 1 e la
 * lettura di CONNACK, SUBACK, PUBACK e PUBLISH in ingresso), il trasporto è
 * esp-tls del core e l'HMAC viene da mbedtls: non serve nessuna libreria
 * aggiuntiva. Il certificato del broker è verificato con il certificate bundle
 * di mbedtls compilato nel core, quindi non c'è nessun PEM da incorporare e da
 * correggere quando Tuya ruota la CA. A differenza degli altri moduli, che
 * usano setInsecure(), qui la verifica va fatta: su questa connessione transita
 * l'HMAC del DeviceSecret.
 *
 * API, allineata a quella degli altri moduli di rete:
 *   - Tuya::begin()           : compone topic e client id, azzera lo stato;
 *   - Tuya::readyToPublish()  : precondizione non temporale (funzione pura);
 *   - Tuya::runPublish()      : pubblica, presuppone la STA connessa;
 *   - Tuya::releaseSession()  : rilascia la connessione, a radio già spenta;
 *   - Tuya::hasFailed()       : esito dell'ultimo tentativo, per il pannello.
 *
 * Il modulo non decide QUANDO tocca a lui: cadenza, ritenti e fascia oraria
 * sono dello scheduler, che chiama runPublish() solo con lo slot aperto e la
 * radio su. readyToPublish() è l'hook `preconditionsMet()` della tabella dei task.
 *
 * Invariante del light sleep: il modulo non ha task propri, quindi una
 * connessione rimasta aperta non fa nulla da sola, ma non deve arrivare al
 * light sleep. runPublish() la chiude su ogni fallimento e la lascia aperta
 * solo dopo un PUBACK; releaseSession() la rilascia, ed è chiamata da
 * wifiOff() del .ino subito dopo lo spegnimento della radio, quindi prima di
 * ogni sleep. Il costo è l'heap di una sessione TLS (~40 KB) tenuta durante i
 * task che seguono Tuya nella tabella.
 *
 * Il modulo non disegna niente e non comanda il refresh del pannello: il badge
 * di guasto lo legge Weather al momento del disegno, quindi compare nel frame
 * del ciclo, che lo scheduler disegna comunque dopo il sensore e i fetch.
 */

/**
 * Broker MQTT della region in cui è stato creato il prodotto sulla console
 * Tuya, nella forma mqtts://host:porta. Deve coincidere con la region
 * dell'account Smart Life: un broker sbagliato risponde con un errore di
 * autenticazione, indistinguibile da credenziali errate.
 *
 * Alternative: mqtts://m1.tuyaus.com:8883      (US west)
 *              mqtts://m1-ueaz.tuyaus.com:8883 (US east)
 *              mqtts://m1-weaz.tuyaeu.com:8883 (Europa occidentale)
 *              mqtts://m1.tuyacn.com:8883      (Cina)
 *              mqtts://m1.tuyain.com:8883      (India)
 */
#ifndef TUYA_MQTT_URI
#define TUYA_MQTT_URI "mqtts://m1.tuyaeu.com:8883"
#endif

/**
 * Identifier delle properties nel device model della console, uno per campo di
 * Indoor::Sample. Nessun moltiplicatore: il tipo di ogni property rispecchia
 * quello del campo, quindi il valore si trasmette così com'è.
 *
 * La corrispondenza da mantenere è fra tipo in console e tipo in Sample: la
 * temperatura è Float perchè il decimale è significativo, le altre quattro
 * sono Value con scale 0 perchè sono già intere nel firmware. Se una property
 * in console avesse una scale diversa da zero il valore apparirebbe scalato
 * nell'app senza che niente segnali l'errore, perchè il PUBACK conferma la
 * consegna MQTT e non l'accettazione del valore.
 */
#ifndef TUYA_DP_TEMPERATURE
#define TUYA_DP_TEMPERATURE "temp_value"
#endif

#ifndef TUYA_DP_HUMIDITY
#define TUYA_DP_HUMIDITY "hum_value"
#endif

#ifndef TUYA_DP_PRESSURE
#define TUYA_DP_PRESSURE "press_value"
#endif

#ifndef TUYA_DP_IAQ
#define TUYA_DP_IAQ "iaq_value"
#endif

/** Accuratezza del calibratore BSEC, 0..3: dice se l'IAQ è attendibile. */
#ifndef TUYA_DP_IAQ_ACCURACY
#define TUYA_DP_IAQ_ACCURACY "iaq_acc"
#endif

/**
 * Interruttore per la conferma applicativa del cloud, da alzare durante il
 * bring-up e da riabbassare a regime.
 *
 * Il PUBACK di MQTT dice che il broker ha ricevuto il messaggio, non che il
 * cloud ne abbia accettato il contenuto: con un identifier fuori dal device
 * model, un valore fuori dai limiti dichiarati o una scale non combaciante il
 * publish risulta comunque riuscito e i valori non compaiono nell'app. È
 * l'unico fallimento silenzioso che resta in questo modulo.
 *
 * A 1 il messaggio porta il campo sys.ack e il modulo si sottoscrive al topic
 * di risposta, dove il cloud restituisce un codice: 0 significa accettato,
 * qualunque altro valore viene loggato insieme al corpo della risposta, che
 * contiene la spiegazione. Costa una sottoscrizione, il traffico della
 * risposta e fino a TUYA_ACK_TIMEOUT_MS di attesa in più per publish, e per
 * questo il default è 0.
 *
 * Forma del campo e del topic prese dall'SDK ufficiale tuya-iot-core-sdk
 * (tuyalink_message_send), che le compone esattamente così.
 */
#ifndef TUYA_REQUEST_ACK
#define TUYA_REQUEST_ACK 0
#endif

/**
 * Coda del payload: il campo sys va a livello di messaggio, dopo data, come
 * nell'SDK ufficiale. Concatenato al format a compile time, così la
 * composizione resta un unico snprintf senza rami condizionali.
 */
#if TUYA_REQUEST_ACK
#define TUYA_SYS_ACK_FIELD ",\"sys\":{\"ack\":1}"
#else
#define TUYA_SYS_ACK_FIELD ""
#endif

namespace Tuya
{
  // -------------------------------------------------------------------------
  // Stato e helper interni, raggruppati in `detail` come negli altri moduli.
  // Gli `inline` permettono la definizione nell'header senza violare la ODR.
  // -------------------------------------------------------------------------
  namespace detail
  {
    /** Composti una volta in begin(): dipendono solo dal DeviceID e dall'URI. */
    inline char clientId[48] = {0};
    inline char topic[96]    = {0};
    inline char brokerHost[64] = {0};
    inline int  brokerPort     = 0;
#if TUYA_REQUEST_ACK
    inline char ackTopic[112] = {0};
#endif

    /** Ricomposti a ogni publish: dipendono dal timestamp corrente. */
    inline char username[192] = {0};
    inline char signSrc[192]  = {0};
    inline char password[65]  = {0};
    inline char payload[384]  = {0};

    inline esp_tls_t* tls = nullptr;
    inline bool     enabled = false;
    inline uint32_t msgSeq  = 0;

    /**
     * Esito dell'ultimo tentativo speso, letto dal rendering per segnalare il
     * guasto sul pannello. Parte da false perchè al boot, prima di qualunque
     * tentativo, non c'è niente da segnalare, e resta false quando il modulo
     * è disattivato: un dispositivo senza credenziali Tuya non è guasto.
     */
    inline bool failedLast = false;

    /**
     * Stato della sessione corrente, scritto dalla lettura dei pacchetti e
     * letto da runPublish(): tutto avviene nel task chiamante, quindi bastano
     * dei bool semplici.
     */
    inline bool connected = false;
    inline bool published = false;
    inline bool failed    = false;
#if TUYA_REQUEST_ACK
    inline bool subscribed  = false;
    inline bool ackReceived = false;
#endif

    /** Identificatore del pacchetto in corso e quello del PUBLISH da confermare. */
    inline uint16_t packetId  = 0;
    inline uint16_t publishId = 0;

    /** Tipi di pacchetto MQTT 3.1.1 che il modulo legge, dai quattro bit alti del primo byte. */
    inline constexpr uint8_t PACKET_CONNACK = 2;
    inline constexpr uint8_t PACKET_PUBLISH = 3;
    inline constexpr uint8_t PACKET_PUBACK  = 4;
    inline constexpr uint8_t PACKET_SUBACK  = 9;

    /**
     * Buffer di trasmissione: i primi TX_HEADER_SPACE byte sono riservati
     * all'intestazione, che si scrive a ritroso una volta nota la lunghezza
     * del corpo, così un pacchetto parte con una sola scrittura. 512 byte
     * contengono il PUBLISH più lungo (topic 96, payload 384).
     */
    inline constexpr size_t TX_HEADER_SPACE = 5;
    inline uint8_t txBuffer[512];
    inline size_t  txUsed     = TX_HEADER_SPACE;
    inline bool    txOverflow = false;

    /** Corpo dell'ultimo pacchetto letto; un pacchetto più grande si scarta. */
    inline uint8_t rxBody[384];

    /**
     * Tiene un eventuale NaN fuori dal payload: BSEC può produrlo e "nan" non
     * è un numero JSON, quindi il messaggio verrebbe scartato per intero.
     */
    inline float toFloatDp(float value)
    {
      return isnan(value) ? 0.0f : value;
    }

    /**
     * Separa host e porta dall'URI del broker, nella forma mqtts://host:porta.
     * @return false se l'URI non ha schema, host o porta validi.
     */
    inline bool parseBrokerUri(const char* uri)
    {
      static const char scheme[] = "mqtts://";
      if (strncmp(uri, scheme, sizeof(scheme) - 1) != 0) return false;

      const char* host  = uri + sizeof(scheme) - 1;
      const char* colon = strchr(host, ':');
      if (colon == nullptr) return false;

      const size_t hostLength = (size_t)(colon - host);
      if (hostLength == 0 || hostLength >= sizeof(brokerHost)) return false;

      memcpy(brokerHost, host, hostLength);
      brokerHost[hostLength] = 0;
      brokerPort = atoi(colon + 1);
      return brokerPort > 0 && brokerPort < 65536;
    }

    /**
     * HMAC-SHA256 one-shot reso in 64 caratteri esadecimali minuscoli, che è
     * la forma che il broker Tuya si aspetta come password.
     * @param out buffer di almeno 65 byte.
     */
    inline bool hmacSha256Hex(const char* key, const char* content, char* out, size_t outSize)
    {
      if (outSize < 65) return false;

      const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
      if (info == nullptr) return false;

      uint8_t digest[32];
      if (mbedtls_md_hmac(info,
                          (const uint8_t*)key,     strlen(key),
                          (const uint8_t*)content, strlen(content),
                          digest) != 0)
        return false;

      static const char hexDigits[] = "0123456789abcdef";
      for (size_t i = 0; i < sizeof(digest); ++i)
      {
        out[i * 2]     = hexDigits[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hexDigits[digest[i] & 0x0F];
      }
      out[64] = 0;
      return true;
    }

    /**
     * Compone username, stringa firmata e password per la sessione corrente.
     * Il timestamp arriva dal chiamante e non viene riletto qui: lo stesso
     * valore deve comparire in username e nella stringa firmata, e due
     * time(nullptr) separati sbaglierebbero a cavallo del secondo, con un
     * errore intermittente e difficile da diagnosticare.
     */
    inline bool buildCredentials(unsigned long ts)
    {
      int n = snprintf(username, sizeof(username),
                       "%s|signMethod=hmacSha256,timestamp=%lu,secureMode=1,accessType=1",
                       TUYA_DEVICE_ID, ts);
      if (n <= 0 || (size_t)n >= sizeof(username)) return false;

      // I campi e il loro ordine sono parte della firma: non riordinabili.
      n = snprintf(signSrc, sizeof(signSrc),
                   "deviceId=%s,timestamp=%lu,secureMode=1,accessType=1",
                   TUYA_DEVICE_ID, ts);
      if (n <= 0 || (size_t)n >= sizeof(signSrc)) return false;

      return hmacSha256Hex(TUYA_DEVICE_SECRET, signSrc, password, sizeof(password));
    }

    /**
     * Compone il payload del property report.
     *
     * Composto con snprintf e non con ArduinoJson malgrado la libreria sia già
     * una dipendenza: lo schema è fisso e noto a compile time, non si parsa
     * niente, e un JsonDocument allocherebbe sull'heap proprio nel momento di
     * massima pressione, con la sessione TLS aperta.
     *
     * Il time del messaggio è l'epoch del campione e non l'istante del
     * publish: il task del sensore gira a valle di quelli di rete, quindi la
     * cache contiene il campione del giro precedente e datarlo "adesso"
     * sarebbe sbagliato. Il time per property viene omesso: la documentazione
     * Tuya lo marca obbligatorio, ma il cloud accetta il messaggio senza
     * (verificato con il report_response, code 0), e quello di messaggio vale
     * per tutte le proprietà. Vale sia in secondi a 10 cifre sia in millisecondi
     * a 13; l'SDK ufficiale usa i secondi perchè il suo system_timestamp()
     * ritorna un uint32_t, che non basterebbe a contenere un epoch in
     * millisecondi.
     *
     * Con TUYA_REQUEST_ACK la coda porta anche il campo sys.ack, che chiede al
     * cloud di rispondere sul topic di report_response.
     */
    inline bool buildPayload(const Indoor::Sample& s, unsigned long ts)
    {
      const unsigned long ageSec   = (millis() - s.lastUpdateMs) / 1000UL;
      const unsigned long sampleTs = (ageSec < ts) ? (ts - ageSec) : ts;

      /**
       * La temperatura esce con un decimale perchè la sua property è Float:
       * %.1f lo emette anche sui valori tondi, e un float senza parte
       * decimale è la forma che le piattaforme TSL rifiutano. Le altre quattro
       * sono già intere in Sample e vanno come interi.
       */
      int n = snprintf(payload, sizeof(payload),
                       "{\"msgId\":\"%lu\",\"time\":%lu,\"data\":{"
                       "\"%s\":{\"value\":%.1f},"
                       "\"%s\":{\"value\":%u},"
                       "\"%s\":{\"value\":%u},"
                       "\"%s\":{\"value\":%u},"
                       "\"%s\":{\"value\":%u}}" TUYA_SYS_ACK_FIELD "}",
                       (unsigned long)++msgSeq, sampleTs,
                       TUYA_DP_TEMPERATURE,  toFloatDp(s.temperature),
                       TUYA_DP_HUMIDITY,     (unsigned)s.humidity,
                       TUYA_DP_PRESSURE,     (unsigned)s.pressure,
                       TUYA_DP_IAQ,          (unsigned)s.iaq,
                       TUYA_DP_IAQ_ACCURACY, (unsigned)s.iaqAccuracy);

      return n > 0 && (size_t)n < sizeof(payload);
    }

    // -----------------------------------------------------------------------
    // Trasporto: lettura e scrittura sulla connessione esp-tls.
    // -----------------------------------------------------------------------

    /** Esito di una lettura: dati arrivati, nessun dato entro il timeout, connessione persa. */
    enum class ReadResult : uint8_t { OK, TIMEOUT, CLOSED };

    /**
     * Attende che la connessione abbia dati da leggere, per al più timeoutMs.
     * I byte già decifrati e rimasti nel buffer di mbedtls non si vedono dal
     * socket, quindi si controllano prima con esp_tls_get_bytes_avail().
     * @return 1 se c'è qualcosa da leggere, 0 allo scadere del tempo, -1 se il
     *         socket non esiste più o select() fallisce.
     */
    inline int waitReadable(uint32_t timeoutMs)
    {
      if (esp_tls_get_bytes_avail(tls) > 0) return 1;

      int fd = -1;
      if (esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK || fd < 0) return -1;

      fd_set readSet;
      FD_ZERO(&readSet);
      FD_SET(fd, &readSet);

      struct timeval timeout;
      timeout.tv_sec  = (time_t)(timeoutMs / 1000UL);
      timeout.tv_usec = (suseconds_t)((timeoutMs % 1000UL) * 1000UL);

      const int ready = select(fd + 1, &readSet, nullptr, nullptr, &timeout);
      return ready < 0 ? -1 : (ready > 0 ? 1 : 0);
    }

    /**
     * Legge esattamente `count` byte entro timeoutMs. Una connessione chiusa o
     * in errore alza `failed`; un timeout no, perchè a pacchetto non iniziato
     * è solo silenzio del broker.
     */
    inline ReadResult readBytes(uint8_t* out, size_t count, uint32_t timeoutMs)
    {
      const uint32_t startMs = millis();
      size_t received = 0;

      while (received < count)
      {
        const uint32_t spentMs     = millis() - startMs;
        const uint32_t remainingMs = spentMs >= timeoutMs ? 0 : timeoutMs - spentMs;

        const int ready = waitReadable(remainingMs);
        if (ready == 0) return ReadResult::TIMEOUT;
        if (ready < 0)
        {
          failed = true;
          return ReadResult::CLOSED;
        }

        const ssize_t n = esp_tls_conn_read(tls, (char*)out + received, count - received);
        if (n > 0)
          received += (size_t)n;
        else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE)
          continue;
        else
        {
          failed = true;
          return ReadResult::CLOSED;
        }
      }
      return ReadResult::OK;
    }

    /**
     * Scrive tutto il buffer sulla connessione. Un errore alza `failed`;
     * WANT_READ e WANT_WRITE si riprovano finchè dura TUYA_PUBACK_TIMEOUT_MS.
     */
    inline bool writeAll(const uint8_t* data, size_t length)
    {
      const uint32_t startMs = millis();
      size_t sent = 0;

      while (sent < length)
      {
        const ssize_t n = esp_tls_conn_write(tls, (const char*)data + sent, length - sent);
        if (n > 0)
        {
          sent += (size_t)n;
          continue;
        }
        if ((n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) &&
            (millis() - startMs) < TUYA_PUBACK_TIMEOUT_MS)
        {
          delay(1);
          continue;
        }
        failed = true;
        return false;
      }
      return true;
    }

    // -----------------------------------------------------------------------
    // Composizione dei pacchetti MQTT 3.1.1.
    // -----------------------------------------------------------------------

    /** Azzera il corpo del pacchetto in composizione. */
    inline void beginBody()
    {
      txUsed     = TX_HEADER_SPACE;
      txOverflow = false;
    }

    inline void putByte(uint8_t value)
    {
      if (txUsed < sizeof(txBuffer))
        txBuffer[txUsed++] = value;
      else
        txOverflow = true;
    }

    inline void putU16(uint16_t value)
    {
      putByte((uint8_t)(value >> 8));
      putByte((uint8_t)(value & 0xFF));
    }

    inline void putBytes(const void* data, size_t length)
    {
      if (txUsed + length > sizeof(txBuffer))
      {
        txOverflow = true;
        return;
      }
      memcpy(txBuffer + txUsed, data, length);
      txUsed += length;
    }

    /** Stringa MQTT: lunghezza a due byte seguita dai caratteri, senza terminatore. */
    inline void putString(const char* text)
    {
      const size_t length = strlen(text);
      putU16((uint16_t)length);
      putBytes(text, length);
    }

    /**
     * Completa il pacchetto con la sua intestazione (primo byte e lunghezza
     * rimanente a lunghezza variabile) e lo invia con una sola scrittura.
     */
    inline bool sendPacket(uint8_t firstByte)
    {
      if (txOverflow) return false;

      uint8_t lengthBytes[4];
      size_t  lengthCount = 0;
      size_t  remaining   = txUsed - TX_HEADER_SPACE;
      const size_t bodyLength = remaining;
      do
      {
        uint8_t digit = (uint8_t)(remaining % 128);
        remaining /= 128;
        if (remaining > 0) digit |= 0x80;
        lengthBytes[lengthCount++] = digit;
      } while (remaining > 0 && lengthCount < sizeof(lengthBytes));

      const size_t start = TX_HEADER_SPACE - 1 - lengthCount;
      txBuffer[start] = firstByte;
      memcpy(txBuffer + start + 1, lengthBytes, lengthCount);

      return writeAll(txBuffer + start, 1 + lengthCount + bodyLength);
    }

    /**
     * CONNECT con sessione pulita, utente e password. Il keepalive è quello che
     * regola per quanto il broker considera online il dispositivo dopo l'ultimo
     * traffico: vedi TUYA_KEEPALIVE_S.
     */
    inline bool sendConnect()
    {
      beginBody();
      putString("MQTT");
      putByte(4);       // livello del protocollo: MQTT 3.1.1, l'unico che Tuya accetta
      putByte(0xC2);    // flag: username, password, sessione pulita
      putU16((uint16_t)TUYA_KEEPALIVE_S);
      putString(clientId);
      putString(username);
      putString(password);
      return sendPacket(0x10);
    }

    /** PUBLISH in QoS 1 del property report, senza retain. */
    inline bool sendPublish()
    {
      beginBody();
      putString(topic);
      publishId = ++packetId;
      putU16(publishId);
      putBytes(payload, strlen(payload));
      return sendPacket(0x32);
    }

#if TUYA_REQUEST_ACK
    /** SUBSCRIBE al topic di risposta del cloud, in QoS 0. */
    inline bool sendSubscribe()
    {
      beginBody();
      putU16(++packetId);
      putString(ackTopic);
      putByte(0);
      return sendPacket(0x82);
    }

    /**
     * Risposta del cloud al property report: msgId, time e un campo code, dove
     * 0 significa accettato. In caso di rifiuto il corpo porta anche il
     * motivo, quindi viene loggato per intero: è l'informazione che manca
     * quando i valori non compaiono nell'app malgrado il PUBACK.
     *
     * La sottoscrizione è in QoS 0, quindi dopo il topic non c'è un packet id
     * e il resto del pacchetto è il corpo JSON. Il corpo non è terminato da
     * zero: va copiato.
     */
    inline void handleCloudResponse(uint8_t firstByte, const uint8_t* body, size_t length)
    {
      if (length < 2) return;

      size_t offset = 2 + (size_t)((body[0] << 8) | body[1]);
      if (((firstByte >> 1) & 3) > 0) offset += 2;
      if (offset > length) return;

      char text[160];
      size_t n = length - offset;
      if (n > sizeof(text) - 1) n = sizeof(text) - 1;
      memcpy(text, body + offset, n);
      text[n] = 0;

      const char* p = strstr(text, "\"code\":");
      const long code = (p != nullptr) ? strtol(p + 7, nullptr, 10) : -1;

      if (code == 0)
        LOG("Tuya", "il cloud ha accettato le proprietà (code 0)");
      else
        LOG("Tuya", "il cloud ha RIFIUTATO le proprietà: %s", text);

      ackReceived = true;
    }
#endif

    /**
     * Applica allo stato un pacchetto letto: CONNACK, PUBACK del publish in
     * corso e, con TUYA_REQUEST_ACK, SUBACK e risposta del cloud. Gli altri
     * tipi non servono a una sessione di un solo messaggio e si ignorano.
     */
    inline void handlePacket(uint8_t firstByte, const uint8_t* body, size_t length)
    {
      switch (firstByte >> 4)
      {
        case PACKET_CONNACK:
          if (length < 2)
          {
            failed = true;
            break;
          }
          if (body[1] == 0)
            connected = true;
          else
          {
            // 0x04 e 0x05 puntano a credenziali, firma o timestamp.
            LOG("Tuya", "connessione rifiutata, return code 0x%02x", (unsigned)body[1]);
            failed = true;
          }
          break;

        case PACKET_PUBACK:
          // In QoS 1 il PUBACK del broker conferma la consegna del publish.
          if (length >= 2 && (uint16_t)((body[0] << 8) | body[1]) == publishId) published = true;
          break;

#if TUYA_REQUEST_ACK
        case PACKET_SUBACK:
          subscribed = true;
          break;

        case PACKET_PUBLISH:
          handleCloudResponse(firstByte, body, length);
          break;
#endif

        default:
          break;
      }
    }

    /**
     * Legge un pacchetto MQTT e lo applica allo stato. Il primo byte attende al
     * più timeoutMs; una volta iniziato, il resto deve arrivare entro
     * TUYA_PUBACK_TIMEOUT_MS, altrimenti il flusso non è più allineato ai
     * pacchetti e la sessione si considera persa.
     */
    inline ReadResult pollPacket(uint32_t timeoutMs)
    {
      uint8_t firstByte = 0;
      const ReadResult first = readBytes(&firstByte, 1, timeoutMs);
      if (first != ReadResult::OK) return first;

      size_t   length     = 0;
      uint32_t multiplier = 1;
      for (uint8_t i = 0;; ++i)
      {
        uint8_t digit = 0;
        if (readBytes(&digit, 1, TUYA_PUBACK_TIMEOUT_MS) != ReadResult::OK || i == 4)
        {
          failed = true;
          return ReadResult::CLOSED;
        }
        length += (size_t)(digit & 0x7F) * multiplier;
        if ((digit & 0x80) == 0) break;
        multiplier *= 128;
      }

      // Un pacchetto più grande del buffer non serve a questa sessione: si
      // legge e si scarta, per restare allineati al successivo.
      const bool keep = length <= sizeof(rxBody);
      size_t left = length;
      while (left > 0)
      {
        const size_t chunk = left < sizeof(rxBody) ? left : sizeof(rxBody);
        if (readBytes(rxBody, chunk, TUYA_PUBACK_TIMEOUT_MS) != ReadResult::OK)
        {
          failed = true;
          return ReadResult::CLOSED;
        }
        left -= chunk;
      }

      if (keep)
        handlePacket(firstByte, rxBody, length);
      else
        LOG("Tuya", "pacchetto di %u byte scartato: più grande del buffer", (unsigned)length);
      return ReadResult::OK;
    }

    /**
     * Attende che un flag di sessione venga alzato leggendo i pacchetti in
     * arrivo, uscendo anche su failed per non consumare il timeout intero
     * quando il broker ha già rifiutato la connessione o l'ha chiusa.
     */
    inline bool waitFlag(const bool& flag, uint32_t timeoutMs)
    {
      const uint32_t startMs = millis();
      while (!flag && !failed)
      {
        const uint32_t spentMs = millis() - startMs;
        if (spentMs >= timeoutMs) break;
        pollPacket(timeoutMs - spentMs);
      }
      return flag;
    }

    /**
     * Chiude la connessione TLS e libera l'handle, senza inviare nessun
     * pacchetto MQTT. Con la radio accesa la chiusura manda un FIN al broker,
     * che segna subito offline il dispositivo: si usa quindi sui fallimenti di
     * runPublish(), dove il dispositivo non ha pubblicato comunque, e da
     * releaseSession() a radio già spenta, dove il FIN non può uscire.
     */
    inline void closeSession()
    {
      if (tls != nullptr)
      {
        esp_tls_conn_destroy(tls);
        tls = nullptr;
      }
      connected = false;
    }

    /**
     * Logga il motivo di una connessione TLS fallita. È la diagnostica che
     * distingue i modi di fallimento del bring-up, che altrimenti si
     * presentano tutti come "non si connette": un errore di certificato punta
     * al bundle CA o alla region, un timeout a DNS o rete.
     */
    inline void logConnectFailure()
    {
      esp_tls_error_handle_t errorHandle = nullptr;
      if (esp_tls_get_error_handle(tls, &errorHandle) == ESP_OK && errorHandle != nullptr)
        LOG("Tuya", "connessione TLS fallita: esp_err=0x%x tls=0x%x flags=0x%x",
            (unsigned)errorHandle->last_error, (unsigned)errorHandle->esp_tls_error_code,
            (unsigned)errorHandle->esp_tls_flags);
      else
        LOG("Tuya", "connessione TLS fallita, heap libero %u", (unsigned)ESP.getFreeHeap());
    }
  } // namespace detail

  /**
   * Azzera lo stato e compone le stringhe che dipendono solo dal DeviceID.
   * Non tocca la rete: va chiamata in setup(), dove la STA non è ancora su.
   *
   * Disattiva il modulo quando Env.h non contiene credenziali vere. Il caso è
   * concreto e non teorico: build.ps1 rigenera Env.h dal template quando
   * manca, e senza questo controllo il firmware spenderebbe un handshake TLS a
   * ogni slot su un dispositivo non configurato.
   */
  inline void begin()
  {
    using namespace detail;

    tls        = nullptr;
    connected  = false;
    published  = false;
    failed     = false;
    failedLast = false;
    msgSeq     = 0;

    enabled = Net::isCredentialSet(TUYA_DEVICE_ID) && Net::isCredentialSet(TUYA_DEVICE_SECRET);

    if (!enabled)
    {
      LOG("Tuya", "credenziali assenti in Env.h: telemetria disattivata");
      return;
    }

    if (!parseBrokerUri(TUYA_MQTT_URI))
    {
      enabled = false;
      LOG("Tuya", "TUYA_MQTT_URI non valido (atteso mqtts://host:porta): telemetria disattivata");
      return;
    }

    snprintf(clientId, sizeof(clientId), "tuyalink_%s", TUYA_DEVICE_ID);
    snprintf(topic, sizeof(topic), "tylink/%s/thing/property/report", TUYA_DEVICE_ID);
#if TUYA_REQUEST_ACK
    subscribed  = false;
    ackReceived = false;
    snprintf(ackTopic, sizeof(ackTopic),
             "tylink/%s/thing/property/report_response", TUYA_DEVICE_ID);
#endif

    LOG("Tuya", "init ok, publish ogni %u min verso %s",
        (unsigned)Timings::get().tuyaMin, TUYA_MQTT_URI);
#if TUYA_REQUEST_ACK
    LOG("Tuya", "conferma applicativa del cloud attiva (bring-up)");
#endif
  }

  /**
   * Precondizione non temporale del publish: credenziali presenti, un campione
   * del sensore da mandare e l'orologio sincronizzato. Funzione pura, non
   * tocca nè lo stato nè la radio.
   *
   * È l'hook `preconditionsMet()` del task nella tabella del .ino, quindi un falso
   * esclude il publish anche dal calcolo del prossimo risveglio, e soprattutto
   * non consuma lo slot: il task ha un solo tentativo per cadenza, quindi ogni
   * slot speso senza poter pubblicare sarebbe perso fino alla cadenza piena.
   *
   * I due controlli non stanno in runPublish() di proposito, perchè entrambe
   * le condizioni si avverano da sè poco dopo il boot: BSEC in ULP produce il
   * primo output dopo qualche minuto, e l'orologio si sincronizza alla prima
   * wifiOn(), che nella finestra di manutenzione non viene chiamata. Senza
   * questa guardia il primo giro segnalerebbe un guasto Tuya che è solo un
   * orologio non ancora pronto, e lo pagherebbe con due refresh pieni da 24 s.
   */
  inline bool readyToPublish()
  {
    return detail::enabled && Indoor::sample().valid && Clock::valido();
  }

  /**
   * Pubblica il campione corrente e attende la conferma del broker.
   *
   * Presuppone la STA connessa e non decide quando tocca a lui: cadenza,
   * ritenti e fascia oraria sono dello scheduler, che chiama questa funzione
   * solo con lo slot aperto e la radio su. Presuppone anche l'orologio
   * sincronizzato, con la guardia qui sotto per il caso in cui non lo sia.
   *
   * Dopo un PUBACK la connessione resta aperta e muta, per i motivi spiegati
   * in testa al file, e la rilascia releaseSession() a radio spenta; su ogni
   * altro esito è già chiusa al ritorno. I passi della sequenza sono
   * concatenati in un solo if, così che la chiusura sia raggiunta da ogni
   * percorso di fallimento. Una connessione rimasta aperta da una chiamata
   * precedente, perchè lo scheduler non ha spento la radio come nella finestra
   * di manutenzione, si chiude prima di aprirne una nuova.
   *
   * Lo stato del badge è pessimista: failedLast viene alzato prima di
   * qualunque operazione fallibile e solo il PUBACK lo azzera, così ogni
   * uscita anticipata lascia il guasto segnalato anche se in futuro si
   * aggiunge un return in mezzo alla sequenza.
   *
   * @return true solo se il PUBACK è arrivato, cioè se il dato è stato
   *         consegnato. Attenzione: conferma la consegna MQTT e non
   *         l'accettazione dei valori, che dipende dal device model.
   */
  inline bool runPublish()
  {
    using namespace detail;

    /**
     * Guardia difensiva: readyToPublish() copre già queste condizioni, ma
     * pubblicare senza campione manderebbe cinque zeri al cloud, che nell'app
     * sono indistinguibili da una misura vera.
     */
    const Indoor::Sample& s = Indoor::sample();
    if (!enabled || !s.valid) return false;

    failedLast = true;

    if (!Clock::valido())
    {
      LOG("Tuya", "orologio non sincronizzato, publish saltato");
      return false;
    }

    const unsigned long ts = (unsigned long)time(nullptr);

    if (!buildCredentials(ts))
    {
      LOG("Tuya", "costruzione delle credenziali fallita");
      return false;
    }
    if (!buildPayload(s, ts))
    {
      LOG("Tuya", "payload troppo lungo per il buffer");
      return false;
    }

    closeSession();

    published = false;
    failed    = false;
    packetId  = 0;
    publishId = 0;
#if TUYA_REQUEST_ACK
    subscribed  = false;
    ackReceived = false;
#endif

    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms        = (int)TUYA_CONNECT_TIMEOUT_MS;

    tls = esp_tls_init();
    if (tls == nullptr)
    {
      LOG("Tuya", "init del client fallita, heap libero %u", (unsigned)ESP.getFreeHeap());
      return false;
    }

    bool ok = false;
    if (esp_tls_conn_new_sync(brokerHost, (int)strlen(brokerHost), brokerPort, &cfg, tls) != 1)
    {
      logConnectFailure();
    }
    else if (sendConnect() && waitFlag(connected, TUYA_CONNECT_TIMEOUT_MS))
    {
#if TUYA_REQUEST_ACK
      /**
       * Sottoscrizione al topic di risposta prima del publish, con attesa del
       * SUBACK: pubblicare con la sottoscrizione non ancora attiva farebbe
       * perdere la risposta. Un fallimento qui non blocca la telemetria, si
       * perde solo la conferma.
       */
      if (sendSubscribe())
        waitFlag(subscribed, TUYA_ACK_TIMEOUT_MS);
      else
        LOG("Tuya", "sottoscrizione al topic di risposta fallita");
#endif

      if (failed)
        LOG("Tuya", "connessione persa prima del publish");
      else if (!sendPublish())
        LOG("Tuya", "publish non inviato");
      else
        ok = waitFlag(published, TUYA_PUBACK_TIMEOUT_MS);

#if TUYA_REQUEST_ACK
      /**
       * L'esito di runPublish() resta quello del PUBACK: la conferma del
       * cloud è diagnostica e il suo codice viene loggato alla lettura della
       * risposta, quindi un rifiuto applicativo non va confuso con una
       * mancata consegna. Attesa solo se il messaggio è stato consegnato:
       * senza PUBACK non c'è nulla che il cloud possa confermare.
       */
      if (ok && !waitFlag(ackReceived, TUYA_ACK_TIMEOUT_MS))
        LOG("Tuya", "nessuna conferma dal cloud entro il timeout");
#endif
    }

    // Dopo un PUBACK la connessione resta aperta: chiuderla farebbe segnare
    // offline il dispositivo. La rilascia releaseSession() a radio spenta.
    if (!ok) closeSession();

    if (ok)
    {
      // Unico punto che azzera il guasto: solo un PUBACK dimostra che il
      // dato è arrivato.
      failedLast = false;
      LOG("Tuya", "publish ok: T=%.1f RH=%u P=%u IAQ=%u acc=%u",
          s.temperature, (unsigned)s.humidity, (unsigned)s.pressure,
          (unsigned)s.iaq, (unsigned)s.iaqAccuracy);
      LOG("Tuya", "connessione tenuta aperta fino allo spegnimento della radio, heap libero %u",
          (unsigned)ESP.getFreeHeap());
    }
    else
      LOG("Tuya", "publish non confermato");

    return ok;
  }

  /**
   * Rilascia la connessione lasciata aperta da runPublish(). Va chiamata solo a
   * radio già spenta: con la radio accesa la chiusura manderebbe un FIN al
   * broker, che segnerebbe subito offline il dispositivo. La chiama wifiOff()
   * del .ino subito dopo lo spegnimento; senza una connessione aperta non fa
   * niente.
   */
  inline void releaseSession()
  {
    detail::closeSession();
  }

  /**
   * Vero se l'ultimo tentativo speso è fallito, per qualunque ragione:
   * orologio non sincronizzato, broker irraggiungibile, credenziali rifiutate
   * o PUBACK mancante.
   *
   * Serve al rendering per segnalare il guasto sul pannello. Torna false al
   * primo publish riuscito, e vale false anche prima del primo tentativo e
   * quando il modulo è disattivato per assenza di credenziali: in nessuno dei
   * due casi c'è un guasto da mostrare. Una radio caduta non lo alza: in quel
   * caso lo scheduler non chiama nemmeno runPublish().
   *
   * Usato da: Weather.h (drawBanner) per il titolo del riquadro Indoor, e dal
   * onOutcome() del task nel .ino per decidere se serve un ridisegno.
   */
  inline bool hasFailed()
  {
    return detail::failedLast;
  }
}

#endif

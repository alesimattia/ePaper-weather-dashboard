#ifndef TUYA_H
#define TUYA_H

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <mqtt_client.h>
#include <esp_crt_bundle.h>
#include <mbedtls/md.h>

#include "Env.h"
#include "Indoor.h"

/**
 * Modulo header-only di telemetria verso il cloud Tuya, protocollo TuyaLink.
 *
 * Pubblica le cinque grandezze del BME680 lette da Indoor.h su un unico topic
 * MQTT, così che il dispositivo, una volta associato con il QR code generato
 * dalla console Tuya, mostri i dati nell'app Android Smart Life.
 *
 * Flusso di una pubblicazione, sincrona e con una sessione MQTT per messaggio:
 *   1. autenticazione: username e password sono derivati dal DeviceSecret con
 *      HMAC-SHA256 su un timestamp Unix, quindi serve l'orologio sincronizzato;
 *   2. connessione TLS 1.2 al broker della propria region, porta 8883;
 *   3. publish in QoS 1 su tylink/{deviceId}/thing/property/report;
 *   4. attesa del PUBACK e chiusura immediata della sessione. Con
 *      TUYA_REQUEST_ACK si attende anche la conferma applicativa del cloud,
 *      che è l'unico modo per accorgersi di un device model non allineato.
 *
 * Il client MQTT è quello nativo del core (esp-mqtt, già linkato) e l'HMAC
 * viene da mbedtls: non serve nessuna libreria aggiuntiva. Il certificato del
 * broker è verificato con il certificate bundle di mbedtls compilato nel core,
 * quindi non c'è nessun PEM da incorporare e da correggere quando Tuya ruota
 * la CA. A differenza degli altri moduli, che usano setInsecure(), qui la
 * verifica va fatta: su questa connessione transita l'HMAC del DeviceSecret.
 *
 * API, allineata a quella degli altri moduli di rete:
 *   - Tuya::begin()          : compone topic e client id, azzera lo stato;
 *   - Tuya::pendingPublish() : true se è ora di pubblicare (funzione pura);
 *   - Tuya::runPublish()     : pubblica, presuppone la STA connessa.
 *
 * Invariante di runPublish(): al ritorno non esiste nessun task esp-mqtt vivo
 * nè nessun handle allocato, in qualunque modo sia andata. È la condizione che
 * rende sicuro il light sleep che il .ino avvia poco dopo, ed è la ragione per
 * cui il modulo non espone nessuna funzione di stop da chiamare prima di
 * dormire: sarebbe codice morto che suggerisce il contrario.
 *
 * Il modulo non disegna niente e non chiama Weather::markDirty(): un refresh
 * pieno del pannello costa ~24 s e la telemetria non cambia nulla a schermo.
 */

/**
 * Broker MQTT della region in cui è stato creato il prodotto sulla console
 * Tuya. Deve coincidere con la region dell'account Smart Life: un broker
 * sbagliato risponde con un errore di autenticazione, indistinguibile da
 * credenziali errate.
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
 * Identifier delle properties nel device model creato sulla console, con il
 * moltiplicatore che converte il valore float del sensore nell'intero che
 * Tuya si aspetta.
 *
 * ATTENZIONE: il moltiplicatore deve corrispondere alla "scale" dichiarata in
 * console per quella property, ed è l'errore di configurazione più insidioso
 * del modulo. Con scale 1 e moltiplicatore 1 la temperatura appare divisa per
 * dieci; con scale 0 e moltiplicatore 10 appare decuplicata. In nessuno dei
 * due casi si vede un errore: il PUBACK conferma la consegna MQTT, non
 * l'accettazione del valore. Va verificato in "Device Debugging" sulla
 * console prima di guardare l'app.
 *
 * I valori qui sotto sono un default plausibile: gli identifier veri si
 * leggono dalla console dopo aver creato le properties.
 */
#ifndef TUYA_DP_TEMPERATURE
#define TUYA_DP_TEMPERATURE "temp_current"
#endif
#ifndef TUYA_MUL_TEMPERATURE
#define TUYA_MUL_TEMPERATURE 10L
#endif

#ifndef TUYA_DP_HUMIDITY
#define TUYA_DP_HUMIDITY "humidity_value"
#endif
#ifndef TUYA_MUL_HUMIDITY
#define TUYA_MUL_HUMIDITY 1L
#endif

#ifndef TUYA_DP_PRESSURE
#define TUYA_DP_PRESSURE "pressure_value"
#endif
#ifndef TUYA_MUL_PRESSURE
#define TUYA_MUL_PRESSURE 1L
#endif

#ifndef TUYA_DP_IAQ
#define TUYA_DP_IAQ "air_quality_index"
#endif
#ifndef TUYA_MUL_IAQ
#define TUYA_MUL_IAQ 1L
#endif

/** Accuratezza del calibratore BSEC, 0..3: dice se l'IAQ è attendibile. */
#ifndef TUYA_DP_IAQ_ACCURACY
#define TUYA_DP_IAQ_ACCURACY "iaq_accuracy"
#endif

/** Cadenza di pubblicazione in minuti. Il valore reale sta nel .ino. */
#ifndef TUYA_PUBLISH_MIN
#define TUYA_PUBLISH_MIN 5
#endif

/**
 * Pavimento epoch oltre il quale l'orologio è considerato sincronizzato.
 * Si aggancia da sè a quello del .ino quando è definito, così le due soglie
 * non possono divergere, e resta autonomo se l'header viene compilato da solo.
 */
#ifndef TUYA_EPOCH_FLOOR
#ifdef TIME_VALID_EPOCH_MIN
#define TUYA_EPOCH_FLOOR TIME_VALID_EPOCH_MIN
#else
#define TUYA_EPOCH_FLOOR 1700000000L
#endif
#endif

/** Attesa massima del CONNACK, che comprende l'handshake TLS. */
#ifndef TUYA_CONNECT_TIMEOUT_MS
#define TUYA_CONNECT_TIMEOUT_MS 8000UL
#endif

/** Attesa massima del PUBACK dopo il publish in QoS 1. */
#ifndef TUYA_PUBACK_TIMEOUT_MS
#define TUYA_PUBACK_TIMEOUT_MS 3000UL
#endif

/** Stack del task esp-mqtt: esplicito perchè l'handshake TLS gira lì dentro. */
#ifndef TUYA_MQTT_TASK_STACK
#define TUYA_MQTT_TASK_STACK 6144
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

/** Attesa massima della risposta del cloud, usata solo con TUYA_REQUEST_ACK. */
#ifndef TUYA_ACK_TIMEOUT_MS
#define TUYA_ACK_TIMEOUT_MS 2000UL
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
    inline constexpr uint32_t PUBLISH_INTERVAL_MS = (uint32_t)TUYA_PUBLISH_MIN * 60UL * 1000UL;

    /** Composti una volta in begin(): dipendono solo dal DeviceID. */
    inline char clientId[48] = {0};
    inline char topic[96]    = {0};
#if TUYA_REQUEST_ACK
    inline char ackTopic[112] = {0};
#endif

    /** Ricomposti a ogni publish: dipendono dal timestamp corrente. */
    inline char username[192] = {0};
    inline char signSrc[192]  = {0};
    inline char password[65]  = {0};
    inline char payload[384]  = {0};

    inline esp_mqtt_client_handle_t client = nullptr;
    inline bool     enabled       = false;
    inline bool     firstPublish  = true;
    inline uint32_t lastPublishMs = 0;
    inline uint32_t msgSeq        = 0;

    /**
     * Esito dell'ultimo tentativo speso, letto dal rendering per segnalare il
     * guasto sul pannello. Parte da false perchè al boot, prima di qualunque
     * tentativo, non c'è niente da segnalare, e resta false quando il modulo
     * è disattivato: un dispositivo senza credenziali Tuya non è guasto.
     */
    inline bool failedLast = false;

    /**
     * Flag scritti dall'event handler, che gira nel task esp-mqtt, e letti dal
     * task Arduino. Bastano dei volatile bool perchè l'handler non pubblica
     * nessun dato verso il chiamante: codici di errore e message id li logga
     * da sè, quindi restano solo store e load atomici.
     */
    inline volatile bool connected = false;
    inline volatile bool published = false;
    inline volatile bool failed    = false;
#if TUYA_REQUEST_ACK
    inline volatile bool subscribed  = false;
    inline volatile bool ackReceived = false;
#endif

    /**
     * Sottrazione signed per confronto rollover-safe di millis().
     * Stesso pattern di Indoor e Weather, per coerenza sulla gestione del
     * wrap a 32 bit (~49 giorni).
     */
    inline bool elapsed(uint32_t now, uint32_t last, uint32_t interval)
    {
      return (int32_t)(now - last) >= (int32_t)interval;
    }

    /** Vero se l'orologio di sistema è stato sincronizzato via SNTP. */
    inline bool clockIsValid()
    {
      return time(nullptr) >= (time_t)TUYA_EPOCH_FLOOR;
    }

    /**
     * Converte una misura in virgola mobile nell'intero atteso da Tuya.
     * lroundf e non un cast: il cast troncherebbe (23,49 con moltiplicatore
     * 10 darebbe 234 invece di 235) e sui negativi troncherebbe verso zero.
     */
    inline long toDp(float value, long mul)
    {
      if (isnan(value)) return 0;
      return lroundf(value * (float)mul);
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
     * publish: Indoor::refresh() gira a valle del blocco dei fetch, quindi la
     * cache contiene il campione del wake precedente e datarlo "adesso"
     * sarebbe sbagliato. Il time per property viene omesso perchè quello di
     * messaggio vale per tutte le proprietà, e la documentazione Tuya ammette
     * esplicitamente sia i secondi a 10 cifre sia i millisecondi a 13. Anche
     * l'SDK ufficiale usa i secondi: il suo system_timestamp() ritorna un
     * uint32_t, che non basterebbe a contenere un epoch in millisecondi.
     *
     * Con TUYA_REQUEST_ACK la coda porta anche il campo sys.ack, che chiede al
     * cloud di rispondere sul topic di report_response.
     */
    inline bool buildPayload(const Indoor::Sample& s, unsigned long ts)
    {
      const unsigned long ageSec   = (millis() - s.lastUpdateMs) / 1000UL;
      const unsigned long sampleTs = (ageSec < ts) ? (ts - ageSec) : ts;

      int n = snprintf(payload, sizeof(payload),
                       "{\"msgId\":\"%lu\",\"time\":%lu,\"data\":{"
                       "\"%s\":{\"value\":%ld},"
                       "\"%s\":{\"value\":%ld},"
                       "\"%s\":{\"value\":%ld},"
                       "\"%s\":{\"value\":%ld},"
                       "\"%s\":{\"value\":%ld}}" TUYA_SYS_ACK_FIELD "}",
                       (unsigned long)++msgSeq, sampleTs,
                       TUYA_DP_TEMPERATURE,  toDp(s.temperature, TUYA_MUL_TEMPERATURE),
                       TUYA_DP_HUMIDITY,     toDp(s.humidity,    TUYA_MUL_HUMIDITY),
                       TUYA_DP_PRESSURE,     toDp(s.pressure,    TUYA_MUL_PRESSURE),
                       TUYA_DP_IAQ,          toDp(s.iaq,         TUYA_MUL_IAQ),
                       TUYA_DP_IAQ_ACCURACY, (long)s.iaqAccuracy);

      return n > 0 && (size_t)n < sizeof(payload);
    }

    /**
     * Attende che un flag venga alzato dall'event handler, uscendo anche su
     * failed per non consumare il timeout intero quando il broker ha già
     * rifiutato la connessione. Il poll con delay() replica il pattern del
     * busy-wait di wifiOn() nel .ino; delay() è vTaskDelay(), quindi cede la
     * CPU e lascia girare i task di rete e gli idle.
     */
    inline bool waitFlag(volatile bool& flag, uint32_t timeoutMs)
    {
      uint32_t t0 = millis();
      while (!flag && !failed && (millis() - t0) < timeoutMs)
      {
        delay(10);
      }
      return flag;
    }

    /**
     * Chiude la sessione e libera l'handle. Va chiamata su ogni percorso di
     * uscita di runPublish(): un task esp-mqtt che sopravvivesse resterebbe in
     * attesa su un socket morto mentre il .ino spegne la radio e va in light
     * sleep.
     *
     * esp_mqtt_client_stop() non viene omessa affidandosi a destroy(): la sua
     * documentazione dichiara solo di distruggere l'handle e non garantisce di
     * fermare il task. Il suo ESP_FAIL su client in stato non valido è un
     * esito accettabile, quindi il ritorno si ignora.
     */
    inline void shutdown()
    {
      if (client == nullptr) return;

      if (connected) esp_mqtt_client_disconnect(client);
      esp_mqtt_client_stop(client);
      esp_mqtt_client_destroy(client);

      client    = nullptr;
      connected = false;
    }

    /**
     * Event handler del client, invocato nel task esp-mqtt.
     *
     * Scrive i flag e logga gli errori qui dentro, senza passare dati al task
     * chiamante. Il log dell'errore è l'unica diagnostica che distingue i modi
     * di fallimento del bring-up, che altrimenti si presentano tutti come "non
     * si connette": un connect return code 0x04 o 0x05 punta a credenziali,
     * firma o timestamp, un errore di trasporto punta a CA, DNS o region
     * sbagliata.
     */
    inline void onMqttEvent(void* /*handler_args*/, esp_event_base_t /*base*/,
                            int32_t event_id, void* event_data)
    {
      esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;

      switch ((esp_mqtt_event_id_t)event_id)
      {
        case MQTT_EVENT_CONNECTED:
          connected = true;
          break;

        case MQTT_EVENT_PUBLISHED:
          // In QoS 1 l'evento è il PUBACK del broker. Per sessione si pubblica
          // un solo messaggio, quindi non serve confrontare il message id, e
          // non confrontarlo evita di leggere un dato che il chiamante sta
          // ancora scrivendo mentre il publish è in corso.
          published = true;
          break;

        case MQTT_EVENT_DISCONNECTED:
          connected = false;
          failed    = true;
          break;

#if TUYA_REQUEST_ACK
        case MQTT_EVENT_SUBSCRIBED:
          subscribed = true;
          break;

        case MQTT_EVENT_DATA:
        {
          /**
           * Risposta del cloud al property report: msgId, time e un campo
           * code, dove 0 significa accettato. In caso di rifiuto il corpo
           * porta anche il motivo, quindi viene loggato per intero: è
           * l'informazione che manca quando i valori non compaiono nell'app
           * malgrado il PUBACK.
           *
           * La risposta è di poche decine di byte e il buffer di esp-mqtt è
           * di 1024, quindi arriva sempre in un frammento unico e non serve
           * riassemblare su current_data_offset. e->data non è terminato da
           * zero: va copiato.
           */
          char body[160];
          int n = (e == nullptr) ? 0 : e->data_len;
          if (n > (int)sizeof(body) - 1) n = (int)sizeof(body) - 1;
          if (n > 0) memcpy(body, e->data, (size_t)n);
          body[n > 0 ? n : 0] = 0;

          const char* p = (n > 0) ? strstr(body, "\"code\":") : nullptr;
          long code = (p != nullptr) ? strtol(p + 7, nullptr, 10) : -1;

          if (code == 0)
            Serial.println(F("[Tuya] il cloud ha accettato le proprietà (code 0)"));
          else
            Serial.printf("[Tuya] il cloud ha RIFIUTATO le proprietà: %s\n", body);

          ackReceived = true;
          break;
        }
#endif

        case MQTT_EVENT_ERROR:
          if (e != nullptr && e->error_handle != nullptr)
          {
            if (e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
              Serial.printf("[Tuya] connessione rifiutata, return code 0x%02x\n",
                            (unsigned)e->error_handle->connect_return_code);
            else
              Serial.printf("[Tuya] errore di trasporto: esp_tls=0x%x stack=0x%x errno=%d\n",
                            (unsigned)e->error_handle->esp_tls_last_esp_err,
                            e->error_handle->esp_tls_stack_err,
                            e->error_handle->esp_transport_sock_errno);
          }
          failed = true;
          break;

        default:
          break;
      }
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

    client        = nullptr;
    connected     = false;
    published     = false;
    failed        = false;
    firstPublish  = true;
    lastPublishMs = 0;
    msgSeq        = 0;

    enabled = strlen(TUYA_DEVICE_ID) >= 8 &&
              strlen(TUYA_DEVICE_SECRET) >= 8 &&
              strcmp(TUYA_DEVICE_ID, "paste_device_id_here") != 0 &&
              strcmp(TUYA_DEVICE_SECRET, "paste_device_secret_here") != 0;

    if (!enabled)
    {
      Serial.println(F("[Tuya] credenziali assenti in Env.h: telemetria disattivata"));
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

    Serial.printf("[Tuya] init ok, publish ogni %d min verso %s\n",
                  (int)TUYA_PUBLISH_MIN, TUYA_MQTT_URI);
#if TUYA_REQUEST_ACK
    Serial.println(F("[Tuya] conferma applicativa del cloud attiva (bring-up)"));
#endif
  }

  /**
   * Vero quando è ora di pubblicare. Funzione pura: non tocca nè lo stato nè
   * la radio, così il .ino può usarla per decidere se accendere il WiFi.
   *
   * Il controllo sul campione valido sta qui e non in runPublish() di
   * proposito: al cold start BSEC produce il primo output dopo qualche minuto,
   * e tenerlo qui evita sia di accendere la radio per niente sia di consumare
   * gli slot di un sensore che non ha ancora misurato.
   */
  inline bool pendingPublish()
  {
    using namespace detail;

    if (!enabled) return false;
    if (!Indoor::sample().valid) return false;
    if (firstPublish) return true;
    return elapsed(millis(), lastPublishMs, PUBLISH_INTERVAL_MS);
  }

  /**
   * Pubblica il campione corrente e attende la conferma del broker.
   * Presuppone la STA connessa e l'orologio sincronizzato.
   *
   * Al ritorno non resta nessun task esp-mqtt vivo nè nessun handle allocato,
   * qualunque sia l'esito: è l'invariante che rende sicuro il light sleep del
   * .ino, ed è il motivo per cui i passi della sequenza sono concatenati in un
   * solo if, così che il cleanup sia raggiunto da ogni percorso.
   *
   * @return true solo se il PUBACK è arrivato, cioè se il dato è stato
   *         consegnato. Attenzione: conferma la consegna MQTT e non
   *         l'accettazione dei valori, che dipende dal device model.
   */
  inline bool runPublish()
  {
    using namespace detail;

    if (!enabled) return false;

    /**
     * Nessun campione: non c'è niente da pubblicare, quindi lo slot resta
     * intatto e non si segnala nessun guasto. Vale anche per il cold start,
     * dove BSEC produce il primo output dopo qualche minuto.
     */
    const Indoor::Sample& s = Indoor::sample();
    if (!s.valid) return false;

    /**
     * Da qui lo slot è speso e lo stato è pessimista: è la convenzione già
     * adottata da fetchCinemaImage() nel .ino, applicata prima di qualunque
     * operazione fallibile e prima della verifica del WiFi.
     *
     * Due conseguenze volute. La prima: qualunque fallimento, radio assente
     * compresa, non viene ritentato prima del prossimo TUYA_PUBLISH_MIN,
     * quindi un cloud irraggiungibile non costa un handshake TLS per wake.
     * La seconda: ogni uscita anticipata lascia il guasto segnalato, e solo
     * il PUBACK lo azzera, quindi la segnalazione resta corretta anche se in
     * futuro si aggiunge un return in mezzo alla sequenza.
     */
    lastPublishMs = millis();
    firstPublish  = false;
    failedLast    = true;

    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println(F("[Tuya] WiFi non connesso, publish saltato"));
      return false;
    }

    if (!clockIsValid())
    {
      Serial.println(F("[Tuya] orologio non sincronizzato, publish saltato"));
      return false;
    }

    const unsigned long ts = (unsigned long)time(nullptr);

    if (!buildCredentials(ts))
    {
      Serial.println(F("[Tuya] costruzione delle credenziali fallita"));
      return false;
    }
    if (!buildPayload(s, ts))
    {
      Serial.println(F("[Tuya] payload troppo lungo per il buffer"));
      return false;
    }

    connected = false;
    published = false;
    failed    = false;
#if TUYA_REQUEST_ACK
    subscribed  = false;
    ackReceived = false;
#endif

    esp_mqtt_client_config_t cfg = {};
    // Assegnazioni campo per campo e non designated initializer: la struct di
    // IDF 5.x annida struct anonime, che in C++ non si inizializzano in modo
    // portabile per designatore.
    cfg.broker.address.uri                    = TUYA_MQTT_URI;
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.credentials.client_id                 = clientId;
    cfg.credentials.username                  = username;
    cfg.credentials.authentication.password   = password;
    // Tuya accetta solo MQTT 3.1.1: dichiarato qui anche se il core è
    // compilato con CONFIG_MQTT_PROTOCOL_311, così il vincolo resta scritto
    // dove è verificabile.
    cfg.session.protocol_ver                  = MQTT_PROTOCOL_V_3_1_1;
    cfg.session.keepalive                     = 60;
    // Senza questo, dopo un fallimento il task riproverebbe da solo dopo
    // qualche secondo, cioè mentre il .ino ha già spento la radio e sta
    // entrando in light sleep.
    cfg.network.disable_auto_reconnect        = true;
    cfg.network.timeout_ms                    = (int)TUYA_CONNECT_TIMEOUT_MS;
    cfg.task.stack_size                       = TUYA_MQTT_TASK_STACK;

    client = esp_mqtt_client_init(&cfg);
    if (client == nullptr)
    {
      Serial.printf("[Tuya] init del client fallita, heap libero %u\n",
                    (unsigned)ESP.getFreeHeap());
      return false;
    }

    bool ok = false;
    if (esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, onMqttEvent, nullptr) == ESP_OK &&
        esp_mqtt_client_start(client) == ESP_OK &&
        waitFlag(connected, TUYA_CONNECT_TIMEOUT_MS))
    {
#if TUYA_REQUEST_ACK
      /**
       * Sottoscrizione al topic di risposta prima del publish, con attesa del
       * SUBACK: pubblicare con la sottoscrizione non ancora attiva farebbe
       * perdere la risposta. Un fallimento qui non blocca la telemetria, si
       * perde solo la conferma.
       */
      if (esp_mqtt_client_subscribe(client, ackTopic, 0) >= 0)
        waitFlag(subscribed, TUYA_ACK_TIMEOUT_MS);
      else
        Serial.println(F("[Tuya] sottoscrizione al topic di risposta fallita"));
#endif

      int msgId = esp_mqtt_client_publish(client, topic, payload, 0, /*qos*/ 1, /*retain*/ 0);
      if (msgId < 0)
        Serial.printf("[Tuya] publish rifiutato dal client (%d)\n", msgId);
      else
        ok = waitFlag(published, TUYA_PUBACK_TIMEOUT_MS);

#if TUYA_REQUEST_ACK
      /**
       * L'esito di runPublish() resta quello del PUBACK: la conferma del
       * cloud è diagnostica e il suo codice viene loggato dall'event handler,
       * quindi un rifiuto applicativo non va confuso con una mancata
       * consegna. Attesa solo se il messaggio è stato consegnato: senza
       * PUBACK non c'è nulla che il cloud possa confermare.
       */
      if (ok && !waitFlag(ackReceived, TUYA_ACK_TIMEOUT_MS))
        Serial.println(F("[Tuya] nessuna conferma dal cloud entro il timeout"));
#endif
    }

    shutdown();

    if (ok)
    {
      // Unico punto che azzera il guasto: solo un PUBACK dimostra che il
      // dato è arrivato.
      failedLast = false;
      Serial.printf("[Tuya] publish ok: T=%.1f RH=%.1f P=%.1f IAQ=%.0f acc=%u\n",
                    s.temperature, s.humidity, s.pressure, s.iaq,
                    (unsigned)s.iaqAccuracy);
    }
    else
      Serial.println(F("[Tuya] publish non confermato"));

    return ok;
  }

  /**
   * Vero se l'ultimo tentativo speso è fallito, per qualunque ragione: WiFi
   * assente, orologio non sincronizzato, broker irraggiungibile, credenziali
   * rifiutate o PUBACK mancante.
   *
   * Serve al rendering per segnalare il guasto sul pannello. Torna false al
   * primo publish riuscito, e vale false anche prima del primo tentativo e
   * quando il modulo è disattivato per assenza di credenziali: in nessuno dei
   * due casi c'è un guasto da mostrare.
   *
   * Usato da: Weather.h (drawBanner) per il titolo del riquadro Indoor.
   */
  inline bool hasFailed()
  {
    return detail::failedLast;
  }
}

#endif

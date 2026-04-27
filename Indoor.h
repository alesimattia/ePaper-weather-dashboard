#ifndef INDOOR_H
#define INDOOR_H

#include <Arduino.h>
#include <Wire.h>
#include <bsec2.h>
#include <Preferences.h>

/**
 * Modulo header-only del sensore ambientale Bosch BME680 (I2C).
 *
 * Integrazione con libreria Bosch BSEC2 in modalita' ULP (sample period
 * = 300 s). BSEC fornisce T e RH heat-compensated, pressione raw e IAQ
 * calibrato (0-500) con livello di accuratezza 0..3. Lo stato del
 * calibratore viene salvato su NVS (namespace "bme680") ogni 6 ore e
 * ripristinato all'avvio, azzerando il warm-up dopo un power cycle.
 *
 * Il modulo e' locale: nessuna rete, nessuna UI in questa fase.
 * Lo sketch chiama refresh() ad ogni giro di loop() e, quando la
 * funzione ritorna true (nuovo campione BSEC pronto), invoca
 * Weather::markDirty() per innescare il refresh del display.
 *
 * Usato da: GxEPD2_1330c_GDEM133Z91.ino
 *
 * @since 21/04/26 Mattia Alesi
 */

/* --- Configurazione BME680 (I2C) ---
 * Costanti locali al modulo: non sono segreti e non hanno motivo di
 * stare in Env.h. Cambiare qui se si sposta il sensore su altri pin
 * o se il breakout espone indirizzo 0x76 (SDO a GND).
 */
#define BME680_I2C_ADDR   0x77
#define BME680_SDA_PIN    21
#define BME680_SCL_PIN    22

namespace Indoor
{
  /**
   * Ultimo campione prodotto dalla libreria BSEC2. Finche' BSEC non
   * ha stabilizzato il primo output, valid=false e tutti i campi a
   * zero; dopo un cold start ci vogliono ~5 min per il primo sample
   * (immediato invece quando lo stato e' stato ripristinato da NVS).
   */
  struct Sample
  {
    bool     valid;
    float    temperature;   // °C (heat compensated)
    float    humidity;      // %RH (heat compensated)
    float    pressure;      // hPa
    float    iaq;           // 0-500
    uint8_t  iaqAccuracy;   // 0 (stabilizing) -> 3 (calibrated)
    uint32_t lastUpdateMs;  // millis() del sample corrente
  };

  // -------------------------------------------------------------------------
  // Stato e helper interni. Raggruppati in `detail` per non inquinare il
  // namespace pubblico del modulo. Gli `inline` (variabili + funzioni)
  // permettono la definizione nell'header senza violare la ODR (C++17).
  // -------------------------------------------------------------------------
  namespace detail
  {
    /** Namespace e chiave Preferences per persistere lo stato BSEC. */
    inline constexpr const char* NVS_NAMESPACE = "bme680";
    inline constexpr const char* NVS_KEY_STATE = "state";

    /**
     * Cadenza di salvataggio dello stato BSEC su NVS.
     * Compromesso fra usura della flash e precisione post-reboot:
     * 6 h significa pochissime scritture/giorno, preservando comunque
     * gran parte della calibrazione.
     */
    inline constexpr uint32_t STATE_SAVE_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;

    inline Bsec2    bsec;
    inline Sample   current          = {};
    inline bool     bsecOk           = false;
    inline bool     hasNewSample     = false;
    inline uint32_t lastStateSaveMs  = 0;

    /**
     * Sottrazione signed per confronto rollover-safe di millis().
     * Replica il pattern gia' usato in Weather per mantenere coerenza
     * sulla gestione del wrap a 32 bit (~49 giorni).
     * @since 21/04/26 Mattia Alesi
     */
    inline bool elapsed(uint32_t now, uint32_t last, uint32_t interval)
    {
      return (int32_t)(now - last) >= (int32_t)interval;
    }

    /**
     * Callback invocato da BSEC quando e' pronto un nuovo set di output.
     * Aggiorna la cache `current` e setta il flag `hasNewSample` che
     * Indoor::refresh() leggera' al ritorno da bsec.run().
     * Firma imposta dalla libreria Bsec2.
     * @since 21/04/26 Mattia Alesi
     */
    inline void onBsecData(const bme68xData /*rawData*/,
                           const bsecOutputs outputs,
                           Bsec2 /*bsec*/)
    {
      if (outputs.nOutputs == 0) return;

      for (uint8_t i = 0; i < outputs.nOutputs; i++)
      {
        const bsecData& out = outputs.output[i];
        switch (out.sensor_id)
        {
          case BSEC_OUTPUT_IAQ:
            current.iaq         = out.signal;
            current.iaqAccuracy = out.accuracy;
            break;
          case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
            current.temperature = out.signal;
            break;
          case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
            current.humidity = out.signal;
            break;
          case BSEC_OUTPUT_RAW_PRESSURE:
            current.pressure = out.signal / 100.0f;   // Pa -> hPa
            break;
          default:
            break;
        }
      }

      current.valid        = true;
      current.lastUpdateMs = millis();
      hasNewSample         = true;
    }

    /**
     * Salva il blob dello stato BSEC in NVS. Chiamata solo quando
     * l'accuratezza e' almeno 1 (evita di persistere stati transitori).
     * @since 21/04/26 Mattia Alesi
     */
    inline void saveBsecState()
    {
      uint8_t buf[BSEC_MAX_STATE_BLOB_SIZE];
      if (!bsec.getState(buf))
      {
        Serial.printf("[BME680] getState failed: %d\n", bsec.status);
        return;
      }

      Preferences prefs;
      if (!prefs.begin(NVS_NAMESPACE, false))
      {
        Serial.println(F("[BME680] NVS open RW failed"));
        return;
      }
      size_t written = prefs.putBytes(NVS_KEY_STATE, buf, BSEC_MAX_STATE_BLOB_SIZE);
      prefs.end();

      if (written == BSEC_MAX_STATE_BLOB_SIZE)
        Serial.printf("[BME680] state saved to NVS (%u bytes)\n", (unsigned)written);
      else
        Serial.printf("[BME680] state partial save: %u/%u\n",
                      (unsigned)written, (unsigned)BSEC_MAX_STATE_BLOB_SIZE);
    }

    /**
     * Ripristina lo stato BSEC da NVS se presente.
     * Chiamata da begin() prima di updateSubscription() per permettere a
     * BSEC di inizializzarsi gia' calibrato.
     * @since 21/04/26 Mattia Alesi
     */
    inline void restoreBsecState()
    {
      Preferences prefs;
      if (!prefs.begin(NVS_NAMESPACE, true))
      {
        Serial.println(F("[BME680] no saved state, cold start"));
        return;
      }

      size_t len = prefs.getBytesLength(NVS_KEY_STATE);
      if (len != BSEC_MAX_STATE_BLOB_SIZE)
      {
        prefs.end();
        Serial.println(F("[BME680] no saved state, cold start"));
        return;
      }

      uint8_t buf[BSEC_MAX_STATE_BLOB_SIZE];
      prefs.getBytes(NVS_KEY_STATE, buf, BSEC_MAX_STATE_BLOB_SIZE);
      prefs.end();

      if (bsec.setState(buf))
        Serial.printf("[BME680] state restored (%u bytes)\n", (unsigned)BSEC_MAX_STATE_BLOB_SIZE);
      else
        Serial.printf("[BME680] setState failed: %d\n", bsec.status);
    }
  } // namespace detail

  /**
   * Inizializza Wire, avvia BSEC2 sul BME680, ripristina l'eventuale
   * stato salvato in NVS e sottoscrive gli output in modalita' ULP
   * (un sample ogni 300 s, coerente col light sleep del ciclo main).
   * Se il sensore non risponde si degrada silenziosamente: il resto
   * del firmware (meteo/calendario/display) continua a funzionare.
   * @since 21/04/26 Mattia Alesi
   */
  inline void begin()
  {
    using namespace detail;
    current         = {};
    bsecOk          = false;
    hasNewSample    = false;
    lastStateSaveMs = 0;

    Wire.begin(BME680_SDA_PIN, BME680_SCL_PIN);

    if (!bsec.begin(BME680_I2C_ADDR, Wire))
    {
      Serial.printf("[BME680] init failed (bsec=%d, sensor=%d)\n",
                    bsec.status, bsec.sensor.status);
      return;
    }

    // Prima di sottoscrivere gli output: se esiste uno stato BSEC salvato
    // in NVS lo ricarichiamo, saltando cosi' il warm-up IAQ.
    restoreBsecState();

    static bsecSensor sensorList[] = {
      BSEC_OUTPUT_IAQ,
      BSEC_OUTPUT_STABILIZATION_STATUS,
      BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
      BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
      BSEC_OUTPUT_RAW_PRESSURE
    };
    const uint8_t sensorCount = sizeof(sensorList) / sizeof(sensorList[0]);

    if (!bsec.updateSubscription(sensorList, sensorCount, BSEC_SAMPLE_RATE_ULP))
    {
      Serial.printf("[BME680] updateSubscription failed: %d\n", bsec.status);
      return;
    }

    bsec.attachCallback(onBsecData);

    bsecOk          = true;
    lastStateSaveMs = millis();
    Serial.println(F("[BME680] init ok, ULP 5 min"));
  }

  /**
   * Polling non bloccante da chiamare ad ogni giro di loop().
   * BSEC temporizza internamente i 5 min, quindi la chiamata e' a
   * costo trascurabile quando nessun sample e' dovuto.
   * @return true se un nuovo campione e' stato prodotto in questa
   *         chiamata: il chiamante usa il valore per innescare un
   *         refresh del display via Weather::markDirty().
   * @since 21/04/26 Mattia Alesi
   */
  inline bool refresh()
  {
    using namespace detail;
    if (!bsecOk) return false;

    hasNewSample = false;
    if (!bsec.run())
    {
      // run() ritorna false anche quando semplicemente non ci sono dati:
      // non e' un errore fatale. Logghiamo solo se lo status lo segnala.
      if (bsec.status < BSEC_OK || bsec.sensor.status < BME68X_OK)
        Serial.printf("[BME680] run error (bsec=%d, sensor=%d)\n",
                      bsec.status, bsec.sensor.status);
      return false;
    }

    if (!hasNewSample) return false;

    Serial.printf("[BME680] T=%.1fC RH=%.1f%% P=%.1fhPa IAQ=%.0f acc=%u\n",
                  current.temperature, current.humidity, current.pressure,
                  current.iaq, (unsigned)current.iaqAccuracy);

    // Salvataggio stato gated: solo a distanza >= 6 h dall'ultimo e solo
    // quando il calibratore ha raggiunto almeno accuracy=1. Evita di
    // sovrascrivere uno stato buono con uno transitorio post-reboot.
    if (current.iaqAccuracy >= 1 &&
        elapsed(millis(), lastStateSaveMs, STATE_SAVE_INTERVAL_MS))
    {
      saveBsecState();
      lastStateSaveMs = millis();
    }

    return true;
  }

  /**
   * Ultimo campione valido (riferimento a cache interna).
   * @since 21/04/26 Mattia Alesi
   */
  inline const Sample& sample()
  {
    return detail::current;
  }

  /**
   * Mappa il valore IAQ (0-500) a una breve etichetta in italiano
   * descrittiva della qualita' dell'aria. Scala ufficiale Bosch BSEC:
   *
   *   IAQ         |  Etichetta  |  Significato
   *   ------------|-------------|-------------------------
   *     0..  50   |  "Ottima"   |  aria pulita
   *    51.. 100   |  "Buona"    |  aria tipica indoor
   *   101.. 150   |  "Discreta" |  leggermente inquinata
   *   151.. 200   |  "Mediocre" |  moderatamente inquinata
   *   201.. 250   |  "Cattiva"  |  pesantemente inquinata
   *   251.. 350   |  "Pessima"  |  gravemente inquinata
   *    > 350      |  "Nociva"   |  aria nociva
   *
   * Usato da: Weather.h (renderIndoorSubColumn) per mostrare la
   * condizione accanto al valore numerico IAQ.
   *
   * @since 21/04/26 Mattia Alesi
   */
  inline const char* iaqLabel(float iaq)
  {
    if (iaq <=  50.0f) return "Ottima";
    if (iaq <= 100.0f) return "Buona";
    if (iaq <= 150.0f) return "Discreta";
    if (iaq <= 200.0f) return "Mediocre";
    if (iaq <= 250.0f) return "Cattiva";
    if (iaq <= 350.0f) return "Pessima";
    return "Nociva";
  }
}

#endif

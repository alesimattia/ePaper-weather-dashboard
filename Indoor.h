#ifndef INDOOR_H
#define INDOOR_H

#include <Arduino.h>
#include <Wire.h>
#include <bsec2.h>
#include <Preferences.h>
#include "Timings.h"
#include "Log.h"

/**
 * Modulo header-only del sensore ambientale Bosch BME680 (I2C).
 *
 * Integrazione con libreria Bosch BSEC2 in modalita' ULP (sample period = 300 s). 
 * BSEC fornisce T e RH heat-compensated, pressione raw e IAQ calibrato (0-500) con livello di accuratezza 0..3. 
 * Lo stato del calibratore viene salvato su NVS (namespace "bme680") ogni 6 ore 
 * e ripristinato all'avvio, azzerando il warm-up dopo un power cycle.
 *
 * Lo scheduler chiama refresh() alla scadenza di msUntilNextPoll() e, quando
 * la funzione ritorna true (nuovo campione o campione scaduto), il .ino chiede
 * il ridisegno del display, solo dentro la fascia WiFi.
 */
#define BME680_I2C_ADDR   0x77
#define BME680_SDA_PIN    21
#define BME680_SCL_PIN    22

/** Clock dell'I2C. Il limite reale non è quello dei datasheet ma quello del
 *  cablaggio (pull-up e capacità dei fili): il valore va preso dal clock
 *  massimo affidabile che I2C_DEBUG misura sul bus, uno scalino sotto se il
 *  margine è stretto. A 400 kHz la lettura di un campione scende da circa 3 ms
 *  a meno di 1 ms. Wire non accetta valori oltre 1 MHz. */
#define BME680_I2C_CLOCK_HZ 400000UL

/**
 * Diagnosi del bus I2C al boot, da accendere durante il bring-up del sensore.
 * A 1 begin() sonda i due soli indirizzi possibili del BME68x (quello
 * configurato e l'altro), verifica il chip ID e misura il clock massimo a cui
 * il blocco di calibrazione si rilegge identico, poi avvia BSEC su indirizzo e
 * clock trovati, loggando quali costanti correggere. Costa ~100 ms una volta
 * sola; se il sensore non c'è il modulo resta un no-op come senza flag.
 * A 0 begin() usa BME680_I2C_ADDR e BME680_I2C_CLOCK_HZ così come sono: va
 * riportato a 0 dopo aver fissato le due costanti.
 */
#ifndef I2C_DEBUG
#define I2C_DEBUG 1
#endif

namespace Indoor
{
  /**
   * Ultimo campione prodotto dalla libreria BSEC2. 
   * Finchè BSEC non ha stabilizzato il primo output, valid=false e tutti i campi a
   * zero; dopo un cold start ci vogliono ~5 min per il primo sample
   * (immediato invece quando lo stato è stato ripristinato da NVS).
   *
   * BSEC espone ogni grandezza come float perchè ha un solo campo signal, ma
   * qui i tipi seguono il dato: il decimale è significativo solo per la
   * temperatura, mentre umidità, pressione e IAQ stanno nel tipo intero più
   * piccolo che ne contiene il range.
   */
  struct Sample
  {
    bool     valid;
    float    temperature;   // °C (heat compensated)
    uint8_t  humidity;      // %RH (heat compensated), arrotondata
    uint16_t pressure;      // mBar (= hPa), arrotondati
    uint16_t iaq;           // 0-500
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

    /** Cadenza di salvataggio dello stato BSEC su NVS, da Timings.h:
     *  compromesso fra usura della flash e precisione post-reboot. */
    inline constexpr uint32_t STATE_SAVE_INTERVAL_MS = BSEC_STATE_SAVE_INTERVAL_MS;

    /** Testo dei codici di stato BSEC che compaiono in esercizio. I positivi
     *  sono warning e riguardano la temporizzazione delle misure: senza
     *  decodifica sarebbero numeri muti sul seriale. */
    inline const char* descriviStatoBsec(int stato)
    {
      switch (stato)
      {
        case 0:   return "ok";
        case 100: return "chiamata fuori tempo";
        case 101: return "misura extra rifiutata: misura ULP troppo vicina";
        case 102: return "misura extra rifiutata: troppo presto dalla precedente";
        default:  return stato > 0 ? "warning" : "errore";
      }
    }

    inline Bsec2    bsec;
    inline Sample   current          = {};
    inline bool     bsecOk           = false;
    inline bool     hasNewSample     = false;
    inline uint32_t lastStateSaveMs  = 0;

    /** Istante dell'ultima chiamata a refresh(): la cadenza delle
     *  interrogazioni si conta da qui e non dal campione, così un sensore che
     *  smette di rispondere non fa svegliare il dispositivo a ogni giro. */
    inline uint32_t lastPollMs       = 0;
    inline bool     everPolled       = false;

    /**
     * Sottrazione signed per confronto rollover-safe di millis(), robusta al
     * wrap a 32 bit (~49 giorni).
     */
    inline bool elapsed(uint32_t now, uint32_t last, uint32_t interval)
    {
      return (int32_t)(now - last) >= (int32_t)interval;
    }

    /**
     * Invalida il campione quando è più vecchio di INDOOR_SAMPLE_MAX_AGE_MS,
     * cioè al secondo campione perso: da lì il riquadro mostra "--" e Tuya
     * smette di pubblicare, invece di esporre un valore vecchio come attuale.
     * @return true se il campione è appena scaduto.
     */
    inline bool expireIfStale()
    {
      if (!current.valid || !elapsed(millis(), current.lastUpdateMs, INDOOR_SAMPLE_MAX_AGE_MS))
        return false;
      current.valid = false;
      LOG("BME680", "nessun campione da %lu s: dato scaduto",
          (unsigned long)((millis() - current.lastUpdateMs) / 1000UL));
      return true;
    }

    /**
     * Arrotonda un output BSEC al valore intero da memorizzare, scartando il
     * NaN: lroundf() su NaN ha comportamento indefinito e produrrebbe un
     * valore arbitrario dentro un campo che non può più segnalare l'errore.
     */
    inline uint32_t toRounded(float signal)
    {
      if (isnan(signal) || signal <= 0.0f) return 0;
      return (uint32_t)lroundf(signal);
    }

    /**
     * Callback invocato da BSEC quando è pronto un nuovo set di output.
     * Aggiorna la cache `current` e setta il flag `hasNewSample` che
     * Indoor::refresh() leggera' al ritorno da bsec.run().
     *
     * La firma a 3 parametri è imposta dalla libreria Bsec2 (typedef
     * bsecCallback). Due dei tre parametri non vengono usati nel corpo:
     *
     *  - rawData (bme68xData): campione "raw" letto dal BME68x prima della
     *    fusion BSEC (T/RH/P grezzi + resistenza VOC). Non lo leggiamo
     *    perchè il firmware mostra solo i valori heat-compensated calcolati
     *    da BSEC (signal degli OUTPUT_SENSOR_HEAT_COMPENSATED_*), già piu'
     *    accurati e gia' calibrati.
     *
     *  - bsec (Bsec2): copia/handle dell'istanza che ha generato l'evento.
     *    Non serve perchè la stessa istanza è disponibile come singleton
     *    `detail::bsec` definito a livello di namespace e gia' accessibile
     *    dalla callback (non gestiamo piu' di un BME680 sullo stesso ESP32).
     *
     * Lasciati senza nome "rawData" e  "bsec" per silenziare
     * il warning -Wunused-parameter senza alterare la firma attesa da Bsec2.
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
            current.iaq         = (uint16_t)toRounded(out.signal);
            current.iaqAccuracy = out.accuracy;
            break;
          case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
            current.temperature = out.signal;
            break;
          case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
            current.humidity = (uint8_t)toRounded(out.signal);
            break;
          case BSEC_OUTPUT_RAW_PRESSURE:
            // Pa -> mBar, numericamente identici agli hPa.
            current.pressure = (uint16_t)toRounded(out.signal / 100.0f);
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
     * l'accuratezza è almeno 1 (evita di persistere stati transitori).
     */
    inline void saveBsecState()
    {
      uint8_t buf[BSEC_MAX_STATE_BLOB_SIZE];
      if (!bsec.getState(buf))
      {
        LOG("BME680", "getState failed: %d", bsec.status);
        return;
      }

      Preferences prefs;
      if (!prefs.begin(NVS_NAMESPACE, false))
      {
        LOG("BME680", "NVS open RW failed");
        return;
      }
      size_t written = prefs.putBytes(NVS_KEY_STATE, buf, BSEC_MAX_STATE_BLOB_SIZE);
      prefs.end();

      if (written == BSEC_MAX_STATE_BLOB_SIZE)
        LOG("BME680", "state saved to NVS (%u bytes)", (unsigned)written);
      else
        LOG("BME680", "state partial save: %u/%u",
            (unsigned)written, (unsigned)BSEC_MAX_STATE_BLOB_SIZE);
    }

    /**
     * Ripristina lo stato BSEC da NVS se presente.
     * Chiamata da begin() prima di updateSubscription() per permettere a
     * BSEC di inizializzarsi gia' calibrato.
     */
    inline void restoreBsecState()
    {
      Preferences prefs;
      if (!prefs.begin(NVS_NAMESPACE, true))
      {
        LOG("BME680", "no saved state, cold start");
        return;
      }

      size_t len = prefs.getBytesLength(NVS_KEY_STATE);
      if (len != BSEC_MAX_STATE_BLOB_SIZE)
      {
        prefs.end();
        LOG("BME680", "no saved state, cold start");
        return;
      }

      uint8_t buf[BSEC_MAX_STATE_BLOB_SIZE];
      prefs.getBytes(NVS_KEY_STATE, buf, BSEC_MAX_STATE_BLOB_SIZE);
      prefs.end();

      if (bsec.setState(buf))
        LOG("BME680", "state restored (%u bytes)", (unsigned)BSEC_MAX_STATE_BLOB_SIZE);
      else
        LOG("BME680", "setState failed: %d", bsec.status);
    }

#if I2C_DEBUG
    /** Letture ripetute per ogni clock dello sweep di findMaxClock(). */
    inline constexpr uint8_t I2C_SWEEP_READS = 16;

    /**
     * Legge n registri consecutivi con la stessa forma di transazione di
     * bme68xI2cRead() (puntatore con STOP, poi requestFrom), così la misura
     * esercita il traffico che BSEC userà. Al clock corrente di Wire.
     */
    inline bool readRegs(uint8_t addr, uint8_t reg, uint8_t* buf, uint8_t n)
    {
      Wire.beginTransmission(addr);
      Wire.write(reg);
      if (Wire.endTransmission() != 0) return false;
      if (Wire.requestFrom(addr, n) != n) return false;
      for (uint8_t i = 0; i < n; i++) buf[i] = (uint8_t)Wire.read();
      return true;
    }

    /**
     * Clock massimo a cui il blocco di calibrazione (NVM costante, bit misti)
     * si rilegge identico al riferimento preso a 100 kHz. Sale per scalini e
     * si ferma al primo errore o differenza, quindi paga al più un timeout.
     * @return la frequenza in Hz, 0 se il bus è instabile già a 100 kHz.
     */
    inline uint32_t findMaxClock(uint8_t addr)
    {
      static constexpr uint32_t SCALINI[] = {100000UL, 200000UL, 400000UL,
                                             600000UL, 800000UL, 1000000UL};
      uint8_t ref[BME68X_LEN_COEFF1], buf[BME68X_LEN_COEFF1];

      Wire.setClock(SCALINI[0]);
      if (!readRegs(addr, BME68X_REG_COEFF1, ref, sizeof(ref)) ||
          !readRegs(addr, BME68X_REG_COEFF1, buf, sizeof(buf)) ||
          memcmp(ref, buf, sizeof(ref)) != 0)
        return 0;

      uint32_t max = 0;
      for (uint32_t f : SCALINI)
      {
        Wire.setClock(f);
        for (uint8_t i = 0; i < I2C_SWEEP_READS; i++)
        {
          if (!readRegs(addr, BME68X_REG_COEFF1, buf, sizeof(buf)) ||
              memcmp(ref, buf, sizeof(ref)) != 0)
          {
            LOG("BME680", "i2c: errore a %lu kHz", (unsigned long)(f / 1000UL));
            return max;
          }
        }
        max = f;
      }
      return max;
    }

    /**
     * Rileva il sensore sul bus e misura il clock: sonda i due indirizzi
     * ammessi dal BME68x (la sonda di Wire gira sempre a 100 kHz, quale che
     * sia il clock impostato), controlla il chip ID e lascia Wire al clock da
     * usare, cioè BME680_I2C_CLOCK_HZ se il bus lo regge, altrimenti il
     * massimo misurato. Ogni esito negativo è loggato con la sua causa.
     * Usata da begin() con I2C_DEBUG a 1.
     * @return false se non c'è un BME680 utilizzabile.
     */
    inline bool debugDetect(uint8_t& addr)
    {
      const uint8_t candidati[] = {
        BME680_I2C_ADDR,
        BME680_I2C_ADDR == BME68X_I2C_ADDR_HIGH ? BME68X_I2C_ADDR_LOW : BME68X_I2C_ADDR_HIGH};

      uint8_t esito = 2;
      addr = 0;
      for (uint8_t a : candidati)
      {
        Wire.beginTransmission(a);
        esito = Wire.endTransmission();
        if (esito == 0) { addr = a; break; }
        // Timeout: bus guasto, ogni altra sonda costerebbe un altro timeout
        if (esito == 5) break;
      }

      if (addr == 0)
      {
        if (esito == 5)
          LOG("BME680", "i2c: timeout sul bus, pull-up assenti o linea bloccata bassa");
        else
          LOG("BME680", "i2c: nessuna risposta a 0x%02X e 0x%02X, controllare "
              "alimentazione 3,3 V, SDA/SCL, CS a 3,3 V e SDO non flottante",
              (unsigned)candidati[0], (unsigned)candidati[1]);
        return false;
      }

      uint8_t chipId = 0;
      Wire.setClock(100000UL);
      if (!readRegs(addr, BME68X_REG_CHIP_ID, &chipId, 1))
      {
        LOG("BME680", "i2c: ACK a 0x%02X ma lettura del chip ID fallita", (unsigned)addr);
        return false;
      }
      if (chipId != BME68X_CHIP_ID)
      {
        LOG("BME680", "i2c: a 0x%02X chip ID 0x%02X, non un BME680 "
            "(0x60 = BME280, 0x58 = BMP280)", (unsigned)addr, (unsigned)chipId);
        return false;
      }

      const uint32_t max = findMaxClock(addr);
      if (max == 0)
      {
        LOG("BME680", "i2c: letture instabili già a 100 kHz");
        return false;
      }
      LOG("BME680", "i2c: BME680 a 0x%02X, clock massimo affidabile %lu kHz%s",
          (unsigned)addr, (unsigned long)(max / 1000UL),
          max == 1000000UL ? " (tetto di Wire)" : "");

      const uint32_t clockHz = BME680_I2C_CLOCK_HZ <= max ? BME680_I2C_CLOCK_HZ : max;
      Wire.setClock(clockHz);
      if (addr != BME680_I2C_ADDR || clockHz != BME680_I2C_CLOCK_HZ)
        LOG("BME680", "i2c: in uso 0x%02X a %lu kHz, aggiornare "
            "BME680_I2C_ADDR / BME680_I2C_CLOCK_HZ",
            (unsigned)addr, (unsigned long)(clockHz / 1000UL));
      return true;
    }
#endif
  } // namespace detail

  /**
   * Inizializza Wire, avvia BSEC2 sul BME680, ripristina l'eventuale
   * stato salvato in NVS e sottoscrive gli output in modalita' ULP
   * (un sample ogni 300 s, coerente col light sleep del ciclo main).
   * Con I2C_DEBUG a 1 indirizzo e clock vengono da debugDetect(), altrimenti
   * da BME680_I2C_ADDR e BME680_I2C_CLOCK_HZ.
   * Se il sensore non risponde il resto del firmware (meteo/calendario/display) continua a funzionare.
   */
  inline void begin()
  {
    using namespace detail;
    current         = {};
    bsecOk          = false;
    hasNewSample    = false;
    lastStateSaveMs = 0;
    lastPollMs      = 0;
    everPolled      = false;

    Wire.begin(BME680_SDA_PIN, BME680_SCL_PIN);
    // Bsec2 e Bme68x usano l'oggetto Wire senza reinizializzarlo, quindi il
    // clock impostato qui resta quello del bus.
    Wire.setClock(BME680_I2C_CLOCK_HZ);

#if I2C_DEBUG
    uint8_t addr;
    if (!debugDetect(addr)) return;
#else
    const uint8_t addr = BME680_I2C_ADDR;
#endif

    if (!bsec.begin(addr, Wire)) {
      LOG("BME680", "init failed (bsec=%d, sensor=%d)",
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

    if (!bsec.updateSubscription(sensorList, sensorCount, BSEC_SAMPLE_RATE_ULP)) {
      LOG("BME680", "updateSubscription failed: %d", bsec.status);
      return;
    }

    bsec.attachCallback(onBsecData);

    bsecOk          = true;
    lastStateSaveMs = millis();
    LOG("BME680", "init ok, ULP 5 min");
  }

  /** Vero quando il sensore e' stato inizializzato con successo. È l'hook
   *  pronto() del task del sensore nel .ino: senza sensore il task è sospeso. */
  inline bool attivo() { return detail::bsecOk; }

  /**
   * Millisecondi alla prossima interrogazione utile, 0 se il sensore non è
   * ancora stato interrogato. Bsec2::run() lavora solo quando arriva il suo
   * next_call, ogni 300 s in ULP, e prima non fa nulla: interrogare più spesso
   * non anticipa nessun campione, quindi la cadenza si conta dall'ultima
   * chiamata, che il campione arrivi o no.
   * Usato dal .ino come scadenza custom del task del sensore.
   */
  inline uint32_t msUntilNextPoll()
  {
    using namespace detail;
    if (!everPolled) return 0;
    const uint32_t due = lastPollMs + (uint32_t)BSEC_PERIODO_ULP_S * 1000UL + BSEC_MARGINE_POLL_MS;
    const int32_t d = (int32_t)(due - millis());
    return d <= 0 ? 0 : (uint32_t)d;
  }

  /**
   * Interroga il sensore. Lo scheduler la chiama alla scadenza data da
   * msUntilNextPoll().
   * @return true quando il dato da mostrare è cambiato: un nuovo campione,
   *         oppure il campione precedente appena scaduto. Il chiamante usa il
   *         valore per chiedere il ridisegno.
   */
  inline bool refresh()
  {
    using namespace detail;
    if (!bsecOk) return false;

    lastPollMs = millis();
    everPolled = true;
    hasNewSample = false;
    if (!bsec.run())
    {
      /**
       * run() ritorna false anche quando semplicemente non ci sono dati:
       * non e' un errore fatale. Il confronto e' con != e non con <: i codici
       * positivi sono warning, fra cui la violazione di temporizzazione
       * (100), e con un filtro sui soli negativi resterebbero invisibili.
       */
      if (bsec.status != BSEC_OK || bsec.sensor.status != BME68X_OK)
        LOG("BME680", "run: bsec=%d (%s), sensor=%d",
            bsec.status, detail::descriviStatoBsec(bsec.status), bsec.sensor.status);
      return expireIfStale();
    }

    if (!hasNewSample) return expireIfStale();

    LOGV("BME680", "T=%.1fC RH=%u%% P=%umBar IAQ=%u acc=%u",
         current.temperature, (unsigned)current.humidity,
         (unsigned)current.pressure, (unsigned)current.iaq,
         (unsigned)current.iaqAccuracy);

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
   * Ultimo campione (riferimento a cache interna); valid è falso finchè non
   * ne arriva uno e dopo che è scaduto.
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
   * Usato da: Weather.h (renderIndoorBox) per mostrare la
   * condizione accanto al valore numerico IAQ.
   */
  inline const char* iaqLabel(uint16_t iaq)
  {
    if (iaq <=  50) return "Ottima";
    if (iaq <= 100) return "Buona";
    if (iaq <= 150) return "Discreta";
    if (iaq <= 200) return "Mediocre";
    if (iaq <= 250) return "Cattiva";
    if (iaq <= 350) return "Pessima";
    return "Nociva";
  }
}

#endif

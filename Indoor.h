#ifndef INDOOR_H
#define INDOOR_H

/**
 * Sensore ambientale montato: va definito uno e uno solo dei due.
 *
 *   INDOOR_SENSOR_BME680  Bosch BME680 con la libreria BSEC2 in ULP: T e RH
 *                         heat-compensated, pressione e IAQ con accuratezza.
 *   INDOOR_SENSOR_BME280  Bosch BME280 con la libreria Adafruit in forced
 *                         mode: T, RH e pressione, nessun IAQ.
 *
 * Il ramo non scelto non entra nella build: arduino-cli scopre le librerie
 * dagli #include che vede il preprocessore, quindi col BME280 la libreria
 * BSEC non viene nè compilata nè collegata.
 */
#define INDOOR_SENSOR_BME280
// #define INDOOR_SENSOR_BME680

#if defined(INDOOR_SENSOR_BME680) == defined(INDOOR_SENSOR_BME280)
#error "Indoor.h: definire uno e uno solo fra INDOOR_SENSOR_BME680 e INDOOR_SENSOR_BME280"
#endif

#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>
#include "Timings.h"
#include "Log.h"

#ifdef INDOOR_SENSOR_BME680
#include <bsec2.h>
#include <Preferences.h>
#define INDOOR_TAG "BME680"
#else
#include <Adafruit_BME280.h>
#define INDOOR_TAG "BME280"
#endif

/**
 * Modulo header-only del sensore ambientale interno (I2C), BME680 o BME280
 * secondo il define qui sopra.
 *
 * BME680: libreria Bosch BSEC2 in modalita' ULP (sample period = 300 s).
 * BSEC fornisce T e RH heat-compensated, pressione raw e IAQ calibrato (0-500) con livello di accuratezza 0..3.
 * Lo stato del calibratore viene salvato su NVS (namespace "bme680") ogni 6 ore
 * e ripristinato all'avvio, azzerando il warm-up dopo un power cycle.
 * La scadenza di BSEC è l'orologio del ciclo: lo scheduler chiama poll() alla
 * scadenza di msUntilNextPoll(), anche di notte.
 *
 * BME280: libreria Adafruit in forced mode, oversampling x1 e filtro spento,
 * il profilo "weather monitoring" del datasheet. Il sensore non ha una
 * scadenza propria: lo scheduler tiene il ciclo interno e chiama poll()
 * all'inizio di ogni ciclo dentro la fascia WiFi, mai di notte.
 *
 * In entrambi i casi poll() è il primo passo del ciclo, e il frame legge poi
 * da sè il campione, solo dentro la fascia WiFi.
 */
#define INDOOR_I2C_ADDR   0x77
#define INDOOR_SDA_PIN    21
#define INDOOR_SCL_PIN    16

/** Clock dell'I2C. Il limite reale non è quello dei datasheet ma quello del
 *  cablaggio (pull-up e capacità dei fili): il valore va preso dal clock
 *  massimo affidabile che I2C_DEBUG misura sul bus, uno scalino sotto se il
 *  margine è stretto. A 400 kHz la lettura di un campione scende da circa 3 ms
 *  a meno di 1 ms. Wire non accetta valori oltre 1 MHz. */
#define INDOOR_I2C_CLOCK_HZ 400000UL

/**
 * Diagnosi del bus I2C al boot, da accendere durante il bring-up del sensore.
 * A 1 begin() sonda i due soli indirizzi possibili del sensore (0x76 e 0x77,
 * gli stessi per BME680 e BME280: quello configurato e l'altro), verifica il
 * chip ID e misura il clock massimo a cui il blocco di calibrazione si rilegge
 * identico, poi avvia il sensore su indirizzo e clock trovati, loggando quali
 * costanti correggere. Costa ~100 ms una volta sola; se il sensore non c'è il
 * modulo resta un no-op come senza flag.
 * A 0 begin() usa INDOOR_I2C_ADDR e INDOOR_I2C_CLOCK_HZ così come sono: va
 * riportato a 0 dopo aver fissato le due costanti.
 */
#ifndef I2C_DEBUG
#define I2C_DEBUG 1
#endif

namespace Indoor
{
  /**
   * Ultimo campione del sensore.
   * Finchè non è arrivato il primo, valid=false e tutti i campi a zero. Col
   * BME680 il ripristino dello stato da NVS non anticipa il primo campione:
   * evita il warm-up dell'accuratezza IAQ.
   *
   * BSEC espone ogni grandezza come float perchè ha un solo campo signal, ma
   * qui i tipi seguono il dato: il decimale è significativo solo per la
   * temperatura, mentre umidità, pressione e IAQ stanno nel tipo intero più
   * piccolo che ne contiene il range. I campi IAQ esistono solo col BME680:
   * un uso non protetto da #ifdef INDOOR_SENSOR_BME680 non compila.
   */
  struct Sample
  {
    bool     valid;
    float    temperature;   // °C (col BME680 heat compensated)
    uint8_t  humidity;      // %RH (col BME680 heat compensated), arrotondata
    uint16_t pressure;      // mBar (= hPa), arrotondati
#ifdef INDOOR_SENSOR_BME680
    uint16_t iaq;           // 0-500
    uint8_t  iaqAccuracy;   // 0 (stabilizing) -> 3 (calibrated)
#endif
    uint32_t lastUpdateMs;  // millis() del sample corrente
  };

  // -------------------------------------------------------------------------
  // Stato e helper interni. Raggruppati in `detail` per non inquinare il
  // namespace pubblico del modulo. Gli `inline` (variabili + funzioni)
  // permettono la definizione nell'header senza violare la ODR (C++17).
  // -------------------------------------------------------------------------
  namespace detail
  {
    inline Sample   current          = {};
    inline bool     sensorOk         = false;

#ifdef INDOOR_SENSOR_BME680
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
    inline uint32_t lastStateSaveMs  = 0;

    /** Vero dopo il primo salvataggio dello stato ad accuratezza IAQ 3 in
     *  questo boot: evita di riscrivere la flash a ogni campione calibrato. */
    inline bool     savedAtFullAccuracy = false;

    /**
     * Configurazione BSEC per BME680 alimentato a 3,3 V, campionamento ULP da
     * 300 s e storia di calibrazione di 4 giorni (bme680_iaq_33v_300s_4d della
     * libreria): la guida Bosch AN011 chiede il file giusto per uscite ULP
     * accurate. Sta in flash a livello di namespace, non sullo stack di begin(),
     * perchè il blob occupa BSEC_MAX_PROPERTY_BLOB_SIZE byte.
     */
    inline const uint8_t BSEC_CONFIG_IAQ[] = {
#include "config/bme680/bme680_iaq_33v_300s_4d/bsec_iaq.txt"
    };

    /** Timestamp BSEC (ns) delle ultime uscite applicate: le uscite restano
     *  nella libreria fra un run() e l'altro, e solo un timestamp diverso
     *  indica un campione nuovo. */
    inline int64_t  lastOutputTimestampNs = 0;

    /** Istante dell'ultima chiamata a poll(): la cadenza delle
     *  interrogazioni si conta da qui e non dal campione, così un sensore che
     *  smette di rispondere non fa svegliare il dispositivo a ogni giro. */
    inline uint32_t lastPollMs       = 0;
    inline bool     everPolled       = false;

    /** Chip ID e blocco di calibrazione del BME680, per la diagnostica I2C. */
    inline constexpr uint8_t CHIP_ID   = BME68X_CHIP_ID;
    inline constexpr uint8_t CALIB_REG = BME68X_REG_COEFF1;
    inline constexpr uint8_t CALIB_LEN = BME68X_LEN_COEFF1;
#else
    inline Adafruit_BME280 bme;

    /** Chip ID e blocco di calibrazione del BME280, per la diagnostica I2C.
     *  La libreria confronta il chip ID col letterale 0x60; il blocco sono i
     *  trim dig_T1..dig_P9, costanti per costruzione. */
    inline constexpr uint8_t CHIP_ID   = 0x60;
    inline constexpr uint8_t CALIB_REG = BME280_REGISTER_DIG_T1;
    inline constexpr uint8_t CALIB_LEN = 24;

    /** Indirizzo I2C su cui il sensore è stato avviato, per leggere ctrl_hum. */
    inline uint8_t bmeAddress = INDOOR_I2C_ADDR;

    /** Registro ctrl_hum del BME280: i bit 2..0 sono il sovracampionamento
     *  dell'umidità, che un reset del chip riporta a zero (umidità saltata). */
    inline constexpr uint8_t REG_CTRL_HUM       = BME280_REGISTER_CONTROLHUMID;   // 0xF2
    inline constexpr uint8_t CTRL_HUM_OSRS_MASK = 0x07;

    /** Sovracampionamento dell'umidità impostato da applySampling(). */
    inline constexpr uint8_t HUM_OSRS_X1 = (uint8_t)Adafruit_BME280::SAMPLING_X1;

    /**
     * Forced mode con oversampling x1 su temperatura, pressione e umidità e
     * filtro spento. Il sensore misura solo quando poll() lo chiede e poi torna
     * in sleep: il default della libreria è normal mode x16, che misura di
     * continuo e scalda il sensore. La chiamano begin() e poll(), quando
     * scopre che il chip si è resettato.
     */
    inline void applySampling()
    {
      bme.setSampling(Adafruit_BME280::MODE_FORCED,
                      Adafruit_BME280::SAMPLING_X1,    // temperatura
                      Adafruit_BME280::SAMPLING_X1,    // pressione
                      Adafruit_BME280::SAMPLING_X1,    // umidità
                      Adafruit_BME280::FILTER_OFF);
    }

    /**
     * Legge ctrl_hum dal chip via Wire. La libreria Adafruit non lo rilegge mai
     * e tiene il valore impostato in una copia software, quindi un reset del
     * chip passa inosservato.
     * @return false se la lettura I2C non riesce.
     */
    inline bool readCtrlHum(uint8_t& value)
    {
      Wire.beginTransmission(bmeAddress);
      Wire.write(REG_CTRL_HUM);
      if (Wire.endTransmission() != 0) return false;
      if (Wire.requestFrom(bmeAddress, (uint8_t)1) != 1) return false;
      value = (uint8_t)Wire.read();
      return true;
    }
#endif

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
     */
    inline void expireIfStale()
    {
      if (!current.valid || !elapsed(millis(), current.lastUpdateMs, INDOOR_SAMPLE_MAX_AGE_MS))
        return;
      current.valid = false;
      LOG(INDOOR_TAG, "nessun campione da %lu s: dato scaduto",
          (unsigned long)((millis() - current.lastUpdateMs) / 1000UL));
    }

    /**
     * Arrotonda una grandezza del sensore al valore intero da memorizzare,
     * scartando il NaN: lroundf() su NaN ha comportamento indefinito e
     * produrrebbe un valore arbitrario dentro un campo che non può più
     * segnalare l'errore.
     */
    inline uint32_t toRounded(float signal)
    {
      if (isnan(signal) || signal <= 0.0f) return 0;
      return (uint32_t)lroundf(signal);
    }

#ifdef INDOOR_SENSOR_BME680
    /**
     * Copia nella cache `current` le uscite di un campione BSEC appena
     * elaborato. Le uscite si leggono per riferimento con getOutputs() dopo
     * bsec.run(), invece che dalla callback della libreria, che riceve per
     * valore sia le uscite sia l'intero Bsec2.
     */
    inline void applyOutputs(const bsecOutputs& outputs)
    {
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
#endif

#if I2C_DEBUG
    /** Letture ripetute per ogni clock dello sweep di findMaxClock(). */
    inline constexpr uint8_t I2C_SWEEP_READS = 16;

    /** Registro del chip ID, lo stesso su BME680 e BME280. */
    inline constexpr uint8_t REG_CHIP_ID = 0xD0;

    /**
     * Legge n registri consecutivi con la stessa forma di transazione di
     * bme68xI2cRead() (puntatore con STOP, poi requestFrom), così la misura
     * esercita il traffico che BSEC userà; la libreria Adafruit usa lo start
     * ripetuto al posto dello STOP, che per il clock massimo non conta.
     * Al clock corrente di Wire.
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
      uint8_t ref[CALIB_LEN], buf[CALIB_LEN];

      Wire.setClock(SCALINI[0]);
      if (!readRegs(addr, CALIB_REG, ref, sizeof(ref)) ||
          !readRegs(addr, CALIB_REG, buf, sizeof(buf)) ||
          memcmp(ref, buf, sizeof(ref)) != 0)
        return 0;

      uint32_t max = 0;
      for (uint32_t f : SCALINI)
      {
        Wire.setClock(f);
        for (uint8_t i = 0; i < I2C_SWEEP_READS; i++)
        {
          if (!readRegs(addr, CALIB_REG, buf, sizeof(buf)) ||
              memcmp(ref, buf, sizeof(ref)) != 0)
          {
            LOG(INDOOR_TAG, "i2c: errore a %lu kHz", (unsigned long)(f / 1000UL));
            return max;
          }
        }
        max = f;
      }
      return max;
    }

    /**
     * Rileva il sensore sul bus e misura il clock: sonda i due indirizzi
     * ammessi da BME680 e BME280 (la sonda di Wire gira sempre a 100 kHz,
     * quale che sia il clock impostato), controlla il chip ID e lascia Wire al
     * clock da usare, cioè INDOOR_I2C_CLOCK_HZ se il bus lo regge, altrimenti
     * il massimo misurato. Ogni esito negativo è loggato con la sua causa.
     * Prima della sonda logga i pin in uso e il livello di SDA e SCL a bus
     * fermo: entrambi devono leggersi alti, una linea a 0 è quella bloccata.
     * Il livello è digitale, quindi una linea a metà tensione può leggersi in
     * un modo o nell'altro: in quel caso decide il tester.
     * Usata da begin() con I2C_DEBUG a 1.
     * @return false se non c'è un sensore utilizzabile.
     */
    inline bool debugDetect(uint8_t& addr)
    {
      LOG(INDOOR_TAG, "i2c: SDA GPIO%d, SCL GPIO%d, livelli a riposo SDA=%d SCL=%d (atteso 1 e 1)",
          INDOOR_SDA_PIN, INDOOR_SCL_PIN,
          gpio_get_level((gpio_num_t)INDOOR_SDA_PIN),
          gpio_get_level((gpio_num_t)INDOOR_SCL_PIN));

      const uint8_t candidati[] = {
        INDOOR_I2C_ADDR,
        INDOOR_I2C_ADDR == 0x77 ? 0x76 : 0x77};

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
          LOG(INDOOR_TAG, "i2c: timeout sul bus, pull-up assenti o linea bloccata bassa");
        else
          LOG(INDOOR_TAG, "i2c: nessuna risposta a 0x%02X e 0x%02X, controllare "
              "alimentazione 3,3 V, SDA/SCL, CS a 3,3 V e SDO non flottante",
              (unsigned)candidati[0], (unsigned)candidati[1]);
        return false;
      }

      uint8_t chipId = 0;
      Wire.setClock(100000UL);
      if (!readRegs(addr, REG_CHIP_ID, &chipId, 1))
      {
        LOG(INDOOR_TAG, "i2c: ACK a 0x%02X ma lettura del chip ID fallita", (unsigned)addr);
        return false;
      }
      if (chipId != CHIP_ID)
      {
        LOG(INDOOR_TAG, "i2c: a 0x%02X chip ID 0x%02X, atteso 0x%02X "
            "(0x61 = BME680, 0x60 = BME280, 0x58 = BMP280)",
            (unsigned)addr, (unsigned)chipId, (unsigned)CHIP_ID);
        return false;
      }

      const uint32_t max = findMaxClock(addr);
      if (max == 0)
      {
        LOG(INDOOR_TAG, "i2c: letture instabili già a 100 kHz");
        return false;
      }
      LOG(INDOOR_TAG, "i2c: " INDOOR_TAG " a 0x%02X, clock massimo affidabile %lu kHz%s",
          (unsigned)addr, (unsigned long)(max / 1000UL),
          max == 1000000UL ? " (tetto di Wire)" : "");

      const uint32_t clockHz = INDOOR_I2C_CLOCK_HZ <= max ? INDOOR_I2C_CLOCK_HZ : max;
      Wire.setClock(clockHz);
      if (addr != INDOOR_I2C_ADDR || clockHz != INDOOR_I2C_CLOCK_HZ)
        LOG(INDOOR_TAG, "i2c: in uso 0x%02X a %lu kHz, aggiornare "
            "INDOOR_I2C_ADDR / INDOOR_I2C_CLOCK_HZ",
            (unsigned)addr, (unsigned long)(clockHz / 1000UL));
      return true;
    }
#endif
  } // namespace detail

  /**
   * Inizializza Wire e avvia il sensore.
   * BME680: avvia BSEC2, ripristina l'eventuale stato salvato in NVS e
   * sottoscrive gli output in modalita' ULP (un sample ogni 300 s, coerente
   * col light sleep del ciclo main).
   * BME280: avvia la libreria Adafruit e la porta in forced mode.
   * Con I2C_DEBUG a 1 indirizzo e clock vengono da debugDetect(), altrimenti
   * da INDOOR_I2C_ADDR e INDOOR_I2C_CLOCK_HZ.
   * Se il sensore non risponde il resto del firmware (meteo/calendario/display) continua a funzionare.
   */
  inline void begin()
  {
    using namespace detail;
    current  = {};
    sensorOk = false;
#ifdef INDOOR_SENSOR_BME680
    lastStateSaveMs = 0;
    savedAtFullAccuracy = false;
    lastPollMs      = 0;
    everPolled      = false;
    lastOutputTimestampNs = 0;
#endif

    Wire.begin(INDOOR_SDA_PIN, INDOOR_SCL_PIN);
    // Le librerie dei sensori usano l'oggetto Wire senza reinizializzarlo:
    // Adafruit richiama Wire.begin(), che a bus già avviato non tocca nè pin
    // nè clock. Il clock impostato qui resta quindi quello del bus.
    Wire.setClock(INDOOR_I2C_CLOCK_HZ);

#if I2C_DEBUG
    uint8_t addr;
    if (!debugDetect(addr)) return;
#else
    const uint8_t addr = INDOOR_I2C_ADDR;
#endif

#ifdef INDOOR_SENSOR_BME680
    if (!bsec.begin(addr, Wire)) {
      LOG("BME680", "init failed (bsec=%d, sensor=%d)",
          bsec.status, bsec.sensor.status);
      return;
    }

    // La configurazione va prima dello stato, come nell'esempio Bosch
    // basic_config_state: lo stato si interpreta rispetto alla configurazione
    // caricata. Se fallisce BSEC resta sulla configurazione di default.
    if (!bsec.setConfig(BSEC_CONFIG_IAQ))
      LOG("BME680", "setConfig failed: %d (%s)", bsec.status, detail::descriviStatoBsec(bsec.status));

    // Prima di sottoscrivere gli output: se esiste uno stato BSEC salvato
    // in NVS lo ricarichiamo, saltando così il warm-up IAQ. Uno stato salvato
    // con un'altra configurazione viene rifiutato e la taratura riparte.
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

    lastStateSaveMs = millis();
    LOG("BME680", "init ok, ULP 5 min");
#else
    bmeAddress = addr;
    if (!bme.begin(addr, &Wire))
    {
      LOG("BME280", "init failed a 0x%02X: nessuna risposta o chip ID diverso da 0x60", (unsigned)addr);
      return;
    }

    applySampling();
    LOG("BME280", "init ok, forced mode");
#endif
    sensorOk = true;
  }

  /** Vero quando il sensore è stato inizializzato con successo. È l'hook
   *  sensorActive dello scheduler: senza sensore poll() non viene chiamata e il
   *  ciclo diventa interno. Col BME280 il ciclo è interno comunque, perchè il
   *  sensore non registra msUntilSensorPoll. */
  inline bool attivo() { return detail::sensorOk; }

#ifdef INDOOR_SENSOR_BME680
  /**
   * Millisecondi alla prossima interrogazione utile, 0 se il sensore non è
   * ancora stato interrogato. Bsec2::run() lavora solo quando arriva il suo
   * next_call, ogni 300 s in ULP, e prima non fa nulla: interrogare più spesso
   * non anticipa nessun campione, quindi la cadenza si conta dall'ultima
   * chiamata, che il campione arrivi o no.
   * È l'hook msUntilSensorPoll dello scheduler, cioè la scadenza del ciclo.
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
   * Interroga il sensore. Lo scheduler la chiama alla scadenza di
   * msUntilNextPoll() e solo con attivo() vero, perchè con il sensore non
   * inizializzato bsec.run() girerebbe su un'istanza BSEC mai creata.
   */
  inline void poll()
  {
    using namespace detail;

    lastPollMs = millis();
    everPolled = true;
    if (!bsec.run())
    {
      /**
       * run() ritorna false per un errore o un warning di BSEC o del sensore;
       * una chiamata prima di next_call ritorna true senza produrre uscite. Il
       * confronto è con != e non con <: i codici positivi sono warning, fra cui
       * la violazione di temporizzazione (100), e con un filtro sui soli
       * negativi resterebbero invisibili.
       */
      if (bsec.status != BSEC_OK || bsec.sensor.status != BME68X_OK)
        LOG("BME680", "run: bsec=%d (%s), sensor=%d",
            bsec.status, detail::descriviStatoBsec(bsec.status), bsec.sensor.status);
      expireIfStale();
      return;
    }

    // Le uscite restano nella libreria fra un run() e l'altro: è nuovo solo un
    // campione con un timestamp diverso dall'ultimo applicato.
    const bsecOutputs* outputs = bsec.getOutputs();
    if (outputs == nullptr || outputs->output[0].time_stamp == lastOutputTimestampNs)
    {
      expireIfStale();
      return;
    }
    lastOutputTimestampNs = outputs->output[0].time_stamp;
    applyOutputs(*outputs);

    LOGV("BME680", "T=%.1fC RH=%u%% P=%umBar IAQ=%u acc=%u",
         current.temperature, (unsigned)current.humidity,
         (unsigned)current.pressure, (unsigned)current.iaq,
         (unsigned)current.iaqAccuracy);

    // Il primo campione ad accuratezza 3 del boot salva subito lo stato, così
    // un riavvio dopo la taratura non la perde; poi ogni 6 h, solo con
    // accuratezza almeno 1, per non sovrascrivere uno stato buono con uno
    // transitorio post-reboot.
    const bool firstFullAccuracy = current.iaqAccuracy >= 3 && !savedAtFullAccuracy;
    if (firstFullAccuracy ||
        (current.iaqAccuracy >= 1 && elapsed(millis(), lastStateSaveMs, STATE_SAVE_INTERVAL_MS)))
    {
      saveBsecState();
      lastStateSaveMs = millis();
      if (current.iaqAccuracy >= 3) savedAtFullAccuracy = true;
    }
  }
#else
  /**
   * Interroga il sensore: una misura forzata (~10 ms con oversampling x1) e la
   * lettura di temperatura, pressione e umidità. Lo scheduler la chiama
   * all'inizio di ogni ciclo dentro la fascia WiFi e solo con attivo() vero,
   * perchè con il sensore non inizializzato la libreria userebbe un
   * dispositivo I2C mai creato. In forced mode ogni 5 minuti il sensore non
   * si scalda da sè, quindi la temperatura non va compensata.
   *
   * Prima di ogni misura rilegge ctrl_hum: se il sensore si è resettato da sè
   * ha l'umidità saltata, la libreria non se ne accorge (tiene la propria copia
   * software della configurazione) e ne esce un valore plausibile ma falso. In
   * quel caso riapplica il campionamento e scarta la lettura.
   */
  inline void poll()
  {
    using namespace detail;

    uint8_t ctrlHum = 0;
    if (!readCtrlHum(ctrlHum))
    {
      LOG("BME280", "lettura di ctrl_hum fallita: lettura scartata");
      expireIfStale();
      return;
    }
    if ((ctrlHum & CTRL_HUM_OSRS_MASK) != HUM_OSRS_X1)
    {
      LOG("BME280", "ctrl_hum 0x%02X: il sensore si è resettato, riapplico il campionamento e scarto la lettura",
          (unsigned)ctrlHum);
      applySampling();
      expireIfStale();
      return;
    }

    // Su un errore I2C la libreria non segnala nulla: read8/read24 restituiscono
    // il buffer non riempito, con in testa l'indirizzo del registro, e la
    // temperatura che ne esce supera di molto gli 85 °C. Il range operativo
    // del datasheet scarta la lettura invece di mostrare un valore inventato.
    // La pressione si scrive come negazione dell'intervallo valido, così anche
    // un NaN viene scartato: ogni confronto con NaN è falso.
    const float t = bme.takeForcedMeasurement() ? bme.readTemperature() : NAN;
    const float p = bme.readPressure() / 100.0f;   // Pa -> mBar (= hPa)
    if (isnan(t) || t < -40.0f || t > 85.0f || !(p >= 300.0f && p <= 1100.0f))
    {
      LOG("BME280", "lettura fallita: T=%.1f P=%.1f", t, p);
      expireIfStale();
      return;
    }

    current.temperature  = t;
    current.humidity     = (uint8_t)toRounded(bme.readHumidity());   // la libreria la limita a 0..100
    current.pressure     = (uint16_t)toRounded(p);
    current.valid        = true;
    current.lastUpdateMs = millis();

    LOGV("BME280", "T=%.1fC RH=%u%% P=%umBar",
         current.temperature, (unsigned)current.humidity, (unsigned)current.pressure);
  }
#endif

  /**
   * Ultimo campione (riferimento a cache interna); valid è falso finchè non
   * ne arriva uno e dopo che è scaduto.
   */
  inline const Sample& sample()
  {
    return detail::current;
  }

  /**
   * Valori del riquadro Interno come si mostrano sul display. Il campione resta
   * alla risoluzione dei suoi tipi: queste tre funzioni applicano solo il crop di
   * visualizzazione, così le oscillazioni sotto la risoluzione utile non cambiano
   * il frame e il driver può saltare il refresh. L'umidità si mostra com'è.
   * Le replica tools/layout_preview.py per l'anteprima.
   */

  /** Temperatura arrotondata a 0,5 °C. Fra -0,25 e 0 °C roundf() dà -0.0, che
   *  "%.1f" stampa come "-0.0": lo zero esce sempre positivo. */
  inline float shownTemperature(float celsius)
  {
    const float shown = roundf(celsius * 2.0f) / 2.0f;
    return shown == 0.0f ? 0.0f : shown;
  }

  /** Pressione in mBar arrotondata a 2 mBar. */
  inline uint16_t shownPressure(uint16_t mbar)
  {
    return (uint16_t)(((uint32_t)mbar + 1) / 2 * 2);
  }

  /** Indice IAQ arrotondato a 10 punti. Le soglie dell'etichetta sono multiple di 10, quindi l'etichetta si calcola sul valore arrotondato. */
  inline uint16_t shownIaq(uint16_t iaq)
  {
    return (uint16_t)(((uint32_t)iaq + 5) / 10 * 10);
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
   * Usato da: Weather.h (renderIndoorBox), solo col BME680, per mostrare la
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

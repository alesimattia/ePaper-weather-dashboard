#ifndef PARTIAL_REFRESH_H
#define PARTIAL_REFRESH_H

#include <stdint.h>
#include <type_traits>
#include <utility>

#include "Timings.h"

/**
 * Interruttore generale del refresh differenziale, definito nel .ino prima degli
 * include dei moduli. 0 = ogni frame è un refresh pieno, come prima; 1 = un frame
 * è partial quando il piano rosso non è cambiato. Questo #ifndef tiene l'header
 * compilabile da solo.
 */
#ifndef DISPLAY_PARTIAL_REFRESH
  #define DISPLAY_PARTIAL_REFRESH 0
#endif

/**
 * Politica del refresh differenziale: quando provare il partial, e i contatori
 * che dicono come va. Il meccanismo (CRC del piano rosso, ruoli delle RAM, LUT)
 * sta nel driver del pannello, in GxEPD2_SOLUM_Differential.h; qui c'è solo la
 * decisione, scritta come funzioni pure che non leggono nè l'orologio nè il
 * sensore, così si provano su host.
 *
 * Due decisioni restano al driver e non sono input di decide(): se esiste un
 * riferimento valido, che solo lui sa, e se il rosso è cambiato, che si vede solo
 * disegnando il frame.
 */
namespace PartialRefresh
{
  /** Esito di un frame, o il motivo per cui non si prova il partial. */
  enum class Reason : uint8_t
  {
    Partial,        // il frame prova il partial (e come esito finale: partito)
    Disabled,       // interruttore spento o pannello senza differenziale
    NoLastFrame,    // nessun frame precedente: primo dopo il boot
    LongGap,        // troppo tempo dall'ultimo frame: notte, riavvio, manutenzione
    ChainLimit,     // tetto di partial consecutivi raggiunto
    SensorInvalid,  // sensore collegato ma senza temperatura: non si sa se la LUT è adatta
    TooCold,        // sotto la soglia di temperatura
    NoReference,    // il driver non ha un riferimento valido
    RedChanged      // il rosso è cambiato: il frame è stato rifatto in pieno
  };

  /** Valori della politica; i default vengono da Timings.h. */
  struct Params
  {
    uint8_t  chainMax;
    float    minTempC;
    float    hysteresisC;
    uint32_t maxGapMs;
  };

  inline constexpr Params DEFAULT_PARAMS = {
    (uint8_t)PARTIAL_CHAIN_MAX, PARTIAL_MIN_TEMP_C, PARTIAL_TEMP_HYSTERESIS_C, (uint32_t)PARTIAL_MAX_GAP_MS
  };

  /** Ciò che serve a decidere, già letto dal chiamante. */
  struct Inputs
  {
    bool     enabled;           // interruttore acceso e pannello con il differenziale
    bool     hasLastFrame;
    uint32_t msSinceLastFrame;
    uint8_t  chain;             // partial consecutivi dall'ultimo pieno
    bool     sensorPresent;     // BME680 inizializzato; falso = non collegato
    bool     sensorValid;
    float    temperatureC;
    bool     coldLatched;       // l'ultima decisione è stata TooCold
  };

  /**
   * Decide se un frame può provare il partial. Ordine delle condizioni: prima
   * quelle che non dipendono dal sensore, poi il sensore. Senza BME680 collegato
   * la temperatura non si controlla e il partial si prova comunque: in una stanza
   * fredda il nero può uscire sbiadito, perchè la LUT del partial non compensa il
   * freddo. Con il sensore collegato un campione non valido tiene il frame pieno.
   * Una temperatura non numerica non supera mai la soglia, perchè con NaN ogni
   * confronto è falso: per questo la condizione è scritta come negazione di ">=".
   */
  inline Reason decide(const Params& p, const Inputs& in)
  {
    if (!in.enabled) return Reason::Disabled;
    if (!in.hasLastFrame) return Reason::NoLastFrame;
    if (in.msSinceLastFrame > p.maxGapMs) return Reason::LongGap;
    if (in.chain >= p.chainMax) return Reason::ChainLimit;
    if (!in.sensorPresent) return Reason::Partial;
    if (!in.sensorValid) return Reason::SensorInvalid;
    const float threshold = in.coldLatched ? p.minTempC + p.hysteresisC : p.minTempC;
    if (!(in.temperatureC >= threshold)) return Reason::TooCold;
    return Reason::Partial;
  }

  /** Testo italiano breve per il log. */
  inline const char* reasonText(Reason r)
  {
    switch (r)
    {
      case Reason::Partial:       return "partial";
      case Reason::Disabled:      return "differenziale spento";
      case Reason::NoLastFrame:   return "primo frame";
      case Reason::LongGap:       return "troppo tempo dall'ultimo frame";
      case Reason::ChainLimit:    return "tetto della catena";
      case Reason::SensorInvalid: return "temperatura non disponibile";
      case Reason::TooCold:       return "temperatura sotto soglia";
      case Reason::NoReference:   return "nessun riferimento";
      case Reason::RedChanged:    return "rosso cambiato";
    }
    return "?";
  }

  /** Contatori di un giorno locale, per misurare la quota di partial dal log. */
  struct DayCounters
  {
    int32_t  dayKey;       // -1 finchè l'ora non è valida
    uint16_t partial;      // frame chiusi con un partial
    uint16_t full;         // frame chiusi con un refresh pieno, rifiutati compresi
    uint16_t redRefused;   // sottoinsieme di full: provati e rifiutati per il rosso
  };

  /** Stato della politica fra un frame e l'altro. */
  struct State
  {
    bool        hasLastFrame = false;
    uint32_t    lastFrameMs = 0;
    bool        coldLatched = false;
    DayCounters day = { -1, 0, 0, 0 };
  };

  /**
   * Compone gli input di decide() dallo stato e dalle letture del momento.
   * L'ora entra come argomento (millis() del chiamante) e la differenza è
   * aritmetica modulare, quindi regge il rollover.
   */
  inline Inputs makeInputs(const State& s, uint32_t nowMs, bool enabled, uint8_t chain,
                           bool sensorPresent, bool sensorValid, float temperatureC)
  {
    Inputs in;
    in.enabled = enabled;
    in.hasLastFrame = s.hasLastFrame;
    in.msSinceLastFrame = s.hasLastFrame ? (uint32_t)(nowMs - s.lastFrameMs) : 0;
    in.chain = chain;
    in.sensorPresent = sensorPresent;
    in.sensorValid = sensorValid;
    in.temperatureC = temperatureC;
    in.coldLatched = s.coldLatched;
    return in;
  }

  /**
   * Registra un frame concluso. `decision` è l'esito di decide(); `finalReason`
   * è come è finito il frame: Partial se il partial è partito, altrimenti il
   * motivo per cui è stato pieno (la decisione, oppure NoReference o RedChanged
   * dal driver). `dayKey` è il giorno locale, -1 senza ora valida: i contatori
   * si azzerano solo con un'ora valida che cambia giorno.
   */
  inline void onFrameDone(State& s, uint32_t nowMs, Reason decision, Reason finalReason, int32_t dayKey)
  {
    s.hasLastFrame = true;
    s.lastFrameMs = nowMs;
    s.coldLatched = (decision == Reason::TooCold);
    if (dayKey >= 0 && dayKey != s.day.dayKey)
    {
      s.day = { dayKey, 0, 0, 0 };
    }
    if (finalReason == Reason::Partial)
    {
      if (s.day.partial < 65535) ++s.day.partial;
    }
    else
    {
      if (s.day.full < 65535) ++s.day.full;
      if (finalReason == Reason::RedChanged && s.day.redRefused < 65535) ++s.day.redRefused;
    }
  }

  // -------------------------------------------------------------------------
  // Aggancio al driver del pannello
  //
  // Weather::renderFrame() e initDisplay() non sono template, e in una funzione
  // non template un `if constexpr` scartato verrebbe comunque compilato: un
  // driver senza l'API non compilerebbe. Gli helper qui sotto sono template sul
  // tipo del driver, quindi il ramo scartato non si istanzia, e un pannello che
  // non dichiara hasDifferentialRefresh si comporta come se il differenziale
  // fosse spento.
  // -------------------------------------------------------------------------

  /** Vero se il driver Panel dichiara hasDifferentialRefresh = true. */
  template <class Panel, class = void>
  struct SupportsDifferential : std::false_type {};

  template <class Panel>
  struct SupportsDifferential<Panel, std::void_t<decltype(Panel::hasDifferentialRefresh)>>
      : std::bool_constant<Panel::hasDifferentialRefresh> {};

  /** Vero se il differenziale è acceso nel firmware e il driver lo supporta. */
  template <class Epd>
  inline constexpr bool isAvailable()
  {
    return (DISPLAY_PARTIAL_REFRESH != 0) && SupportsDifferential<Epd>::value;
  }

  template <class Epd>
  inline void setEnabled(Epd& epd, bool on)
  {
    if constexpr (SupportsDifferential<Epd>::value) epd.setDifferentialRefresh(on);
    else { (void)epd; (void)on; }
  }

  /** Arma il frame; da chiamare prima di firstPage(). Falso senza riferimento. */
  template <class Epd>
  inline bool beginFrame(Epd& epd)
  {
    if constexpr (SupportsDifferential<Epd>::value) return epd.beginDifferentialFrame();
    else { (void)epd; return false; }
  }

  /** Chiude il frame armato. Vero se il partial è partito. */
  template <class Epd>
  inline bool endFrame(Epd& epd)
  {
    if constexpr (SupportsDifferential<Epd>::value) return epd.endDifferentialFrame();
    else { (void)epd; return false; }
  }

  template <class Epd>
  inline uint8_t chain(Epd& epd)
  {
    if constexpr (SupportsDifferential<Epd>::value) return epd.differentialChain();
    else { (void)epd; return 0; }
  }

  template <class Epd>
  inline uint16_t refusedPages(Epd& epd)
  {
    if constexpr (SupportsDifferential<Epd>::value) return epd.differentialRefusedPages();
    else { (void)epd; return 0; }
  }
}

#endif

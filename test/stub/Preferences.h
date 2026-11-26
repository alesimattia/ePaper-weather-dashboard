#pragma once
/** NVS finta in memoria: rende verificabile il round-trip di Timings e di
 *  Routefinder. Una mappa per tipo, chiavi senza namespace come basta ai test.
 *  Le put*() ritornano i byte scritti come quelle vere, e 0 quando il test
 *  inietta un errore di scrittura con scritturaFallisce. */
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

struct Preferences
{
  static inline std::map<std::string, uint16_t> store;
  static inline std::map<std::string, int32_t> storeInt;
  static inline std::map<std::string, uint8_t> storeUChar;
  static inline bool disponibile = true;

  /** Errore di scrittura iniettato, come una partizione NVS piena: con
   *  scritturaFallisce vero ogni put*() ritorna 0 e non memorizza niente; se
   *  chiaveCheFallisce non è vuota, fallisce solo quella chiave. */
  static inline bool scritturaFallisce = false;
  static inline std::string chiaveCheFallisce;
  static bool fallisce(const char* k)
  {
    return scritturaFallisce && (chiaveCheFallisce.empty() || chiaveCheFallisce == k);
  }

  bool begin(const char*, bool = false) { return disponibile; }
  void end() {}
  void clear() { store.clear(); storeInt.clear(); storeUChar.clear(); }
  uint16_t getUShort(const char* k, uint16_t d = 0)
  {
    auto i = store.find(k);
    return i == store.end() ? d : i->second;
  }
  size_t putUShort(const char* k, uint16_t v)
  {
    if (fallisce(k)) return 0;
    store[k] = v;
    return sizeof(uint16_t);
  }
  int32_t getInt(const char* k, int32_t d = 0)
  {
    auto i = storeInt.find(k);
    return i == storeInt.end() ? d : i->second;
  }
  size_t putInt(const char* k, int32_t v)
  {
    if (fallisce(k)) return 0;
    storeInt[k] = v;
    return sizeof(int32_t);
  }
  uint8_t getUChar(const char* k, uint8_t d = 0)
  {
    auto i = storeUChar.find(k);
    return i == storeUChar.end() ? d : i->second;
  }
  size_t putUChar(const char* k, uint8_t v)
  {
    if (fallisce(k)) return 0;
    storeUChar[k] = v;
    return sizeof(uint8_t);
  }
};

#pragma once
/** NVS finta in memoria: rende verificabile il round-trip di Timings e di
 *  Routefinder. Una mappa per tipo, chiavi senza namespace come basta ai test. */
#include <cstdint>
#include <map>
#include <string>

struct Preferences
{
  static inline std::map<std::string, uint16_t> store;
  static inline std::map<std::string, int32_t> storeInt;
  static inline std::map<std::string, uint8_t> storeUChar;
  static inline bool disponibile = true;

  bool begin(const char*, bool = false) { return disponibile; }
  void end() {}
  void clear() { store.clear(); storeInt.clear(); storeUChar.clear(); }
  uint16_t getUShort(const char* k, uint16_t d = 0)
  {
    auto i = store.find(k);
    return i == store.end() ? d : i->second;
  }
  void putUShort(const char* k, uint16_t v) { store[k] = v; }
  int32_t getInt(const char* k, int32_t d = 0)
  {
    auto i = storeInt.find(k);
    return i == storeInt.end() ? d : i->second;
  }
  void putInt(const char* k, int32_t v) { storeInt[k] = v; }
  uint8_t getUChar(const char* k, uint8_t d = 0)
  {
    auto i = storeUChar.find(k);
    return i == storeUChar.end() ? d : i->second;
  }
  void putUChar(const char* k, uint8_t v) { storeUChar[k] = v; }
};

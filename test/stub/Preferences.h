#pragma once
/** NVS finta in memoria: rende verificabile il round-trip di Timings. */
#include <cstdint>
#include <map>
#include <string>

struct Preferences
{
  static inline std::map<std::string, uint16_t> store;
  static inline bool disponibile = true;

  bool begin(const char*, bool = false) { return disponibile; }
  void end() {}
  void clear() { store.clear(); }
  uint16_t getUShort(const char* k, uint16_t d = 0)
  {
    auto i = store.find(k);
    return i == store.end() ? d : i->second;
  }
  void putUShort(const char* k, uint16_t v) { store[k] = v; }
};

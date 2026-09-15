#pragma once
/**
 * Minimo di Arduino.h che serve a Timings.h e Scheduler.h su host.
 * L'orologio monotono e' controllato dal test tramite g_msFinto.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <ctype.h>

extern uint32_t g_msFinto;
inline uint32_t millis() { return g_msFinto; }
inline void delay(uint32_t) {}

/**
 * API del core ESP32 che Clock.h usa e che su host non esiste. Non fa niente:
 * i test non sincronizzano nessun orologio, il fuso glielo da' fuso_posix.h.
 */
inline void configTzTime(const char*, const char*, const char* = nullptr) {}

#ifdef _MSC_VER
/** Il CRT Windows ha questi due con il nome sotto trattino basso. */
inline int  setenv(const char* chiave, const char* valore, int) { return _putenv_s(chiave, valore); }
inline void tzset() { _tzset(); }
#endif

/**
 * printf ritorna size_t come Print::printf del core ESP32: LOG_DISCARD gli
 * applica sizeof, quindi un ritorno void non compilerebbe.
 */
struct SerialeFinta
{
  template <class... A> size_t printf(const char* f, A... a) { return (size_t)std::printf(f, a...); }
  void flush() {}
};
extern SerialeFinta Serial;

struct Print
{
  virtual size_t write(uint8_t) = 0;
  template <class... A> size_t printf(const char* f, A... a) { return (size_t)std::printf(f, a...); }
};

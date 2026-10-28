#pragma once
// Stub minimo di esp_sntp.h per i test su host: Clock.h registra la callback e
// chiede la risincronizzazione, ma i test non esercitano SNTP.
#if __has_include(<sys/time.h>)
  #include <sys/time.h>
#else
  #include <ctime>
  #include <cstdint>
  struct timeval { long long tv_sec; long tv_usec; };
  inline int gettimeofday(struct timeval* tv, void*)
  {
    tv->tv_sec = (long long)time(nullptr);
    tv->tv_usec = 0;
    return 0;
  }
#endif

typedef void (*sntp_sync_time_cb_t)(struct timeval* tv);
inline void sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t) {}
inline bool sntp_restart() { return true; }

#pragma once
/**
 * Tipi di select() che Tuya.h usa. Su POSIX bastano gli header di sistema; su
 * Windows, dove select() è di Winsock, se ne dichiarano di minimi. La funzione
 * select() vera non si chiama mai: il test la sostituisce con una macro.
 */
#ifdef _MSC_VER
#include <cstring>
#include "esp_sntp.h"   // porta già la struct timeval, che non va ridefinita
typedef long suseconds_t;
struct fd_set
{
  unsigned char bits[16];
};
#define FD_ZERO(set) memset((set), 0, sizeof(fd_set))
#define FD_SET(fd, set) ((set)->bits[(fd) / 8] |= (unsigned char)(1u << ((fd) % 8)))
#else
#include <sys/select.h>
#include <sys/time.h>
#endif

#pragma once
#include <cstdint>
enum { WL_CONNECTED = 3, WIFI_OFF = 0 };
struct WiFiFinto
{
  int stato = WL_CONNECTED;
  int modo  = WIFI_OFF;
  int status() { return stato; }
  int getMode() { return modo; }
};
extern WiFiFinto WiFi;

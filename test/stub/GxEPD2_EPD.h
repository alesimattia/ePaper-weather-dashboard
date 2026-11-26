#pragma once
/**
 * Minimo di GxEPD2_EPD.h che serve a GxEPDImage.h su host: i colori di GxEPD2,
 * min e max come nel core Arduino e la lettura da PROGMEM, che su ESP32 è un
 * accesso normale alla memoria.
 */
#include <algorithm>
#include <cstdint>

using std::max;
using std::min;

#define GxEPD_BLACK 0x0000
#define GxEPD_WHITE 0xFFFF
#define GxEPD_RED   0xF800

#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#endif

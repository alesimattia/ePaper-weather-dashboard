#pragma once
/**
 * Minimo di esp_heap_caps.h che serve a CinemaStore.h su host: le allocazioni
 * vanno su malloc() e il test può farle fallire con g_heapEsaurito, per
 * verificare che l'archivio ritenti invece di disattivarsi.
 */
#include <cstddef>
#include <cstdlib>

#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_8BIT     (1 << 2)

inline bool g_heapEsaurito = false;

inline void* heap_caps_malloc(size_t size, unsigned)
{
  return g_heapEsaurito ? nullptr : std::malloc(size);
}

inline size_t heap_caps_get_free_size(unsigned) { return 0; }
inline size_t heap_caps_get_largest_free_block(unsigned) { return 0; }

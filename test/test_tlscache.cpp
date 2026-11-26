/**
 * Verifica della cache delle sessioni TLS (TlsSessionCache.h): sessioni
 * distinte per host e per modo di verifica, commit che rende valida una
 * sessione, oltre la capacità scartata, sostituzione del meno usato e oblio
 * dopo un handshake fallito.
 *
 * Include l'header VERO (-I..), che non dipende da mbedTLS: salva e
 * restituisce byte.
 */
#include "TlsSessionCache.h"
#include <cstdio>
#include <cstring>

static int falliti = 0;
static void ok(bool c, const char* nome)
{
  std::printf("%-76s %s\n", nome, c ? "ok" : "FALLITO");
  if (!c) ++falliti;
}

using Cache = TlsSessionCache<2, 16>;

/** Salva length byte uguali a fill per (host, verify). */
static void store(Cache& cache, const char* host, bool verify, uint8_t fill, size_t length)
{
  uint8_t* buffer = cache.reserve(host, verify);
  if (buffer == nullptr) return;
  std::memset(buffer, fill, length <= Cache::capacity() ? length : Cache::capacity());
  cache.commit(length);
}

int main()
{
  {
    Cache cache;
    size_t length = 0;
    ok(cache.find("a.example", true, length) == nullptr, "cache vuota: nessuna sessione");
    store(cache, "a.example", true, 0xA1, 10);
    const uint8_t* data = cache.find("a.example", true, length);
    ok(data != nullptr && length == 10 && data[0] == 0xA1 && data[9] == 0xA1, "sessione salvata e ritrovata");
    ok(cache.find("a.example", false, length) == nullptr,
       "una sessione verificata non vale per una richiesta insecure dello stesso host");
    ok(cache.find("b.example", true, length) == nullptr, "un altro host non la vede");
  }

  {
    Cache cache;
    size_t length = 0;
    uint8_t* buffer = cache.reserve("a.example", true);
    ok(buffer != nullptr, "reserve dà un buffer");
    ok(cache.find("a.example", true, length) == nullptr, "senza commit la sessione non è valida");
    cache.commit(0);
    ok(cache.find("a.example", true, length) == nullptr, "commit(0) la scarta");
    store(cache, "a.example", true, 0x11, 17);
    ok(cache.find("a.example", true, length) == nullptr, "una sessione oltre la capacità non si salva");
  }

  {
    Cache cache;
    size_t length = 0;
    store(cache, "a.example", true, 0x01, 4);
    store(cache, "b.example", true, 0x02, 4);
    cache.find("a.example", true, length);          // a è ora la più recente
    store(cache, "c.example", true, 0x03, 4);        // sostituisce b, la meno usata
    ok(cache.find("a.example", true, length) != nullptr, "la sessione usata di recente resta");
    ok(cache.find("b.example", true, length) == nullptr, "la meno usata viene sostituita");
    ok(cache.find("c.example", true, length) != nullptr, "la nuova sessione è salvata");
    store(cache, "a.example", true, 0x09, 6);
    const uint8_t* data = cache.find("a.example", true, length);
    ok(data != nullptr && length == 6 && data[0] == 0x09, "un host già presente riusa il suo slot");
  }

  {
    Cache cache;
    size_t length = 0;
    store(cache, "a.example", true, 0x01, 4);
    cache.forget("a.example", true);
    ok(cache.find("a.example", true, length) == nullptr, "forget scarta la sessione dopo un handshake fallito");
    char longHost[80];
    std::memset(longHost, 'x', sizeof(longHost) - 1);
    longHost[sizeof(longHost) - 1] = '\0';
    ok(cache.reserve(longHost, true) == nullptr, "un host più lungo dello spazio del nome non si salva");
  }

  std::printf("\n%s\n", falliti == 0 ? "tutti i casi superati" : "CI SONO CASI FALLITI");
  return falliti == 0 ? 0 : 1;
}

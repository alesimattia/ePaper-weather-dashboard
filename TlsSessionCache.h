#ifndef TLS_SESSION_CACHE_H
#define TLS_SESSION_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * Cache delle sessioni TLS serializzate, per host e per modo di verifica, in
 * buffer statici: niente heap, quindi nessuna frammentazione fra un ciclo e
 * l'altro. Una sessione verificata e una insecure dello stesso host restano
 * distinte, così una richiesta verificata non riprende mai una sessione nata
 * senza verifica. Quando gli slot sono pieni si sostituisce il meno usato di
 * recente. Non conosce mbedTLS: salva e restituisce byte, e la usa Net.h.
 */
template <size_t SLOTS, size_t BYTES>
class TlsSessionCache
{
public:
  static constexpr size_t HOST_CAPACITY = 64;

  /** Sessione salvata per (host, verify), nullptr se assente. Aggiorna l'uso recente. */
  const uint8_t* find(const char* host, bool verify, size_t& length)
  {
    Slot* slot = lookup(host, verify);
    if (slot == nullptr) return nullptr;
    slot->lastUse = ++useCounter;
    length = slot->length;
    return slot->data;
  }

  /**
   * Buffer in cui serializzare la sessione di (host, verify): lo slot già
   * assegnato all'host, altrimenti uno libero o il meno usato. Il contenuto
   * diventa valido solo con commit(); nullptr se il nome dell'host non entra.
   */
  uint8_t* reserve(const char* host, bool verify)
  {
    if (host == nullptr || strlen(host) >= HOST_CAPACITY) return nullptr;
    Slot* slot = lookup(host, verify);
    if (slot == nullptr) slot = victim();
    strncpy(slot->host, host, HOST_CAPACITY - 1);
    slot->host[HOST_CAPACITY - 1] = '\0';
    slot->verify = verify;
    slot->length = 0;
    slot->lastUse = ++useCounter;
    pending = slot;
    return slot->data;
  }

  /** Rende valida la sessione scritta nell'ultimo reserve(); 0 la scarta. */
  void commit(size_t length)
  {
    if (pending == nullptr) return;
    pending->length = length <= BYTES ? (uint16_t)length : 0;
    if (pending->length == 0) pending->host[0] = '\0';
    pending = nullptr;
  }

  /** Scarta la sessione di (host, verify), per esempio dopo un handshake fallito. */
  void forget(const char* host, bool verify)
  {
    Slot* slot = lookup(host, verify);
    if (slot == nullptr) return;
    slot->host[0] = '\0';
    slot->length = 0;
  }

  /** Byte disponibili per una sessione serializzata. */
  static constexpr size_t capacity() { return BYTES; }

private:
  struct Slot
  {
    char host[HOST_CAPACITY] = {};
    bool verify = false;
    uint16_t length = 0;
    uint32_t lastUse = 0;
    uint8_t data[BYTES] = {};
  };

  Slot* lookup(const char* host, bool verify)
  {
    if (host == nullptr) return nullptr;
    for (Slot& slot : slots)
      if (slot.length > 0 && slot.verify == verify && strcmp(slot.host, host) == 0) return &slot;
    return nullptr;
  }

  /** Primo slot vuoto, altrimenti quello usato meno di recente. */
  Slot* victim()
  {
    Slot* oldest = &slots[0];
    for (Slot& slot : slots)
    {
      if (slot.length == 0) return &slot;
      if (slot.lastUse < oldest->lastUse) oldest = &slot;
    }
    return oldest;
  }

  Slot slots[SLOTS];
  Slot* pending = nullptr;
  uint32_t useCounter = 0;
};

#endif // TLS_SESSION_CACHE_H

#pragma once
/**
 * esp-tls finto: un broker MQTT scriptabile in memoria. Ogni scrittura del
 * firmware viene registrata e, se il test ha installato onPacket, il broker
 * può rispondere accodando byte in ingresso. Nessuna rete, nessun TLS.
 *
 * Il tempo scorre solo quando il firmware attende: select() del test fa
 * avanzare g_msFinto del timeout richiesto, così un broker muto fa scadere i
 * timeout senza che il test aspetti davvero.
 */
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

/** Tipo di errore e costanti del core ESP32 che gli stub di esp_tls usano. */
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#define ESP_FAIL -1
#endif

#ifdef _MSC_VER
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#else
#include <sys/types.h>
#endif

#define ESP_TLS_ERR_SSL_WANT_READ -0x6900
#define ESP_TLS_ERR_SSL_WANT_WRITE -0x6880

typedef struct esp_tls_last_error
{
  esp_err_t last_error;
  int esp_tls_error_code;
  int esp_tls_flags;
} esp_tls_last_error_t;
typedef struct esp_tls_last_error* esp_tls_error_handle_t;

struct esp_tls_cfg_t
{
  esp_err_t (*crt_bundle_attach)(void* conf);
  int timeout_ms;
};

struct esp_tls_t
{
  esp_tls_last_error error;
};

/** Stato del broker finto, condiviso fra gli stub e il test. */
struct FakeBroker
{
  std::vector<uint8_t> written;   // tutto ciò che il firmware ha scritto
  std::deque<uint8_t> incoming;   // byte che il broker "manda" al firmware
  void (*onPacket)(const std::vector<uint8_t>& packet) = nullptr;
  bool connectOk       = true;
  bool peerClosed      = false;   // il broker ha chiuso: read ritorna 0
  size_t readChunk     = 0;       // se > 0, ogni read ritorna al più tanti byte
  bool wantReadFirst   = false;   // la prima read di ogni giro ritorna WANT_READ
  int created          = 0;
  int destroyed        = 0;
  int liveHandles      = 0;
  int lastTimeoutMs    = 0;
  bool bundleAttached  = false;

  void reset() { *this = FakeBroker(); }
};
inline FakeBroker g_broker;

inline esp_tls_t* esp_tls_init()
{
  ++g_broker.created;
  ++g_broker.liveHandles;
  return new esp_tls_t();
}

inline int esp_tls_conn_new_sync(const char*, int, int, const esp_tls_cfg_t* cfg, esp_tls_t* tls)
{
  g_broker.lastTimeoutMs   = cfg->timeout_ms;
  g_broker.bundleAttached  = cfg->crt_bundle_attach != nullptr;
  if (!g_broker.connectOk)
  {
    tls->error.last_error         = 0x8001;
    tls->error.esp_tls_error_code = -0x2700;
    tls->error.esp_tls_flags      = 0x08;
    return -1;
  }
  return 1;
}

inline ssize_t esp_tls_conn_write(esp_tls_t*, const void* data, size_t length)
{
  const uint8_t* bytes = (const uint8_t*)data;
  std::vector<uint8_t> packet(bytes, bytes + length);
  g_broker.written.insert(g_broker.written.end(), packet.begin(), packet.end());
  if (g_broker.onPacket) g_broker.onPacket(packet);
  return (ssize_t)length;
}

inline ssize_t esp_tls_conn_read(esp_tls_t*, void* data, size_t length)
{
  static bool skippedOnce = false;
  if (g_broker.wantReadFirst && !skippedOnce)
  {
    skippedOnce = true;
    return ESP_TLS_ERR_SSL_WANT_READ;
  }
  skippedOnce = false;

  if (g_broker.incoming.empty()) return g_broker.peerClosed ? 0 : ESP_TLS_ERR_SSL_WANT_READ;

  size_t count = length < g_broker.incoming.size() ? length : g_broker.incoming.size();
  if (g_broker.readChunk > 0 && count > g_broker.readChunk) count = g_broker.readChunk;
  uint8_t* out = (uint8_t*)data;
  for (size_t i = 0; i < count; ++i)
  {
    out[i] = g_broker.incoming.front();
    g_broker.incoming.pop_front();
  }
  return (ssize_t)count;
}

inline ssize_t esp_tls_get_bytes_avail(esp_tls_t*) { return (ssize_t)g_broker.incoming.size(); }

inline esp_err_t esp_tls_get_conn_sockfd(esp_tls_t*, int* sockfd)
{
  *sockfd = 7;
  return ESP_OK;
}

inline esp_err_t esp_tls_get_error_handle(esp_tls_t* tls, esp_tls_error_handle_t* handle)
{
  *handle = &tls->error;
  return ESP_OK;
}

inline int esp_tls_conn_destroy(esp_tls_t* tls)
{
  ++g_broker.destroyed;
  --g_broker.liveHandles;
  delete tls;
  return 0;
}

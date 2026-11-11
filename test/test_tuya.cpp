/**
 * Verifica di Tuya.h: il client MQTT minimale che pubblica il BME680 sul cloud
 * Tuya. Include l'header VERO del firmware, con un esp-tls finto che fa da
 * broker scriptabile (stub/esp_tls.h) e un HMAC-SHA256 indipendente
 * (stub/mbedtls/md.h).
 *
 * Cosa si verifica: la sequenza e il contenuto dei pacchetti CONNECT, SUBSCRIBE
 * e PUBLISH; che la sessione non mandi mai DISCONNECT nè PINGREQ e che dopo un
 * PUBACK la connessione resti aperta fino a releaseSession(), che è la ragione
 * d'essere del modulo (Tuya dichiara offline il dispositivo nell'istante in
 * cui la connessione si chiude); la chiusura immediata su ogni fallimento; i
 * timeout con un broker muto; le letture frammentate; i pacchetti più grandi
 * del buffer.
 *
 * Non si verifica la rete nè il TLS: quelli si provano solo sul dispositivo.
 *
 * Il test gira con TUYA_REQUEST_ACK a 1 (come il .ino) e a 0: il runner lo
 * compila due volte, perchè i due rami hanno sequenze di pacchetti diverse.
 */
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <lwip/sockets.h>

#include "esp_tls.h"

extern uint32_t g_msFinto;

/**
 * select() finto: un broker che ha chiuso risveglia subito la lettura, uno
 * muto fa scorrere il tempo del timeout richiesto. Sostituisce la funzione
 * vera con una macro, dopo che gli header di sistema l'hanno dichiarata.
 */
static int fakeSelect(int, fd_set*, fd_set*, fd_set*, struct timeval* timeout)
{
  if (g_broker.peerClosed) return 1;
  g_msFinto += (uint32_t)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
  return 0;
}
#define select fakeSelect

struct EspFinto
{
  uint32_t getFreeHeap() { return 100000; }
};
static EspFinto ESP;

#define LOG_LEVEL 1

#ifndef TUYA_REQUEST_ACK
#define TUYA_REQUEST_ACK 1
#endif

// Env.h, Indoor.h e Net.h veri tirano dentro credenziali, BSEC e WiFiClientSecure:
// si sostituiscono dichiarando prima le loro guardie.
#define ENV_H
#define TUYA_DEVICE_ID "testdevice0123456789ab"
#define TUYA_DEVICE_SECRET "testsecret_0123456"

#define INDOOR_H
namespace Indoor
{
  struct Sample
  {
    bool valid;
    float temperature;
    uint8_t humidity;
    uint16_t pressure;
    uint16_t iaq;
    uint8_t iaqAccuracy;
    uint32_t lastUpdateMs;
  };
  inline Sample g_sample = {};
  inline const Sample& sample() { return g_sample; }
}

#define NET_H
namespace Net
{
  inline bool isCredentialSet(const char* value)
  {
    return value != nullptr && value[0] != '\0' && strncmp(value, "paste_", 6) != 0 &&
           strncmp(value, "0000000", 7) != 0;
  }
}

#include "Timings.h"

#ifdef _MSC_VER
// Clock.h usa localtime_r(), che il CRT Windows non ha: lo stub POSIX dei
// fusi dei test lo fornisce (il fuso non conta qui, serve solo a compilare).
#include "fuso_posix.h"
#define localtime_r(a, b) fusoLocaltime(a, b)
#endif

#include "Tuya.h"

uint32_t g_msFinto = 0;
SerialeFinta Serial;

static int falliti = 0;
static void ok(bool c, const char* nome)
{
  std::printf("%-72s %s\n", nome, c ? "ok" : "FALLITO");
  if (!c) ++falliti;
}

// ---------------------------------------------------------------------------
// Costruzione e lettura dei pacchetti MQTT, scritte indipendentemente dal firmware
// ---------------------------------------------------------------------------
using Bytes = std::vector<uint8_t>;

struct Packet
{
  uint8_t first;
  Bytes body;
};

static Bytes packetOf(uint8_t first, const Bytes& body)
{
  Bytes out;
  out.push_back(first);
  size_t remaining = body.size();
  do
  {
    uint8_t digit = (uint8_t)(remaining % 128);
    remaining /= 128;
    if (remaining) digit |= 0x80;
    out.push_back(digit);
  } while (remaining);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

static std::vector<Packet> parsePackets(const Bytes& bytes)
{
  std::vector<Packet> packets;
  size_t i = 0;
  while (i < bytes.size())
  {
    Packet p;
    p.first = bytes[i++];
    size_t length = 0, multiplier = 1;
    while (i < bytes.size())
    {
      const uint8_t digit = bytes[i++];
      length += (size_t)(digit & 0x7F) * multiplier;
      multiplier *= 128;
      if (!(digit & 0x80)) break;
    }
    p.body.assign(bytes.begin() + i, bytes.begin() + i + length);
    i += length;
    packets.push_back(p);
  }
  return packets;
}

/** Legge una stringa MQTT (lunghezza a due byte) e avanza l'offset. */
static std::string readString(const Bytes& body, size_t& offset)
{
  const size_t length = ((size_t)body[offset] << 8) | body[offset + 1];
  std::string text((const char*)&body[offset + 2], length);
  offset += 2 + length;
  return text;
}

static Bytes connack(uint8_t returnCode) { return packetOf(0x20, {0x00, returnCode}); }
static Bytes puback(uint16_t id) { return packetOf(0x40, {(uint8_t)(id >> 8), (uint8_t)id}); }
static Bytes suback(uint16_t id) { return packetOf(0x90, {(uint8_t)(id >> 8), (uint8_t)id, 0x00}); }

/** PUBLISH in QoS 0 dal broker, come la risposta del cloud al property report. */
static Bytes cloudPublish(const std::string& topic, const std::string& json)
{
  Bytes body = {(uint8_t)(topic.size() >> 8), (uint8_t)topic.size()};
  body.insert(body.end(), topic.begin(), topic.end());
  body.insert(body.end(), json.begin(), json.end());
  return packetOf(0x30, body);
}

// ---------------------------------------------------------------------------
// Broker finto: reagisce ai pacchetti del firmware secondo lo scenario
// ---------------------------------------------------------------------------
struct Scenario
{
  uint8_t connackCode     = 0;
  bool answerConnect      = true;
  bool answerSubscribe    = true;
  bool answerPublish      = true;
  int pubackIdOffset      = 0;
  bool closeAfterConnack  = false;
  size_t oversizeBefore   = 0;      // PUBLISH inutile da N byte prima del PUBACK
  std::string cloudJson;            // risposta del cloud dopo il PUBACK, se non vuota
};
static Scenario g_scenario;

static void enqueue(const Bytes& bytes) { g_broker.incoming.insert(g_broker.incoming.end(), bytes.begin(), bytes.end()); }

static void brokerReacts(const Bytes& raw)
{
  const std::vector<Packet> packets = parsePackets(raw);
  if (packets.empty()) return;
  const Packet& p = packets[0];

  switch (p.first >> 4)
  {
    case 1: // CONNECT
      if (g_scenario.answerConnect) enqueue(connack(g_scenario.connackCode));
      if (g_scenario.closeAfterConnack) g_broker.peerClosed = true;
      break;

    case 8: // SUBSCRIBE
      if (g_scenario.answerSubscribe) enqueue(suback((uint16_t)((p.body[0] << 8) | p.body[1])));
      break;

    case 3: // PUBLISH
    {
      size_t offset = 0;
      readString(p.body, offset);
      const uint16_t id = (uint16_t)((p.body[offset] << 8) | p.body[offset + 1]);
      if (g_scenario.oversizeBefore > 0)
        enqueue(cloudPublish("tylink/other/topic", std::string(g_scenario.oversizeBefore, 'x')));
      if (g_scenario.answerPublish) enqueue(puback((uint16_t)(id + g_scenario.pubackIdOffset)));
      if (!g_scenario.cloudJson.empty())
        enqueue(cloudPublish("tylink/" TUYA_DEVICE_ID "/thing/property/report_response", g_scenario.cloudJson));
      break;
    }
    default:
      break;
  }
}

/** Prepara un giro pulito: broker azzerato, campione valido, modulo inizializzato. */
static void prepare(const Scenario& scenario)
{
  g_broker.reset();
  g_broker.onPacket = brokerReacts;
  g_scenario = scenario;
  g_msFinto = 0;
  Indoor::g_sample = {true, 22.5f, 45, 1013, 50, 1, 0};
  Tuya::begin();
}

static bool hasPacketType(const std::vector<Packet>& packets, uint8_t type)
{
  for (const Packet& p : packets)
    if ((p.first >> 4) == type) return true;
  return false;
}

/**
 * Rilascia la connessione rimasta aperta dopo un publish riuscito, come fa
 * wifiOff() a radio spenta, e verifica che non esca nessun byte e che non
 * resti nessun handle vivo.
 */
static bool releasedCleanly()
{
  const size_t writtenBefore = g_broker.written.size();
  Tuya::releaseSession();
  return g_broker.liveHandles == 0 && g_broker.written.size() == writtenBefore;
}

/** Vero se la sessione non ha mai mandato DISCONNECT (tipo 14) nè PINGREQ (tipo 12). */
static bool neverDisconnected(const std::vector<Packet>& packets)
{
  return !hasPacketType(packets, 14) && !hasPacketType(packets, 12);
}

static std::string hexOf(const uint8_t* data, size_t length)
{
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < length; ++i)
  {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 15];
  }
  return out;
}

// ---------------------------------------------------------------------------
int main()
{
  const size_t expectedPackets = TUYA_REQUEST_ACK ? 3 : 2;
  std::printf("TUYA_REQUEST_ACK = %d\n\n", (int)TUYA_REQUEST_ACK);

  // --- funzioni pure -------------------------------------------------------
  {
    char out[65];
    const char* key = "Jefe";
    const char* data = "what do ya want for nothing?";
    ok(Tuya::detail::hmacSha256Hex(key, data, out, sizeof(out)) &&
           std::string(out) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
       "hmacSha256Hex: vettore RFC 4231 caso 2");
    ok(!Tuya::detail::hmacSha256Hex(key, data, out, 64), "hmacSha256Hex: rifiuta un buffer da 64 byte");

    ok(Tuya::detail::parseBrokerUri("mqtts://m1.tuyaeu.com:8883") &&
           std::string(Tuya::detail::brokerHost) == "m1.tuyaeu.com" && Tuya::detail::brokerPort == 8883,
       "parseBrokerUri: host e porta dell'URI di default");
    ok(!Tuya::detail::parseBrokerUri("m1.tuyaeu.com:8883"), "parseBrokerUri: senza schema è rifiutato");
    ok(!Tuya::detail::parseBrokerUri("mqtts://m1.tuyaeu.com"), "parseBrokerUri: senza porta è rifiutato");
    ok(!Tuya::detail::parseBrokerUri("mqtts://:8883"), "parseBrokerUri: senza host è rifiutato");
    ok(!Tuya::detail::parseBrokerUri("mqtts://host:0"), "parseBrokerUri: porta 0 è rifiutata");
  }

  // --- percorso nominale ---------------------------------------------------
  {
    std::printf("\n-- percorso nominale\n");
    prepare(Scenario());
    if (TUYA_REQUEST_ACK) g_scenario.cloudJson = "{\"code\":0,\"msgId\":\"1\",\"time\":1,\"version\":\"1.0\"}";
    const bool result = Tuya::runPublish();
    const std::vector<Packet> packets = parsePackets(g_broker.written);

    ok(result, "runPublish ritorna true");
    ok(!Tuya::hasFailed(), "il badge di guasto resta spento");
    ok(packets.size() == expectedPackets, "sequenza: CONNECT (+ SUBSCRIBE con ack) + PUBLISH");
    ok(neverDisconnected(packets), "nessun DISCONNECT nè PINGREQ in tutta la sessione");
    ok(g_broker.created == 1 && g_broker.destroyed == 0 && g_broker.liveHandles == 1,
       "dopo il PUBACK la connessione resta aperta: chiuderla farebbe segnare offline");
    ok(releasedCleanly(), "releaseSession chiude senza scrivere nessun byte e senza handle vivi");
    ok(g_broker.bundleAttached, "il certificate bundle è agganciato alla connessione");
    ok(g_broker.lastTimeoutMs == (int)TUYA_CONNECT_TIMEOUT_MS, "timeout di connessione = TUYA_CONNECT_TIMEOUT_MS");

    // CONNECT
    const Packet& connect = packets[0];
    size_t o = 0;
    ok(connect.first == 0x10, "CONNECT: tipo 1, nessun flag");
    ok(readString(connect.body, o) == "MQTT" && connect.body[o] == 4, "CONNECT: protocollo MQTT livello 4 (3.1.1)");
    ok(connect.body[o + 1] == 0xC2, "CONNECT: flag username, password, sessione pulita");
    const uint16_t keepalive = (uint16_t)((connect.body[o + 2] << 8) | connect.body[o + 3]);
    ok(keepalive == TUYA_KEEPALIVE_S, "CONNECT: keepalive = TUYA_KEEPALIVE_S");
    o += 4;
    const std::string clientId = readString(connect.body, o);
    const std::string username = readString(connect.body, o);
    const std::string password = readString(connect.body, o);
    ok(clientId == "tuyalink_" TUYA_DEVICE_ID, "CONNECT: client id tuyalink_{deviceId}");

    unsigned long ts = 0;
    ok(std::sscanf(username.c_str(), TUYA_DEVICE_ID "|signMethod=hmacSha256,timestamp=%lu,secureMode=1,accessType=1", &ts) == 1,
       "CONNECT: username nella forma attesa");

    char signSrc[160];
    std::snprintf(signSrc, sizeof(signSrc), "deviceId=%s,timestamp=%lu,secureMode=1,accessType=1", TUYA_DEVICE_ID, ts);
    uint8_t digest[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t*)TUYA_DEVICE_SECRET,
                    strlen(TUYA_DEVICE_SECRET), (const uint8_t*)signSrc, strlen(signSrc), digest);
    ok(password == hexOf(digest, 32), "CONNECT: password = HMAC-SHA256 del secret sulla stringa firmata");

    // PUBLISH
    const Packet& publish = packets.back();
    o = 0;
    ok(publish.first == 0x32, "PUBLISH: QoS 1, nessun retain");
    ok(readString(publish.body, o) == "tylink/" TUYA_DEVICE_ID "/thing/property/report", "PUBLISH: topic property/report");
    const uint16_t id = (uint16_t)((publish.body[o] << 8) | publish.body[o + 1]);
    ok(id == (TUYA_REQUEST_ACK ? 2 : 1), "PUBLISH: packet id non nullo e coerente con la sessione");
    const std::string payload((const char*)&publish.body[o + 2], publish.body.size() - o - 2);

    char expected[400];
    std::snprintf(expected, sizeof(expected),
                  "{\"msgId\":\"1\",\"time\":%lu,\"data\":{\"temp_value\":{\"value\":22.5},\"hum_value\":{\"value\":45},"
                  "\"press_value\":{\"value\":1013},\"iaq_value\":{\"value\":50},\"iaq_acc\":{\"value\":1}}%s}",
                  ts, TUYA_REQUEST_ACK ? ",\"sys\":{\"ack\":1}" : "");
    ok(payload == expected, "PUBLISH: payload identico a quello che il cloud ha accettato");

#if TUYA_REQUEST_ACK
    ok(packets[1].first == 0x82 && packets[1].body.size() > 2, "SUBSCRIBE: tipo 8 con flag 2");
    o = 2;
    ok(readString(packets[1].body, o) == "tylink/" TUYA_DEVICE_ID "/thing/property/report_response",
       "SUBSCRIBE: topic report_response");
    ok(Tuya::detail::ackReceived, "la risposta del cloud è stata letta");
#endif
  }

  // --- letture frammentate -------------------------------------------------
  {
    std::printf("\n-- letture di un byte alla volta, con WANT_READ\n");
    prepare(Scenario());
    g_broker.readChunk = 1;
    g_broker.wantReadFirst = true;
    ok(Tuya::runPublish(), "runPublish riesce anche con letture frammentate");
    ok(releasedCleanly(), "releaseSession lascia zero handle vivi");
  }

  // --- pacchetto più grande del buffer ------------------------------------
  {
    std::printf("\n-- pacchetto in ingresso più grande del buffer\n");
    Scenario s;
    s.oversizeBefore = 700;
    prepare(s);
    ok(Tuya::runPublish(), "il pacchetto da 700 byte si scarta e il PUBACK successivo si legge");
    ok(releasedCleanly(), "releaseSession lascia zero handle vivi");
  }

  // --- CONNACK rifiutato ---------------------------------------------------
  {
    std::printf("\n-- CONNACK con return code 5\n");
    Scenario s;
    s.connackCode = 5;
    prepare(s);
    const bool result = Tuya::runPublish();
    const std::vector<Packet> packets = parsePackets(g_broker.written);
    ok(!result, "runPublish ritorna false");
    ok(Tuya::hasFailed(), "il badge di guasto si accende");
    ok(!hasPacketType(packets, 3), "nessun PUBLISH dopo il rifiuto");
    ok(neverDisconnected(packets), "nessun DISCONNECT nè PINGREQ");
    ok(g_broker.created == 1 && g_broker.liveHandles == 0, "handle distrutto");
    ok(g_msFinto < 100, "esce subito, senza consumare il timeout");
  }

  // --- broker muto ---------------------------------------------------------
  {
    std::printf("\n-- broker che non risponde al CONNECT\n");
    Scenario s;
    s.answerConnect = false;
    prepare(s);
    const bool result = Tuya::runPublish();
    ok(!result && Tuya::hasFailed(), "runPublish ritorna false e il badge si accende");
    ok(g_msFinto >= TUYA_CONNECT_TIMEOUT_MS && g_msFinto < TUYA_CONNECT_TIMEOUT_MS + 100,
       "attende TUYA_CONNECT_TIMEOUT_MS e non oltre");
    ok(!hasPacketType(parsePackets(g_broker.written), 3), "nessun PUBLISH");
    ok(g_broker.liveHandles == 0, "handle distrutto");
  }

  // --- PUBACK con id sbagliato ---------------------------------------------
  {
    std::printf("\n-- PUBACK per un altro packet id\n");
    Scenario s;
    s.pubackIdOffset = 7;
    prepare(s);
    const bool result = Tuya::runPublish();
    ok(!result && Tuya::hasFailed(), "un PUBACK estraneo non conferma il publish");
    ok(g_msFinto >= TUYA_PUBACK_TIMEOUT_MS, "attende TUYA_PUBACK_TIMEOUT_MS");
    ok(g_broker.liveHandles == 0, "handle distrutto");
  }

  // --- connessione chiusa a metà -------------------------------------------
  {
    std::printf("\n-- il broker chiude la connessione dopo il CONNACK\n");
    Scenario s;
    s.closeAfterConnack = true;
    s.answerSubscribe   = false;
    s.answerPublish     = false;
    prepare(s);
    const bool result = Tuya::runPublish();
    const std::vector<Packet> packets = parsePackets(g_broker.written);
    ok(!result && Tuya::hasFailed(), "runPublish ritorna false e il badge si accende");
#if TUYA_REQUEST_ACK
    // Solo l'attesa del SUBACK rivela la chiusura prima del publish: senza
    // ack fra CONNACK e PUBLISH non c'è nessuna lettura, e il publish parte.
    ok(!hasPacketType(packets, 3), "nessun PUBLISH su una connessione persa");
#endif
    ok(g_msFinto < 100, "esce subito, senza consumare il timeout");
    ok(g_broker.liveHandles == 0, "handle distrutto");
  }

  // --- handshake TLS fallito -----------------------------------------------
  {
    std::printf("\n-- handshake TLS fallito\n");
    prepare(Scenario());
    g_broker.connectOk = false;
    ok(!Tuya::runPublish() && Tuya::hasFailed(), "runPublish ritorna false e il badge si accende");
    ok(g_broker.written.empty(), "nessun byte scritto");
    ok(g_broker.created == 1 && g_broker.destroyed == 1, "handle creato e distrutto");
  }

  // --- sessione rimasta aperta ---------------------------------------------
  {
    std::printf("\n-- due publish senza spegnere la radio fra l'uno e l'altro\n");
    prepare(Scenario());
    ok(Tuya::runPublish() && g_broker.liveHandles == 1, "il primo publish lascia la connessione aperta");
    ok(Tuya::runPublish(), "il secondo publish riesce");
    ok(g_broker.created == 2 && g_broker.destroyed == 1 && g_broker.liveHandles == 1,
       "la connessione rimasta aperta si chiude prima di aprirne una nuova");
    ok(releasedCleanly(), "releaseSession lascia zero handle vivi");
    ok(releasedCleanly(), "releaseSession senza connessione aperta non fa niente");
  }

  // --- precondizioni -------------------------------------------------------
  {
    std::printf("\n-- campione non valido\n");
    prepare(Scenario());
    Indoor::g_sample.valid = false;
    ok(!Tuya::readyToPublish(), "readyToPublish è falso senza campione");
    ok(!Tuya::runPublish() && g_broker.created == 0, "runPublish non apre nessuna connessione");
    ok(!Tuya::hasFailed(), "senza campione non c'è un guasto da mostrare");
  }

#if TUYA_REQUEST_ACK
  // --- conferma applicativa del cloud --------------------------------------
  {
    std::printf("\n-- risposta del cloud con code 2003\n");
    prepare(Scenario());
    g_scenario.cloudJson = "{\"code\":2003,\"msgId\":\"1\",\"time\":1,\"version\":\"1.0\"}";
    ok(Tuya::runPublish(), "il PUBACK conferma la consegna anche se il cloud rifiuta i valori");
    ok(Tuya::detail::ackReceived, "la risposta di rifiuto è stata letta e loggata");
  }
  {
    std::printf("\n-- il cloud non risponde\n");
    prepare(Scenario());
    ok(Tuya::runPublish(), "senza risposta del cloud l'esito resta quello del PUBACK");
    ok(!Tuya::detail::ackReceived && g_msFinto >= TUYA_ACK_TIMEOUT_MS, "attende TUYA_ACK_TIMEOUT_MS");
    ok(releasedCleanly(), "releaseSession lascia zero handle vivi");
  }
#endif

  std::printf("\n%s\n", falliti ? "CI SONO FALLIMENTI" : "tutti i controlli superati");
  return falliti ? 1 : 0;
}

#pragma once
/**
 * HMAC-SHA256 completo e indipendente dal firmware, per verificare la password
 * di Tuya.h contro vettori noti. È codice di test: sull'ESP32 la funzione
 * vera sta in mbedtls.
 */
#include <cstddef>
#include <cstdint>
#include <cstring>

struct mbedtls_md_info_t
{
  int unused;
};
enum
{
  MBEDTLS_MD_SHA256 = 1
};

namespace stub_sha256
{
  inline uint32_t rotr(uint32_t value, int bits) { return (value >> bits) | (value << (32 - bits)); }

  /** SHA-256 di un buffer unico, scritto per leggibilità e non per velocità. */
  inline void hash(const uint8_t* data, size_t length, uint8_t out[32])
  {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    const size_t paddedLength = ((length + 9 + 63) / 64) * 64;
    uint8_t* padded = new uint8_t[paddedLength];
    memset(padded, 0, paddedLength);
    memcpy(padded, data, length);
    padded[length] = 0x80;
    const uint64_t bitLength = (uint64_t)length * 8;
    for (int i = 0; i < 8; ++i) padded[paddedLength - 1 - i] = (uint8_t)(bitLength >> (8 * i));

    for (size_t block = 0; block < paddedLength; block += 64)
    {
      uint32_t w[64];
      for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)padded[block + i * 4] << 24) | ((uint32_t)padded[block + i * 4 + 1] << 16) |
               ((uint32_t)padded[block + i * 4 + 2] << 8) | (uint32_t)padded[block + i * 4 + 3];
      for (int i = 16; i < 64; ++i)
      {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }
      uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
      for (int i = 0; i < 64; ++i)
      {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t t1 = hh + s1 + choose + k[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + majority;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
      }
      h[0] += a;
      h[1] += b;
      h[2] += c;
      h[3] += d;
      h[4] += e;
      h[5] += f;
      h[6] += g;
      h[7] += hh;
    }
    delete[] padded;
    for (int i = 0; i < 8; ++i)
    {
      out[i * 4]     = (uint8_t)(h[i] >> 24);
      out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
      out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
      out[i * 4 + 3] = (uint8_t)h[i];
    }
  }
} // namespace stub_sha256

inline const mbedtls_md_info_t* mbedtls_md_info_from_type(int type)
{
  static const mbedtls_md_info_t info = {1};
  return type == MBEDTLS_MD_SHA256 ? &info : nullptr;
}

/** HMAC one-shot come mbedtls_md_hmac(): chiave lunga quanto serve, uscita da 32 byte. */
inline int mbedtls_md_hmac(const mbedtls_md_info_t*, const uint8_t* key, size_t keyLength,
                           const uint8_t* input, size_t inputLength, uint8_t* out)
{
  uint8_t block[64] = {0};
  if (keyLength > sizeof(block))
    stub_sha256::hash(key, keyLength, block);
  else
    memcpy(block, key, keyLength);

  uint8_t inner[64 + 4096];
  uint8_t outer[64 + 32];
  if (inputLength > 4096) return -1;
  for (int i = 0; i < 64; ++i)
  {
    inner[i] = block[i] ^ 0x36;
    outer[i] = block[i] ^ 0x5c;
  }
  memcpy(inner + 64, input, inputLength);
  stub_sha256::hash(inner, 64 + inputLength, outer + 64);
  stub_sha256::hash(outer, 64 + 32, out);
  return 0;
}

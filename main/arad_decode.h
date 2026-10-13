// Arad / Master Meter Dialog3G decoder for CC1101 raw-chip packets.
// Checksum + bit correction ported from rtl_433 src/devices/arad_ms_meter.c (GPL-2.0+).
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>

struct AradResult {
  bool ok;
  uint32_t serial;
  uint8_t suffix;
  uint8_t flags1, flags2, b9;   // b9: payload byte 9, meaning unknown (logged for experiments)
  int leaking;
  uint32_t raw_counter;
  float gear;
  const char *unit;
  double volume;
  int corrections;      // checksum bit corrections
  int manchester_errs;  // invalid chip pairs
  int sync_errs;        // bit errors in the fixed 40-bit sync/uid tail
  char fail[24];
};

namespace arad {

static const uint64_t MASK = 0xffffffffffULL, KEY0 = 0x3037889dd8ULL,
  G39 = 0x00014013f8ULL, G31 = 0x201080d890ULL, G23 = 0x00018f36c8ULL;
static uint64_t keys[88];
static bool keys_init = false;

static inline uint64_t roll(uint64_t k) {
  uint64_t n = (k << 1) & MASK;
  if (k & (1ULL << 39)) n ^= G39;
  if (k & (1ULL << 31)) n ^= G31;
  if (k & (1ULL << 23)) n ^= G23;
  return n;
}
static void init_keys() {
  if (keys_init) return;
  uint64_t k = KEY0;
  for (int j = 87; j >= 0; --j) { keys[j] = k; k = roll(k); }
  keys_init = true;
}
static uint64_t checksum(const uint8_t *b) {
  uint64_t s = 0;
  for (int n = 0; n < 11; n++)
    for (int i = 0; i < 8; i++)
      if ((b[n] >> (7 - i)) & 1) s ^= keys[n * 8 + i];
  return s;
}
static inline void flip(uint8_t *b, int i) { b[i / 8] ^= (uint8_t)(1u << (7 - i % 8)); }
static int correct(uint8_t *b, uint64_t syn) {
  for (int i = 0; i < 88; i++) if (keys[i] == syn) { flip(b, i); return 1; }
  for (int i = 0; i < 88; i++) for (int j = i + 1; j < 88; j++)
    if ((keys[i] ^ keys[j]) == syn) { flip(b, i); flip(b, j); return 2; }
  for (int i = 0; i < 88; i++) for (int j = i + 1; j < 88; j++) {
    uint64_t x = keys[i] ^ keys[j];
    for (int k = j + 1; k < 88; k++)
      if ((x ^ keys[k]) == syn) { flip(b, i); flip(b, j); flip(b, k); return 3; }
  }
  return -1;
}
static inline int popcount8(uint8_t v) { int c = 0; while (v) { c += v & 1; v >>= 1; } return c; }

// chips: bytes from CC1101 FIFO directly after the 16-chip sync word (0xA556 = raw byte 0xC1).
// invert: true if the radio's FSK polarity is opposite (high freq = 0).
// Expects >= 42 chip bytes: raw 96 F5 13 85 37 + 16 payload bytes, Manchester "10"=1 "01"=0.
static AradResult decode(const uint8_t *chips, int nchips, bool invert) {
  AradResult r; memset(&r, 0, sizeof(r));
  init_keys();
  if (nchips < 42) { snprintf(r.fail, sizeof r.fail, "short"); return r; }
  uint8_t raw[21];
  for (int i = 0; i < 21; i++) {
    uint16_t w = ((uint16_t)chips[2 * i] << 8) | chips[2 * i + 1];
    if (invert) w = ~w;
    uint8_t v = 0;
    for (int k = 0; k < 8; k++) {
      uint8_t pair = (w >> (14 - 2 * k)) & 3;
      v <<= 1;
      if (pair == 2) v |= 1;           // "10" -> 1
      else if (pair == 1) v |= 0;      // "01" -> 0
      else { r.manchester_errs++; if (pair == 3) v |= 1; } // invalid pair: guess
    }
    raw[i] = v;
  }
  static const uint8_t tail[5] = {0x96, 0xF5, 0x13, 0x85, 0x37};
  for (int i = 0; i < 5; i++) r.sync_errs += popcount8(raw[i] ^ tail[i]);
  if (r.sync_errs > 6) { snprintf(r.fail, sizeof r.fail, "sync(%d)", r.sync_errs); return r; }
  uint8_t b[16];
  for (int i = 0; i < 16; i++) b[i] = (uint8_t)~raw[5 + i];   // rtl_433 inverts the bitbuffer
  uint64_t xr = ((uint64_t)b[11] << 32) | ((uint64_t)b[12] << 24) | ((uint64_t)b[13] << 16) | ((uint64_t)b[14] << 8) | b[15];
  uint64_t xc = checksum(b);
  if (xr != xc) {
    r.corrections = correct(b, xr ^ xc);
    if (r.corrections < 0) { snprintf(r.fail, sizeof r.fail, "checksum"); return r; }
  }
  r.leaking = (b[0] & 0x20) >> 5;
  r.serial = (uint32_t)b[1] | ((uint32_t)b[2] << 8) | ((uint32_t)b[3] << 16);
  r.suffix = b[4];
  r.flags1 = b[5];
  r.raw_counter = (uint32_t)b[6] | ((uint32_t)b[7] << 8) | ((uint32_t)b[8] << 16);
  r.flags2 = b[10];
  r.b9 = b[9];
  r.gear = 0.1f; r.unit = "m3";
  if (r.suffix == 0x00 && (r.flags1 == 0x00 || r.flags1 == 0x40)) { r.gear = 0.01f; r.unit = "m3"; }
  else if (r.suffix == 0x27 && r.flags1 == 0x00) { r.gear = 0.1f; r.unit = "gal"; }
  r.volume = (double)r.raw_counter * (r.gear < 0.05f ? 0.01 : 0.1);
  r.ok = true;
  return r;
}

} // namespace arad

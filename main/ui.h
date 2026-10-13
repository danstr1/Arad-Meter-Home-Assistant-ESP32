// 128x64 monochrome UI for the Arad receiver (SSD1306 page layout framebuffer).
// Pure C++ (no ESP-IDF deps) so it can be rendered on a PC for previews.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "fonts.h"

namespace ui {

static uint8_t fb[128 * 8];

static inline void clear() { memset(fb, 0, sizeof fb); }
static inline void px(int x, int y, int on = 1) {
  if (x < 0 || x > 127 || y < 0 || y > 63) return;
  if (on) fb[(y >> 3) * 128 + x] |= (1 << (y & 7)); else fb[(y >> 3) * 128 + x] &= ~(1 << (y & 7));
}
static inline void hline(int x0, int x1, int y, int on = 1) { for (int x = x0; x <= x1; x++) px(x, y, on); }
static inline void vline(int x, int y0, int y1, int on = 1) { for (int y = y0; y <= y1; y++) px(x, y, on); }
static inline void rect(int x, int y, int w, int h) { hline(x, x + w - 1, y); hline(x, x + w - 1, y + h - 1); vline(x, y, y + h - 1); vline(x + w - 1, y, y + h - 1); }
static inline void fill(int x, int y, int w, int h, int on = 1) { for (int j = y; j < y + h; j++) hline(x, x + w - 1, j, on); }
static inline void invert(int x, int y, int w, int h) {
  for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) {
    if (i < 0 || i > 127 || j < 0 || j > 63) continue;
    fb[(j >> 3) * 128 + i] ^= (1 << (j & 7));
  }
}

// draw text with baseline at y; returns advance width
static int text(const Font &f, int x, int y, const char *s, int on = 1) {
  int x0 = x;
  for (; *s; s++) {
    unsigned c = (unsigned char)*s;
    if (c < 32 || c > 126) c = '?';
    const Glyph &g = f.g[c - 32];
    int bpr = (g.w + 7) / 8;
    int top = y - (g.h + g.yo);
    for (int r = 0; r < g.h; r++)
      for (int col = 0; col < g.w; col++) {
        uint8_t b = f.bits[g.off + r * bpr + col / 8];
        if (b & (0x80 >> (col & 7))) px(x + g.xo + col, top + r, on);
      }
    x += g.adv;
  }
  return x - x0;
}
static int textw(const Font &f, const char *s) {
  int w = 0;
  for (; *s; s++) { unsigned c = (unsigned char)*s; if (c < 32 || c > 126) c = '?'; w += f.g[c - 32].adv; }
  return w;
}

// ---------- icons ----------
static void wifi_icon(int x, int y, bool ok) {   // 9x7, bottom-left at (x, y)
  if (!ok) { text(F_SMALL, x, y, "x"); return; }
  for (int i = 0; i < 3; i++) { px(x + 3 + i, y - 1); }
  px(x + 2, y - 2); px(x + 6, y - 2); hline(x + 3, x + 5, y - 3);
  px(x + 1, y - 4); px(x + 7, y - 4); hline(x + 2, x + 6, y - 5);
  px(x, y - 6); px(x + 8, y - 6); hline(x + 1, x + 7, y - 7);
}
static void signal_bars(int x, int y, float snr, bool fresh) {   // 4 bars, 11 px wide, bottom at y
  int n = !fresh ? 0 : snr > 30 ? 4 : snr > 20 ? 3 : snr > 12 ? 2 : snr > 6 ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;
    if (i < n) fill(x + i * 3, y - h + 1, 2, h); else { px(x + i * 3, y); px(x + i * 3 + 1, y); }
  }
}
static void drop(int cx, int top, int size, bool filled) {   // water drop, tip at top
  for (int r = 0; r < size * 2; r++) {
    float y = r;
    float half;
    if (r < size) half = (float)r * 0.55f; else { float t = (float)(r - size) / size; half = size * 0.55f * sqrtf(fmaxf(0.f, 1.f - t * t * 0.9f)) + 0.0f; if (r - size < 2) half = size * 0.55f; }
    int h = (int)(half + 0.5f);
    if (filled) hline(cx - h, cx + h, top + (int)y);
    else { px(cx - h, top + (int)y); px(cx + h, top + (int)y); }
  }
  if (!filled) hline(cx - (int)(size * 0.35f), cx + (int)(size * 0.35f), top + size * 2 - 1);
}

// ---------- data model ----------
struct MeterView {
  const char *name;
  bool seen;            // at least one packet
  float flow_lpm;       // current use, liters per minute
  float today_l;        // liters since local midnight (or since boot)
  double total_m3;      // meter register
  int age_s;            // seconds since last packet
  float snr;
  bool leaking;
  float hourly[24];     // liters per hour today
  int cur_hour;
};
struct Status {
  bool wifi, mqtt, time_ok;
  char clock[8];        // "12:05"
  int meters_heard;
};

static void fmt_short(char *b, int n, float l) {
  if (l < 1000) snprintf(b, n, "%.0fL", l);
  else if (l < 100000) snprintf(b, n, "%.2fm3", l / 1000.f);
  else snprintf(b, n, "%.0fm3", l / 1000.f);
}
static void fmt_liters(char *b, int n, float l) {
  if (l < 1000) snprintf(b, n, "%.0f L", l);
  else snprintf(b, n, "%.2f m3", l / 1000.f);
}

static void header(const char *title, const Status &st, int page, int pages, bool leak, int frame) {
  int tw = text(F_MED, 0, 10, title);
  int x = 128;
  if (pages > 8) {   // too many for dots: "3/12"
    char pg[24]; snprintf(pg, sizeof pg, "%d/%d", page + 1, pages);
    x -= textw(F_SMALL, pg); text(F_SMALL, x, 9, pg); x -= 2;
  } else if (pages > 1) {   // page dots: current page = 3x3 block
    for (int i = pages - 1; i >= 0; i--) { x -= 4; if (i == page) fill(x, 8, 3, 3); else px(x + 1, 9); }
  }
  x -= 11; wifi_icon(x, 9, st.wifi);
  if (st.mqtt) { x -= 4; fill(x, 3, 2, 2); }
  if (st.time_ok) { int w = textw(F_SMALL, st.clock); if (x - w - 3 > tw + 2) { x -= w + 3; text(F_SMALL, x, 9, st.clock); } }
  hline(0, 127, 12);
  if (leak && (frame / 4) % 2 == 0) invert(0, 0, 128, 12);
}

// hourly bar chart at bottom: 24 bars, 5 px pitch
static void hour_chart(const MeterView &m, int frame) {
  const int base = 63, maxh = 10, x0 = 4;
  float mx = 1;
  for (int i = 0; i < 24; i++) if (m.hourly[i] > mx) mx = m.hourly[i];
  for (int i = 0; i < 24; i++) {
    int x = x0 + i * 5;
    int h = m.hourly[i] > 0 ? 1 + (int)((maxh - 1) * m.hourly[i] / mx + 0.5f) : 0;
    if (i == m.cur_hour) {   // current hour: blinks between filled and outline
      int hh = h ? h : 2;
      if ((frame / 4) % 2 == 0) fill(x, base - hh + 1, 4, hh); else rect(x, base - hh + 1, 4, hh);
    } else if (h) {
      if (i < m.cur_hour) fill(x, base - h + 1, 4, h); else rect(x, base - h + 1, 4, h);
    } else px(x + 1, base);
  }
}

static void meter_page(const MeterView &m, const Status &st, int page, int pages, int frame) {
  clear();
  header(m.name, st, page, pages, m.leaking, frame);
  bool stale = !m.seen || m.age_s > 300;
  bool flowing = !stale && m.flow_lpm > 0.05f;

  // left: animated drop / idle drop
  if (flowing) {
    int period = m.flow_lpm > 10 ? 6 : m.flow_lpm > 3 ? 9 : 14;   // faster for more flow
    int ph = frame % period;
    int y = 14 + ph * 18 / period;
    drop(10, y, 5, true);
    if (ph >= period - 3) { hline(3, 17, 37); px(2, 36); px(18, 36); }   // splash
    // stream of small drops
    int y2 = 14 + ((ph + period / 2) % period) * 18 / period;
    px(10, y2); px(10, y2 + 1);
  } else {
    drop(10, 16, 6, false);
  }

  // big number = current use
  char b[24];
  if (stale) {
    text(F_SMALL, 24, 24, m.seen ? "no signal" : "waiting for");
    if (m.seen) { snprintf(b, sizeof b, "%d min", m.age_s / 60); text(F_SMALL, 24, 33, b); }
    else text(F_SMALL, 24, 33, "first packet");
  } else {
    snprintf(b, sizeof b, m.flow_lpm < 10 ? "%.1f" : "%.0f", m.flow_lpm);
    int w = textw(F_BIG, b);
    text(F_BIG, 92 - w, 37, b);
    text(F_SMALL, 95, 27, "L/min");
    text(F_SMALL, 95, 36, flowing ? "flow" : "idle");
  }
  // today
  text(F_SMALL, 0, 48, "Today");
  fmt_liters(b, sizeof b, m.today_l);
  text(F_MED, 32, 49, b);
  if (m.leaking) text(F_SMALL, 100, 48, "LEAK");
  else signal_bars(116, 48, m.snr, !stale);
  hour_chart(m, frame);
}

// list page: up to 4 meters (label, flow-now L/min, today)
static void overview_page(const MeterView *ms, int n, const Status &st, int page, int pages, int frame, int lp, int nl, int total) {
  clear();
  bool anyleak = false;
  for (int i = 0; i < n; i++) anyleak |= ms[i].leaking;
  char title[16];
  if (nl > 1) snprintf(title, sizeof title, "Meters %d", total); else snprintf(title, sizeof title, "Meters");
  (void)lp;
  header(title, st, page, pages, anyleak, frame);
  for (int i = 0; i < n && i < 4; i++) {
    const MeterView &m = ms[i];
    int y = 23 + i * 12;
    bool stale = !m.seen || m.age_s > 300;
    bool flowing = !stale && m.flow_lpm > 0.05f;
    char b[24];
    char nm[9]; snprintf(nm, sizeof nm, "%s", m.name);
    text(F_SMALL, 0, y, nm);
    if (flowing) {   // moving chevrons
      int o = frame % 4;
      for (int k = 0; k < 2; k++) { int cx = 44 + o + k * 5; px(cx, y - 5); px(cx + 1, y - 4); px(cx + 2, y - 3); px(cx + 1, y - 2); px(cx, y - 1); }
    }
    if (stale) snprintf(b, sizeof b, "--");
    else snprintf(b, sizeof b, "%.1f", m.flow_lpm);
    int w = textw(F_SMALL, b); text(F_SMALL, 82 - w, y, b);
    fmt_short(b, sizeof b, m.today_l);
    w = textw(F_SMALL, b); text(F_SMALL, 128 - w, y, b);
    if (m.leaking && (frame / 4) % 2 == 0) invert(0, y - 8, 128, 10);
  }
}

// shown until the first meter is decoded
static void waiting_page(const Status &st, bool wifi_set, const char *ip, int frame) {
  clear();
  header("Arad RX", st, 0, 1, false, frame);
  text(F_SMALL, 0, 24, "Listening for meters");
  int dots = (frame / 4) % 4;
  for (int i = 0; i < dots; i++) fill(102 + i * 4, 22, 2, 2);
  text(F_SMALL, 0, 34, "916.29 MHz FSK");
  if (!wifi_set) { text(F_SMALL, 0, 48, "Wi-Fi: not set up"); text(F_SMALL, 0, 58, "use USB + web page"); }
  else if (st.wifi) { char b[32]; snprintf(b, sizeof b, "IP %s", ip); text(F_SMALL, 0, 48, b); text(F_SMALL, 0, 58, st.mqtt ? "MQTT connected" : "MQTT not connected"); }
  else text(F_SMALL, 0, 48, "Wi-Fi connecting...");
}

// receiver info: web address, meters heard, Wi-Fi signal
static void info_page(const Status &st, int page, int pages, int frame, const char *ip, int wifi_rssi, int heard, int clients) {
  clear();
  header("Receiver", st, page, pages, false, frame);
  char b[40];
  text(F_SMALL, 0, 22, "Web: arad.local");
  if (ip && ip[0]) { snprintf(b, sizeof b, "     %s", ip); text(F_SMALL, 0, 31, b); }
  else text(F_SMALL, 0, 31, "     (no Wi-Fi)");
  snprintf(b, sizeof b, "Meters heard: %d", heard); text(F_SMALL, 0, 42, b);
  if (wifi_rssi) { snprintf(b, sizeof b, "Wi-Fi %d dBm", wifi_rssi); text(F_SMALL, 0, 52, b); }
  if (clients > 0) { snprintf(b, sizeof b, "%d page%s open", clients, clients > 1 ? "s" : ""); text(F_SMALL, 0, 62, b); }
}

static void splash(int frame, const char *line2) {
  clear();
  text(F_MED, (128 - textw(F_MED, "Arad Water RX")) / 2, 16, "Arad Water RX");
  // waves
  for (int x = 0; x < 128; x++) {
    float y = 40 + 4 * sinf((x + frame * 3) * 0.12f);
    px(x, (int)y); px(x, (int)y + 1);
    float y2 = 47 + 3 * sinf((x - frame * 2) * 0.17f + 1.3f);
    px(x, (int)y2);
  }
  int w = textw(F_SMALL, line2);
  text(F_SMALL, (128 - w) / 2, 62, line2);
}

}  // namespace ui

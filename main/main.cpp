// Arad Dialog3G / Master Meter water meter receiver for Heltec WiFi LoRa 32 V2 (SX1276) and V3 (SX1262).
// The radio runs in FSK packet mode at the Manchester chip rate (8.4 us chips, ~119 kBaud, +-77 kHz) with a 32-chip
// sync word (= raw bytes C1 96). Manchester decoding and the Arad checksum / 3-bit correction are done in software
// (ported from rtl_433 src/devices/arad_ms_meter.c, GPL-2.0-or-later).
// Output: JSON lines on the USB serial port (115200 baud) + OLED + MQTT with Home Assistant discovery.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "board.h"
#include "arad_decode.h"

#define FW_VERSION "3.2"

#define PAYLOAD_LEN 40              // chip bytes after the 32-chip sync (raw F5 13 85 37 + 16 bytes)
#define CHIP_RATE 119047.6          // 8.4 us Manchester chips

// ---------------- output ----------------
static char obuf[768];
static void web_broadcast(const char *s);
static void out_line(const char *s) {          // every output line goes to USB serial and to open web pages
  uart_write_bytes(UART_NUM_0, s, strlen(s));
  uart_write_bytes(UART_NUM_0, "\r\n", 2);
  web_broadcast(s);
}
#define OUT(...) do { snprintf(obuf, sizeof obuf, __VA_ARGS__); out_line(obuf); } while (0)
static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------- NVS ----------------
static nvs_handle_t nvs;
static bool nvs_has(const char *k) { int32_t v; return nvs_get_i32(nvs, k, &v) == ESP_OK; }
static int32_t nvs_i(const char *k, int32_t d) { int32_t v; return nvs_get_i32(nvs, k, &v) == ESP_OK ? v : d; }
static void nvs_seti(const char *k, int32_t v) { nvs_set_i32(nvs, k, v); nvs_commit(nvs); }
static void nvs_del(const char *k) { nvs_erase_key(nvs, k); nvs_commit(nvs); }

// ---------------- radio settings / counters ----------------
static float freq_mhz = 916.29f;
static int bw_khz = 0;
static int dev_khz = 77;
static int pol = -1, cur_pol = 0;   // pol: -1 = auto
static bool locked = false;
static int raw_mode = 0;
static uint32_t seq = 0, n_pre = 0, n_sync = 0, n_rx = 0, n_ok = 0, n_fail = 0;
static float noise = -200.0f, peak = -200.0f;

static const uint8_t SYNC_N[4] = {0xA5, 0x56, 0x96, 0x69};   // chips of raw C1 96, "10"=1 "01"=0
static void sync_bytes(uint8_t *s) { for (int i = 0; i < 4; i++) s[i] = cur_pol ? (uint8_t)~SYNC_N[i] : SYNC_N[i]; }

#if RADIO_SX1262
#include "radio_sx1262.h"
#else
#include "radio_sx1276.h"
#endif

#include "scale.h"
#include "app.h"
#include "web.h"

static void print_status() {
  OUT("{\"type\":\"status\",\"fw\":\"" FW_VERSION "\",\"board\":\"" BOARD_NAME "\",\"radio\":\"" RADIO_NAME "\",\"bw_options\":\"" RADIO_BW_LIST "\",\"uptime_s\":%lu,\"freq_mhz\":%.3f,\"bw_khz\":%d,\"pol\":%d,\"pol_mode\":\"%s\",\"noise\":%.1f,"
      "\"peak_rssi\":%.1f,\"preambles\":%lu,\"syncs\":%lu,\"captures\":%lu,\"decoded\":%lu,\"failed\":%lu,\"radio_errors\":%lu,"
      "\"wifi\":%d,\"mqtt\":%d,\"ip\":\"%s\",\"ssid\":\"%s\",\"mqtt_uri\":\"%s\",\"show\":\"%s\",\"tracked\":%d,\"named\":%d,\"tz\":\"%s\",\"heard\":%d,\"web_clients\":%d,\"scale_all\":%g}",
      (unsigned long)(millis() / 1000), freq_mhz, bw_khz, cur_pol, pol >= 0 ? "fixed" : (locked ? "auto-locked" : "auto-searching"),
      noise, peak, (unsigned long)n_pre, (unsigned long)n_sync, (unsigned long)n_rx, (unsigned long)n_ok, (unsigned long)n_fail,
      (unsigned long)radio_errors(), wifi_up, mqtt_up, ip_str, cfg_ssid, cfg_mqtt_uri, show_named ? "named" : "all", ntrk, count_named(), cfg_tz, nwh, (int)ws_clients, scale_global() / 1000.0);
}

static void handle_packet(const uint8_t *p, float rssi) {
  n_rx++;
  uint8_t chips[42];
  chips[0] = cur_pol ? (uint8_t)~0x96 : 0x96;   // re-insert chips of raw 0x96 (2nd sync byte) for the decoder
  chips[1] = cur_pol ? (uint8_t)~0x69 : 0x69;
  memcpy(chips + 2, p, PAYLOAD_LEN);
  AradResult r = arad::decode(chips, 42, cur_pol == 1);
  if (r.ok) {
    n_ok++;
    if (pol < 0 && !locked) { locked = true; nvs_seti("pol", cur_pol); }
    char id[16]; snprintf(id, sizeof id, "%08lu-%02x", (unsigned long)r.serial, r.suffix);
    double sc = scale_of(id);                          // per-meter reading scale (x1 / x0.1 / x0.01)
    r.volume *= sc;
    OUT("{\"model\":\"AradMsMeter-Dialog3G\",\"id\":\"%s\",\"leaking\":%d,\"flags1\":%u,\"gear\":%.3f,\"volume\":%.4f,\"scale\":%g,\"scale_own\":%d,"
        "\"unit\":\"%s\",\"flags2\":%u,\"b9\":%u,\"corrections\":%d,\"mic\":\"CHECKSUM\",\"rssi\":%.1f,\"snr\":%.1f,\"noise\":%.1f,"
        "\"manchester_errs\":%d,\"seq\":%lu,\"ts\":%lld}",
        id, r.leaking, r.flags1, r.gear, r.volume, sc, scale_own(id) ? 1 : 0, r.unit, r.flags2, r.b9, r.corrections,
        rssi, rssi - noise, noise, r.manchester_errs, (unsigned long)++seq, (long long)(time_ok() ? time(NULL) : 0));
    static char line[768]; strcpy(line, obuf);
    web_record(id, r, rssi, rssi - noise, seq);
    int ti = track_update(id, r.volume, rssi, rssi - noise, r.leaking);
    publish_packet(ti, id, line);
    gpio_set_level((gpio_num_t)P_LED, 1); vTaskDelay(pdMS_TO_TICKS(30)); gpio_set_level((gpio_num_t)P_LED, 0);
  } else {
    n_fail++;
    if (raw_mode) {
      char hex[2 * PAYLOAD_LEN + 1];
      for (int i = 0; i < PAYLOAD_LEN; i++) sprintf(hex + 2 * i, "%02x", p[i]);
      OUT("{\"type\":\"raw\",\"fail\":\"%s\",\"rssi\":%.1f,\"snr\":%.1f,\"pol\":%d,\"manchester_errs\":%d,\"hex\":\"%s\"}",
          r.fail, rssi, rssi - noise, cur_pol, r.manchester_errs, hex);
    }
  }
}

static const char *HELP =
  "{\"type\":\"info\",\"msg\":\"commands: wifi <ssid> | wifipass <password> | mqtt <mqtt://host:1883> | mqttuser <user> | mqttpass <password> | "
  "mqtt off | name <meter id> <label> | unname <meter id> | scale all|<meter id> 1|0.1|0.01|default | show all|named | tz <POSIX TZ> | meters | freq <MHz> | bw <kHz> | dev <kHz> | "
  "pol 0|1|auto | raw on|off | reboot | factory | status\"}";

static void handle_cmd(char *s) {
  while (*s == ' ') s++;
  int n = strlen(s); while (n && (s[n - 1] == ' ' || s[n - 1] == '\r')) s[--n] = 0;
  if (!n) return;
  if (extra_cmd(s)) { print_status(); return; }
  if (!strncmp(s, "freq ", 5)) { freq_mhz = atof(s + 5); nvs_seti("freq_khz", (int32_t)lroundf(freq_mhz * 1000)); radio_config(); }
  else if (!strncmp(s, "bw ", 3)) { bw_khz = atoi(s + 3); nvs_seti("bw", bw_khz); radio_config(); }
  else if (!strncmp(s, "dev ", 4)) { dev_khz = atoi(s + 4); nvs_seti("dev", dev_khz); radio_config(); }
  else if (!strcmp(s, "pol auto")) { pol = -1; locked = false; nvs_del("pol"); }
  else if (!strncmp(s, "pol ", 4)) { pol = atoi(s + 4) ? 1 : 0; cur_pol = pol; nvs_seti("pol", pol); radio_config(); }
  else if (!strcmp(s, "raw off")) raw_mode = 0;
  else if (!strcmp(s, "raw on")) raw_mode = 1;
#if CONFIG_ETH_USE_OPENETH
  else if (!strncmp(s, "fake ", 5)) {   // emulator test build only: fake <meter id> <m3>
    AradResult r; memset(&r, 0, sizeof r); r.ok = true; r.gear = 0.01f; r.unit = "m3";
    char id[16] = {0}; double v = 0; sscanf(s + 5, "%15s %lf", id, &v); v *= scale_of(id); r.volume = v;
    OUT("{\"model\":\"AradMsMeter-Dialog3G\",\"id\":\"%s\",\"leaking\":0,\"gear\":0.010,\"volume\":%.4f,\"scale\":%g,\"scale_own\":%d,\"unit\":\"m3\",\"corrections\":0,\"rssi\":-90.0,\"snr\":20.0,\"seq\":%lu,\"ts\":0}", id, v, scale_of(id), scale_own(id) ? 1 : 0, (unsigned long)++seq);
    static char line[768]; strcpy(line, obuf);
    web_record(id, r, -90, 20, seq); int ti = track_update(id, v, -90, 20, 0); publish_packet(ti, id, line);
  }
#endif
  else if (!strcmp(s, "reboot")) { OUT("{\"type\":\"info\",\"msg\":\"rebooting\"}"); vTaskDelay(pdMS_TO_TICKS(300)); esp_restart(); }
  else if (!strcmp(s, "factory")) { OUT("{\"type\":\"info\",\"msg\":\"erasing all settings and rebooting\"}"); nvs_erase_all(nvs); nvs_commit(nvs); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
  else if (strcmp(s, "status")) out_line(HELP);
  print_status();
}
static void poll_input() {
  static char line[160]; static int len = 0;
  uint8_t buf[32]; int n;
  while ((n = uart_read_bytes(UART_NUM_0, buf, sizeof buf, 0)) > 0)
    for (int i = 0; i < n; i++) {
      char c = (char)buf[i];
      if (c == '\n' || c == '\r') { if (len) { line[len] = 0; handle_cmd(line); len = 0; } }
      else if (len < (int)sizeof(line) - 1) line[len++] = c;
    }
  char wc[WEB_CMD_LEN];                       // commands from the web page
  while (web_cmdq && xQueueReceive(web_cmdq, wc, 0) == pdTRUE) handle_cmd(wc);
}

extern "C" void app_main(void) {
  uart_config_t uc = {}; uc.baud_rate = 115200; uc.data_bits = UART_DATA_8_BITS; uc.parity = UART_PARITY_DISABLE;
  uc.stop_bits = UART_STOP_BITS_1; uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE; uc.source_clk = UART_SCLK_DEFAULT;
  uart_driver_install(UART_NUM_0, 1024, 4096, 0, NULL, 0);
  uart_param_config(UART_NUM_0, &uc);
  if (nvs_flash_init() != ESP_OK) { nvs_flash_erase(); nvs_flash_init(); }
  nvs_open("arad", NVS_READWRITE, &nvs);

  gpio_reset_pin((gpio_num_t)P_LED); gpio_set_direction((gpio_num_t)P_LED, GPIO_MODE_OUTPUT); gpio_set_level((gpio_num_t)P_LED, 0);
#if !CONFIG_ETH_USE_OPENETH                    // (emulator test build has no radio)
  radio_bus_init();
#endif
  vTaskDelay(pdMS_TO_TICKS(300));
  OUT("{\"type\":\"info\",\"msg\":\"Arad water meter receiver fw " FW_VERSION " starting on " BOARD_NAME " (" RADIO_NAME ")\"}");

  freq_mhz = nvs_i("freq_khz", 916290) / 1000.0f;
  bw_khz = nvs_i("bw", radio_bw_default());
  dev_khz = nvs_i("dev", 77);
  if (nvs_has("pol")) { cur_pol = nvs_i("pol", 0); locked = true; }
#if !CONFIG_ETH_USE_OPENETH
  radio_config();
#endif
  web_init();
  app_init();
  print_status();

  uint32_t last_status = millis(), last_rssi = 0, last_pol = millis();
  uint8_t buf[PAYLOAD_LEN];
  while (true) {
    poll_input();
    float rssi;
#if CONFIG_ETH_USE_OPENETH
    vTaskDelay(1); continue;
#endif
    if (radio_poll(buf, &rssi)) { if (rssi > peak) peak = rssi; handle_packet(buf, rssi); }
    uint32_t now = millis();
    if (now - last_rssi > 50) {
      last_rssi = now;
      float r = radio_rssi_now();
      if (r > peak) peak = r;
      if (noise < -199.0f) noise = r;
      else if (r < noise + 6.0f) noise = noise * 0.97f + r * 0.03f;
    }
    if (pol < 0 && !locked && now - last_pol > 15000) { last_pol = now; cur_pol ^= 1; radio_config(); }
    if (now - last_status > 10000) { last_status = now; print_status(); peak = -200.0f; }
    vTaskDelay(1);
  }
}

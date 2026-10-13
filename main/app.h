// Meter tracking (flow, today, hourly), Wi-Fi, SNTP, MQTT + Home Assistant discovery, OLED pages, settings commands.
// All user settings live in NVS and are entered from the web page (Wi-Fi or USB) or the serial port (see README).
// Home Assistant gets only the meters you give a name; the screen and the web page show every meter heard.
#pragma once
#include <time.h>
#include <sys/time.h>
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "ui.h"
static void web_start();
static int nwh_count();
static int web_clients_count();

#define MAX_TRACK 24          // meters kept in memory / on screen / in Home Assistant
#define HIST_N 48
#define DEFAULT_TZ "IST-2IDT,M3.4.4/26,M10.5.0"   // Israel (Arad meters); change with: tz <POSIX TZ string>

struct Sample { uint32_t t; double v; };
struct Tracked {
  char id[16];
  char name[16];              // label shown on screen / in HA ("" = use meter id)
  bool named;                 // user gave it a name (kept across reboots, always shown)
  bool seen, announced;
  double vol;
  uint32_t last_ms;
  float rssi, snr;
  int leaking;
  float flow;                 // L/min, average over ~5 min
  Sample hist[HIST_N]; int hn, hhead;
  double day_base; int32_t day_key;   // yyyymmdd, 0 = unknown (no clock yet), <0 = restored from NVS
  int32_t pending_key;
  float hourly[24];
  uint32_t last_change_ms;
  float now_rate;             // "flow now": L/min between the last two packets (-1 = previous packet too old)
  float pkt_iv;               // typical time between this meter's packets (ms, running average)
};
static Tracked trk[MAX_TRACK];
static int ntrk = 0;
static SemaphoreHandle_t mtx;
static uint32_t last_day_save = 0;
static bool show_named = false;           // false = show every meter heard, true = only named meters

// settings (NVS)
static char cfg_ssid[33], cfg_wpass[65], cfg_mqtt_uri[96], cfg_mqtt_user[48], cfg_mqtt_pass[64], cfg_tz[64];

static void nvs_str(const char *k, char *out, size_t n, const char *def) {
  size_t len = n; if (nvs_get_str(nvs, k, out, &len) != ESP_OK) { strncpy(out, def, n - 1); out[n - 1] = 0; }
}
static void nvs_sets(const char *k, const char *v) { if (v && *v) nvs_set_str(nvs, k, v); else nvs_erase_key(nvs, k); nvs_commit(nvs); }

static bool time_ok() { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); return tm.tm_year + 1900 >= 2024; }
static int32_t today_key() {
  if (!time_ok()) return 0;
  time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
  return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}
static int cur_hour() {
  if (!time_ok()) return (int)((millis() / 3600000UL) % 24);
  time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); return tm.tm_hour;
}
static const char *label(const Tracked &t) { return t.name[0] ? t.name : t.id; }
static int count_named() { int n = 0; for (int i = 0; i < ntrk; i++) n += trk[i].named; return n; }
static int track_find(const char *id) { for (int i = 0; i < ntrk; i++) if (!strcmp(trk[i].id, id)) return i; return -1; }

// ---------------- persistence: names (n0..n23 = "<id> <label>") and today's baseline (blob d_<id8>)
static void names_save() {
  for (int i = 0; i < MAX_TRACK; i++) { char k[8]; snprintf(k, sizeof k, "n%d", i); nvs_erase_key(nvs, k); }
  int j = 0;
  for (int i = 0; i < ntrk; i++) if (trk[i].named) {
    char k[8], v[40]; snprintf(k, sizeof k, "n%d", j++); snprintf(v, sizeof v, "%s %s", trk[i].id, trk[i].name);
    nvs_set_str(nvs, k, v);
  }
  nvs_commit(nvs);
}
static void names_load() {
  ntrk = 0;
  for (int i = 0; i < MAX_TRACK; i++) {
    char k[8], v[40]; size_t len = sizeof v;
    snprintf(k, sizeof k, "n%d", i);
    if (nvs_get_str(nvs, k, v, &len) != ESP_OK) continue;
    char *sp = strchr(v, ' '); if (!sp) continue;
    *sp = 0;
    Tracked &t = trk[ntrk++]; memset(&t, 0, sizeof t);
    strncpy(t.id, v, sizeof t.id - 1); strncpy(t.name, sp + 1, sizeof t.name - 1); t.named = true;
  }
}
struct DayBlob { int32_t key; double base; float hourly[24]; };
static void day_key_name(const char *id, char *k) { snprintf(k, 12, "d_%.8s", id); }
static void day_save(int i) {
  DayBlob b; b.key = trk[i].day_key > 0 ? trk[i].day_key : 0; b.base = trk[i].day_base;
  memcpy(b.hourly, trk[i].hourly, sizeof b.hourly);
  char k[12]; day_key_name(trk[i].id, k);
  nvs_set_blob(nvs, k, &b, sizeof b); nvs_commit(nvs);
}
static void day_restore(Tracked &t) {
  DayBlob b; size_t len = sizeof b; char k[12]; day_key_name(t.id, k);
  if (nvs_get_blob(nvs, k, &b, &len) == ESP_OK && len == sizeof b && b.key > 0) {
    t.day_key = -b.key; t.day_base = b.base; memcpy(t.hourly, b.hourly, sizeof b.hourly);
  }
}
static void day_forget(const char *id) { char k[12]; day_key_name(id, k); nvs_erase_key(nvs, k); nvs_commit(nvs); }
// once the clock is known: delete saved baselines from previous days (meters that were not heard today)
static void day_cleanup() {
  int32_t key = today_key(); if (!key) return;
  char old[64][16]; int n = 0;
  nvs_iterator_t it = nullptr;
  esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, "arad", NVS_TYPE_BLOB, &it);
  while (e == ESP_OK && n < 64) {
    nvs_entry_info_t info; nvs_entry_info(it, &info);
    if (!strncmp(info.key, "d_", 2)) {
      DayBlob b; size_t len = sizeof b;
      if (nvs_get_blob(nvs, info.key, &b, &len) != ESP_OK || len != sizeof b || (b.key != key && b.key != 0)) strcpy(old[n++], info.key);
    }
    e = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  for (int i = 0; i < n; i++) nvs_erase_key(nvs, old[i]);
  if (n) nvs_commit(nvs);
}

// ---------------- flow
// "Flow now" = change between the meter's last two packets: liters / minutes between them.
// A packet with the same reading as the one before -> 0 at once. If the previous packet was more than 5 min older,
// the ~5-minute average is shown instead. No packet at all for 5 min -> 0.
static float flow_now(const Tracked &t, uint32_t now) {
  if (!t.seen || !t.last_ms || now - t.last_ms > 300000) return 0;
  return t.now_rate < 0 ? t.flow : t.now_rate;
}

// ---------------- MQTT / Home Assistant (declared early: used when meters are added / removed)
static bool wifi_up = false, mqtt_up = false, wifi_started = false;
static esp_mqtt_client_handle_t mqttc = nullptr;
static char mac_id[13];
static char ip_str[16] = "";
static void id_us(const char *id, char *out) { int j = 0; for (; id[j] && j < 15; j++) out[j] = id[j] == '-' ? '_' : id[j]; out[j] = 0; }
static void mqtt_pub(const char *topic, const char *payload, bool retain) {
  if (mqttc && mqtt_up) esp_mqtt_client_enqueue(mqttc, topic, payload, 0, 1, retain, true);
}
struct HaEnt { const char *comp, *key, *name, *extra; };
static const HaEnt HA_ENTS[] = {
  {"sensor", "total", "Water meter", "\"unit_of_meas\":\"m³\",\"dev_cla\":\"water\",\"stat_cla\":\"total_increasing\",\"val_tpl\":\"{{ value_json.volume }}\",\"sug_dsp_prc\":2"},
  {"sensor", "flow", "Water flow", "\"unit_of_meas\":\"L/min\",\"dev_cla\":\"volume_flow_rate\",\"stat_cla\":\"measurement\",\"val_tpl\":\"{{ value_json.flow_lpm }}\",\"ic\":\"mdi:water-pump\""},
  {"sensor", "flow_now", "Water flow now", "\"unit_of_meas\":\"L/min\",\"dev_cla\":\"volume_flow_rate\",\"stat_cla\":\"measurement\",\"val_tpl\":\"{{ value_json.flow_now }}\",\"ic\":\"mdi:water\""},
  {"sensor", "today", "Water today", "\"unit_of_meas\":\"L\",\"dev_cla\":\"water\",\"stat_cla\":\"total_increasing\",\"val_tpl\":\"{{ value_json.today_l }}\""},
  {"sensor", "rssi", "Signal", "\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",\"val_tpl\":\"{{ value_json.rssi }}\""},
  {"binary_sensor", "flowing", "Water flowing", "\"dev_cla\":\"running\",\"pl_on\":\"1\",\"pl_off\":\"0\",\"val_tpl\":\"{{ value_json.flowing }}\",\"ic\":\"mdi:water-alert\""},
  {"binary_sensor", "leak", "Leak", "\"dev_cla\":\"moisture\",\"pl_on\":\"1\",\"pl_off\":\"0\",\"val_tpl\":\"{{ value_json.leaking }}\""},
};
// caller holds mtx
static void ha_announce(Tracked &t) {
  if (!mqtt_up) return;
  static char topic[128], p[900];
  char u[16]; id_us(t.id, u);
  char dev[300];
  snprintf(dev, sizeof dev, "\"dev\":{\"ids\":[\"arad_%s\"],\"name\":\"Arad %s\",\"mdl\":\"Dialog3G water meter %s\",\"mf\":\"Arad\",\"via_device\":\"arad_rx_%s\"}",
           u, label(t), t.id, mac_id);
  for (auto &e : HA_ENTS) {
    snprintf(topic, sizeof topic, "homeassistant/%s/arad_%s/%s/config", e.comp, u, e.key);
    snprintf(p, sizeof p, "{\"name\":\"%s\",\"uniq_id\":\"arad_%s_%s\",\"obj_id\":\"arad_%s_%s\",\"stat_t\":\"arad/meter/%s/state\",\"avty_t\":\"arad/status\",%s,%s}",
             e.name, u, e.key, u, e.key, t.id, e.extra, dev);
    mqtt_pub(topic, p, true);
  }
  t.announced = true;
}
static void ha_remove(const char *id) {   // (only meters that were announced)
  if (!mqtt_up) return;
  char u[16], topic[128]; id_us(id, u);
  for (auto &e : HA_ENTS) { snprintf(topic, sizeof topic, "homeassistant/%s/arad_%s/%s/config", e.comp, u, e.key); mqtt_pub(topic, "", true); }
  snprintf(topic, sizeof topic, "arad/meter/%s/state", id); mqtt_pub(topic, "", true);
}
static void ha_receiver() {
  char topic[96], p[400];
  snprintf(topic, sizeof topic, "homeassistant/sensor/arad_rx_%s/meters/config", mac_id);
  snprintf(p, sizeof p, "{\"name\":\"Meters heard\",\"uniq_id\":\"arad_rx_%s_meters\",\"stat_t\":\"arad/receiver\",\"avty_t\":\"arad/status\",\"val_tpl\":\"{{ value_json.meters }}\","
           "\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:radio-tower\",\"dev\":{\"ids\":[\"arad_rx_%s\"],\"name\":\"Arad Water Receiver\",\"mdl\":\"" BOARD_NAME "\",\"mf\":\"DIY\",\"sw\":\"" FW_VERSION "\"}}",
           mac_id, mac_id);
  mqtt_pub(topic, p, true);
}

// ---------------- tracking
static void remove_at(int i) { for (int k = i; k < ntrk - 1; k++) trk[k] = trk[k + 1]; ntrk--; }

// find or add (caller holds mtx); returns -1 if the meter is not shown
static int track_get(const char *id) {
  int i = track_find(id);
  if (i >= 0) return i;
  if (show_named) return -1;
  if (ntrk >= MAX_TRACK) {            // full: replace the unnamed meter heard least recently
    int old = -1;
    for (int k = 0; k < ntrk; k++) if (!trk[k].named && (old < 0 || (int32_t)(trk[k].last_ms - trk[old].last_ms) < 0)) old = k;
    if (old < 0) return -1;
    day_forget(trk[old].id);
    remove_at(old);
  }
  i = ntrk++;
  memset(&trk[i], 0, sizeof trk[i]);
  strncpy(trk[i].id, id, sizeof trk[i].id - 1);
  return i;
}

static int track_update(const char *id, double vol, float rssi, float snr, int leaking) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  int i = track_get(id);
  if (i < 0) { xSemaphoreGive(mtx); return -1; }
  Tracked &t = trk[i];
  uint32_t now = millis();
  int32_t key = today_key();
  if (!t.seen) {
    t.seen = true;
    day_restore(t);
    bool restored = t.day_key < 0;
    int32_t rkey = restored ? -t.day_key : 0;
    if (key > 0) {
      if (restored && rkey == key) t.day_key = key;
      else { t.day_key = key; t.day_base = vol; memset(t.hourly, 0, sizeof t.hourly); }
    } else {
      if (restored) t.pending_key = rkey; else { t.day_base = vol; memset(t.hourly, 0, sizeof t.hourly); }
      t.day_key = 0;
    }
    t.vol = vol; t.last_change_ms = now;
    day_save(i);
  }
  if (key > 0 && t.day_key != key) {   // clock just became known, or midnight
    if (t.day_key == 0) {
      if (t.pending_key && t.pending_key != key) { t.day_base = t.vol; memset(t.hourly, 0, sizeof t.hourly); }
      t.day_key = key;
    } else { t.day_key = key; t.day_base = t.vol; memset(t.hourly, 0, sizeof t.hourly); }
    t.pending_key = 0;
    day_save(i);
  }
  double dv = vol - t.vol;
  if (dv > 0 && dv < 50) t.hourly[cur_hour()] += (float)(dv * 1000.0);
  if (dv != 0) t.last_change_ms = now;
  if (t.last_ms) {
    uint32_t dp = now - t.last_ms;
    if (dp >= 5000) {                  // flow now: change since the previous packet (ignore repeats < 5 s apart)
      if (dp <= 300000) t.now_rate = (dv > 0 && dv < 50) ? (float)(dv * 1000.0) / (dp / 60000.f) : 0;
      else t.now_rate = -1;
    }
    if (dp > 5000 && dp < 600000) t.pkt_iv = t.pkt_iv > 0 ? t.pkt_iv * 0.8f + dp * 0.2f : (float)dp;   // learn packet rate
  }
  t.vol = vol; t.last_ms = now; t.rssi = rssi; t.snr = snr; t.leaking = leaking;
  t.hist[t.hhead] = {now, vol}; t.hhead = (t.hhead + 1) % HIST_N; if (t.hn < HIST_N) t.hn++;
  const Sample *old = nullptr;          // flow over ~5 minutes
  for (int k = 0; k < t.hn; k++) {
    const Sample &s = t.hist[(t.hhead - 1 - k + 2 * HIST_N) % HIST_N];
    if (now - s.t >= 60000) { old = &s; if (now - s.t >= 300000) break; }
  }
  if (old) t.flow = (float)fmax(0.0, (vol - old->v) * 1000.0 / ((now - old->t) / 60000.0));
  if (now - t.last_change_ms > 600000) t.flow = 0;
  if (t.named && !t.announced) ha_announce(t);
  if (now - last_day_save > 1200000) { last_day_save = now; for (int k = 0; k < ntrk; k++) if (trk[k].seen) day_save(k); }
  xSemaphoreGive(mtx);
  return i;
}

// raw packet of every meter -> MQTT; state of named meters -> MQTT (Home Assistant); state of every shown meter -> web page
static void publish_packet(int ti, const char *id, const char *json_line) {
  char topic[48];
  snprintf(topic, sizeof topic, "arad/raw/%s", id);
  mqtt_pub(topic, json_line, false);
  if (ti < 0) return;
  char p[360];
  xSemaphoreTake(mtx, portMAX_DELAY);
  Tracked &t = trk[ti];
  float fn = flow_now(t, millis());
  bool named = t.named;
  snprintf(p, sizeof p, "{\"volume\":%.3f,\"flow_lpm\":%.1f,\"flow_now\":%.1f,\"flowing\":%d,\"today_l\":%.0f,\"rssi\":%.1f,\"snr\":%.1f,\"leaking\":%d,\"pkt_s\":%.0f,\"name\":\"%s\"}",
           t.vol, t.flow, fn, fn > 0 ? 1 : 0, (t.vol - t.day_base) * 1000.0, t.rssi, t.snr, t.leaking, t.pkt_iv / 1000.0f, t.name);
  xSemaphoreGive(mtx);
  if (named) { snprintf(topic, sizeof topic, "arad/meter/%s/state", id); mqtt_pub(topic, p, true); }
  OUT("{\"type\":\"meter\",\"id\":\"%s\",%s", id, p + 1);
  static uint32_t last_rx = 0;
  if (millis() - last_rx > 60000) { last_rx = millis(); char r[48]; snprintf(r, sizeof r, "{\"meters\":%d}", ntrk); mqtt_pub("arad/receiver", r, true); }
}

// ---------------- MQTT client
static void mqtt_event(void *, esp_event_base_t, int32_t id, void *) {
  if (id == MQTT_EVENT_CONNECTED) {
    mqtt_up = true;
    esp_mqtt_client_enqueue(mqttc, "arad/status", "online", 0, 1, 1, true);
    ha_receiver();
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < ntrk; i++) if (trk[i].named) ha_announce(trk[i]);
    xSemaphoreGive(mtx);
    OUT("{\"type\":\"info\",\"msg\":\"MQTT connected to %s, Home Assistant discovery sent for %d named meters\"}", cfg_mqtt_uri, count_named());
  } else if (id == MQTT_EVENT_DISCONNECTED) {
    if (mqtt_up) OUT("{\"type\":\"info\",\"msg\":\"MQTT disconnected\"}");
    mqtt_up = false;
  } else if (id == MQTT_EVENT_ERROR) {
    static uint32_t last = 0;
    if (millis() - last > 30000) { last = millis(); OUT("{\"type\":\"info\",\"msg\":\"MQTT connection to %s failed (check address, user, password)\"}", cfg_mqtt_uri); }
  }
}
static void mqtt_stop() {
  if (!mqttc) return;
  esp_mqtt_client_stop(mqttc); esp_mqtt_client_destroy(mqttc);
  mqttc = nullptr; mqtt_up = false;
}
static void mqtt_start() {
  if (mqttc || !cfg_mqtt_uri[0] || !wifi_up) return;
  esp_mqtt_client_config_t c = {};
  c.broker.address.uri = cfg_mqtt_uri;
  if (cfg_mqtt_user[0]) c.credentials.username = cfg_mqtt_user;
  if (cfg_mqtt_pass[0]) c.credentials.authentication.password = cfg_mqtt_pass;
  c.credentials.client_id = nullptr;
  c.session.last_will.topic = "arad/status";
  c.session.last_will.msg = "offline";
  c.session.last_will.retain = 1;
  c.session.last_will.qos = 1;
  c.network.reconnect_timeout_ms = 10000;
  mqttc = esp_mqtt_client_init(&c);
  if (!mqttc) { OUT("{\"type\":\"info\",\"msg\":\"MQTT address not valid: %s\"}", cfg_mqtt_uri); return; }
  esp_mqtt_client_register_event(mqttc, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event, nullptr);
  esp_mqtt_client_start(mqttc);
  OUT("{\"type\":\"info\",\"msg\":\"MQTT connecting to %s ...\"}", cfg_mqtt_uri);
}

// ---------------- Wi-Fi
static void wifi_event(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
  else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
    static uint32_t last = 0;
    if (wifi_up || millis() - last > 30000) { last = millis(); OUT("{\"type\":\"info\",\"msg\":\"Wi-Fi %s not connected (reason %d), retrying\"}", cfg_ssid, d ? d->reason : 0); }
    wifi_up = false; ip_str[0] = 0;
    if (cfg_ssid[0]) { vTaskDelay(pdMS_TO_TICKS(3000)); esp_wifi_connect(); }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    snprintf(ip_str, sizeof ip_str, IPSTR, IP2STR(&e->ip_info.ip));
    wifi_up = true;
    OUT("{\"type\":\"info\",\"msg\":\"Wi-Fi connected to %s, IP %s\"}", cfg_ssid, ip_str);
    static bool sntp = false;
    if (!sntp) { sntp = true; esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org"); esp_netif_sntp_init(&sc); }
    mqtt_start();
    web_start();
  }
}
#if CONFIG_ETH_USE_OPENETH
#include "esp_eth.h"
static void wifi_start() {   // emulator test build (QEMU): Ethernet instead of Wi-Fi
  esp_netif_init(); esp_event_loop_create_default();
  esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH(); esp_netif_t *en = esp_netif_new(&cfg);
  eth_mac_config_t mc = ETH_MAC_DEFAULT_CONFIG(); eth_phy_config_t pc = ETH_PHY_DEFAULT_CONFIG(); pc.autonego_timeout_ms = 100;
  esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mc); esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&pc);
  esp_eth_config_t ec = ETH_DEFAULT_CONFIG(mac, phy); esp_eth_handle_t h;
  esp_eth_driver_install(&ec, &h); esp_netif_attach(en, esp_eth_new_netif_glue(h));
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, [](void *, esp_event_base_t, int32_t, void *d) {
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)d; snprintf(ip_str, sizeof ip_str, IPSTR, IP2STR(&e->ip_info.ip)); wifi_up = true;
    OUT("{\"type\":\"info\",\"msg\":\"ETH IP %s\"}", ip_str); mqtt_start(); web_start(); }, nullptr);
  esp_eth_start(h);
}
#else
static void wifi_start() {
  if (!cfg_ssid[0]) { OUT("{\"type\":\"info\",\"msg\":\"Wi-Fi not set up. Use Settings on the web page (USB), or type: wifi <network name>  then  wifipass <password>\"}"); return; }
  if (!wifi_started) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *n = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(n, "arad-receiver");
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wc);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr);
    esp_wifi_set_mode(WIFI_MODE_STA);
  }
  wifi_config_t cfg = {};
  strncpy((char *)cfg.sta.ssid, cfg_ssid, sizeof cfg.sta.ssid);
  strncpy((char *)cfg.sta.password, cfg_wpass, sizeof cfg.sta.password);
  cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
  if (!wifi_started) { esp_wifi_start(); wifi_started = true; }
  else { esp_wifi_disconnect(); esp_wifi_connect(); }
  esp_wifi_set_ps(WIFI_PS_NONE);
  OUT("{\"type\":\"info\",\"msg\":\"Wi-Fi connecting to %s ...\"}", cfg_ssid);
}
#endif

// ---------------- OLED (SSD1306 128x64 on I2C)
static i2c_master_dev_handle_t oled;
static bool oled_ok = false;
static esp_err_t oled_cmds(const uint8_t *c, int n) { uint8_t b[32]; b[0] = 0x00; memcpy(b + 1, c, n); return i2c_master_transmit(oled, b, n + 1, 50); }
static void oled_init() {
  gpio_reset_pin((gpio_num_t)P_VEXT); gpio_set_direction((gpio_num_t)P_VEXT, GPIO_MODE_OUTPUT); gpio_set_level((gpio_num_t)P_VEXT, 0);
  vTaskDelay(pdMS_TO_TICKS(50));
  gpio_reset_pin((gpio_num_t)P_OLED_RST); gpio_set_direction((gpio_num_t)P_OLED_RST, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)P_OLED_RST, 0); vTaskDelay(pdMS_TO_TICKS(20)); gpio_set_level((gpio_num_t)P_OLED_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
  i2c_master_bus_config_t bc = {};
  bc.i2c_port = I2C_NUM_0; bc.sda_io_num = (gpio_num_t)P_SDA; bc.scl_io_num = (gpio_num_t)P_SCL;
  bc.clk_source = I2C_CLK_SRC_DEFAULT; bc.glitch_ignore_cnt = 7; bc.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t bus;
  if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return;
  i2c_device_config_t dc = {};
  dc.dev_addr_length = I2C_ADDR_BIT_LEN_7; dc.device_address = 0x3C; dc.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(bus, &dc, &oled) != ESP_OK) return;
  static const uint8_t init[] = {0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8,
                                 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF};
  esp_log_level_set("i2c.master", ESP_LOG_NONE);   // no screen -> stay quiet
  oled_ok = oled_cmds(init, sizeof init) == ESP_OK;
}
static void oled_flush() {
  if (!oled_ok) return;
  static const uint8_t win[] = {0x21, 0, 127, 0x22, 0, 7};
  oled_cmds(win, sizeof win);
  static uint8_t b[129];
  for (int i = 0; i < 1024; i += 128) { b[0] = 0x40; memcpy(b + 1, ui::fb + i, 128); i2c_master_transmit(oled, b, 129, 50); }
}

// Pages: one detail page per named meter, then list pages with every shown meter (4 per page).
static void display_task(void *) {
  oled_init();
  gpio_reset_pin((gpio_num_t)P_BTN); gpio_set_direction((gpio_num_t)P_BTN, GPIO_MODE_INPUT); gpio_set_pull_mode((gpio_num_t)P_BTN, GPIO_PULLUP_ONLY);
  int frame = 0, page = 0;
  uint32_t page_t = millis(), manual_until = 0;
  bool btn_prev = true, cleaned = false;
  static ui::MeterView mv[MAX_TRACK];
  static int order[MAX_TRACK];
  while (true) {
    uint32_t now = millis();
    ui::Status st = {};
    st.wifi = wifi_up; st.mqtt = mqtt_up; st.time_ok = time_ok();
    if (st.time_ok) {
      time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(st.clock, sizeof st.clock, "%H:%M", &tm);
      if (!cleaned) { cleaned = true; day_cleanup(); }
    }
    int n = 0, nd = 0;
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < ntrk; i++) order[n++] = i;
    for (int a = 1; a < n; a++) {               // named first, then by id
      int x = order[a], b = a - 1;
      while (b >= 0 && (trk[order[b]].named < trk[x].named ||
                        (trk[order[b]].named == trk[x].named && strcmp(trk[order[b]].id, trk[x].id) > 0))) { order[b + 1] = order[b]; b--; }
      order[b + 1] = x;
    }
    for (int k = 0; k < n; k++) {
      Tracked &t = trk[order[k]];
      ui::MeterView &m = mv[k];
      m.name = label(t); m.seen = t.seen; m.flow_lpm = flow_now(t, now);
      m.today_l = t.seen ? (float)((t.vol - t.day_base) * 1000.0) : 0;
      m.total_m3 = t.vol; m.age_s = t.seen ? (int)((now - t.last_ms) / 1000) : 0;
      m.snr = t.snr; m.leaking = t.leaking; memcpy(m.hourly, t.hourly, sizeof t.hourly);
      m.cur_hour = cur_hour();
      if (t.named) nd++;
    }
    xSemaphoreGive(mtx);
    int nl = (n + 3) / 4; if (nl < 1) nl = 1;
    int pages = nd + nl + 1;                    // detail pages, list pages, receiver info page
    bool btn = gpio_get_level((gpio_num_t)P_BTN);
    if (!btn && btn_prev) { page = (page + 1) % pages; page_t = now; manual_until = now + 60000; }
    btn_prev = btn;
    if (now > manual_until && now - page_t > 5000) { page = (page + 1) % pages; page_t = now; }
    if (page >= pages) page = 0;

    if (now < 3000) ui::splash(frame, "listening 916.29 MHz");
    else if (n == 0) ui::waiting_page(st, cfg_ssid[0] != 0, ip_str, frame);
    else if (page < nd) ui::meter_page(mv[page], st, page, pages, frame);
    else if (page < nd + nl) { int lp = page - nd; ui::overview_page(mv + lp * 4, n - lp * 4 > 4 ? 4 : n - lp * 4, st, page, pages, frame, lp + 1, nl, n); }
    else {
      int wr = 0;
#if !CONFIG_ETH_USE_OPENETH
      wifi_ap_record_t ap; if (wifi_up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) wr = ap.rssi;
#endif
      ui::info_page(st, page, pages, frame, wifi_up ? ip_str : "", wr, nwh_count(), web_clients_count());
    }
    oled_flush();
    frame++;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ---------------- commands
static void norm_uri(const char *in, char *out, size_t n) {
  if (strstr(in, "://")) snprintf(out, n, "%s", in); else snprintf(out, n, "mqtt://%s", in);
}
static bool extra_cmd(char *s) {
  if (!strncmp(s, "wifi ", 5) || !strncmp(s, "wifissid ", 9)) {
    const char *v = s + (s[4] == ' ' ? 5 : 9);
    if (!strcmp(v, "off")) { cfg_ssid[0] = cfg_wpass[0] = 0; nvs_sets("wifi_ssid", ""); nvs_sets("wifi_pass", ""); mqtt_stop(); if (wifi_started) esp_wifi_disconnect(); OUT("{\"type\":\"info\",\"msg\":\"Wi-Fi settings erased\"}"); return true; }
    snprintf(cfg_ssid, sizeof cfg_ssid, "%s", v); nvs_sets("wifi_ssid", cfg_ssid);
    OUT("{\"type\":\"info\",\"msg\":\"Wi-Fi network saved: %s\"}", cfg_ssid); wifi_start(); return true;
  }
  if (!strncmp(s, "wifipass", 8) && (s[8] == ' ' || !s[8])) {
    snprintf(cfg_wpass, sizeof cfg_wpass, "%s", s[8] ? s + 9 : ""); nvs_sets("wifi_pass", cfg_wpass);
    OUT("{\"type\":\"info\",\"msg\":\"Wi-Fi password saved\"}"); wifi_start(); return true;
  }
  if (!strcmp(s, "mqtt off")) { cfg_mqtt_uri[0] = 0; nvs_sets("mqtt_uri", ""); mqtt_stop(); OUT("{\"type\":\"info\",\"msg\":\"MQTT off\"}"); return true; }
  if (!strncmp(s, "mqtt ", 5)) {
    norm_uri(s + 5, cfg_mqtt_uri, sizeof cfg_mqtt_uri); nvs_sets("mqtt_uri", cfg_mqtt_uri);
    OUT("{\"type\":\"info\",\"msg\":\"MQTT broker saved: %s\"}", cfg_mqtt_uri); mqtt_stop(); mqtt_start(); return true;
  }
  if (!strncmp(s, "mqttuser", 8) && (s[8] == ' ' || !s[8])) {
    snprintf(cfg_mqtt_user, sizeof cfg_mqtt_user, "%s", s[8] ? s + 9 : ""); nvs_sets("mqtt_user", cfg_mqtt_user);
    OUT("{\"type\":\"info\",\"msg\":\"MQTT user saved\"}"); mqtt_stop(); mqtt_start(); return true;
  }
  if (!strncmp(s, "mqttpass", 8) && (s[8] == ' ' || !s[8])) {
    snprintf(cfg_mqtt_pass, sizeof cfg_mqtt_pass, "%s", s[8] ? s + 9 : ""); nvs_sets("mqtt_pass", cfg_mqtt_pass);
    OUT("{\"type\":\"info\",\"msg\":\"MQTT password saved\"}"); mqtt_stop(); mqtt_start(); return true;
  }
  if (!strncmp(s, "tz ", 3)) {
    snprintf(cfg_tz, sizeof cfg_tz, "%s", s + 3); nvs_sets("tz", cfg_tz); setenv("TZ", cfg_tz, 1); tzset();
    OUT("{\"type\":\"info\",\"msg\":\"time zone saved: %s\"}", cfg_tz); return true;
  }
  if (!strcmp(s, "show all") || !strcmp(s, "show named")) {
    show_named = s[5] == 'n'; nvs_seti("show_named", show_named);
    if (show_named) {
      xSemaphoreTake(mtx, portMAX_DELAY);
      for (int i = ntrk - 1; i >= 0; i--) if (!trk[i].named) remove_at(i);
      xSemaphoreGive(mtx);
    }
    OUT("{\"type\":\"info\",\"msg\":\"showing %s\"}", show_named ? "only named meters" : "every meter heard"); return true;
  }
  if (!strncmp(s, "name ", 5)) {   // name <id> <label>
    char *id = s + 5, *sp = strchr(id, ' ');
    if (!sp || !sp[1]) { OUT("{\"type\":\"info\",\"msg\":\"usage: name <meter id, e.g. 01234567-00> <label>\"}"); return true; }
    *sp = 0;
    xSemaphoreTake(mtx, portMAX_DELAY);
    int i = track_find(id);
    if (i < 0) {
      if (ntrk >= MAX_TRACK) { for (int k = ntrk - 1; k >= 0; k--) if (!trk[k].named) { remove_at(k); break; } }
      if (ntrk < MAX_TRACK) { i = ntrk++; memset(&trk[i], 0, sizeof trk[i]); strncpy(trk[i].id, id, 15); }
    }
    if (i >= 0) { strncpy(trk[i].name, sp + 1, 15); trk[i].name[15] = 0; trk[i].named = true; ha_announce(trk[i]); }
    xSemaphoreGive(mtx);
    names_save();
    if (i >= 0) OUT("{\"type\":\"info\",\"msg\":\"meter %s is now called %s\"}", id, sp + 1);
    else OUT("{\"type\":\"info\",\"msg\":\"too many named meters (max %d)\"}", MAX_TRACK);
    return true;
  }
  if (!strncmp(s, "unname ", 7)) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    int i = track_find(s + 7);
    if (i >= 0) {
      trk[i].named = false; trk[i].name[0] = 0;
      ha_remove(trk[i].id); trk[i].announced = false;     // unnamed meters are not in Home Assistant
      if (show_named) remove_at(i);
    }
    xSemaphoreGive(mtx);
    names_save();
    OUT("{\"type\":\"info\",\"msg\":\"%s %s\"}", s + 7, i >= 0 ? "name removed" : "not found");
    return true;
  }
  if (!strcmp(s, "meters")) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < ntrk; i++)
      OUT("{\"type\":\"meter\",\"id\":\"%s\",\"name\":\"%s\",\"seen\":%d,\"volume\":%.3f,\"flow_lpm\":%.1f,\"flow_now\":%.1f,\"today_l\":%.0f,\"rssi\":%.1f,\"age_s\":%lu}",
          trk[i].id, trk[i].name, trk[i].seen, trk[i].vol, trk[i].flow, flow_now(trk[i], millis()), trk[i].seen ? (trk[i].vol - trk[i].day_base) * 1000 : 0.0,
          trk[i].rssi, (unsigned long)(trk[i].seen ? (millis() - trk[i].last_ms) / 1000 : 0));
    xSemaphoreGive(mtx);
    return true;
  }
  return false;
}

static void app_init() {
  mtx = xSemaphoreCreateMutex();
  nvs_str("wifi_ssid", cfg_ssid, sizeof cfg_ssid, "");
  nvs_str("wifi_pass", cfg_wpass, sizeof cfg_wpass, "");
  nvs_str("mqtt_uri", cfg_mqtt_uri, sizeof cfg_mqtt_uri, "");
  nvs_str("mqtt_user", cfg_mqtt_user, sizeof cfg_mqtt_user, "");
  nvs_str("mqtt_pass", cfg_mqtt_pass, sizeof cfg_mqtt_pass, "");
  nvs_str("tz", cfg_tz, sizeof cfg_tz, DEFAULT_TZ);
  show_named = nvs_i("show_named", 0) != 0;
  setenv("TZ", cfg_tz, 1); tzset();
  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(mac_id, sizeof mac_id, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  names_load();
  xTaskCreate(display_task, "oled", 6144, nullptr, 1, nullptr);
  wifi_start();
  if (!cfg_mqtt_uri[0]) OUT("{\"type\":\"info\",\"msg\":\"MQTT not set up (optional). Use the Settings panel, or type: mqtt <broker address>\"}");
}

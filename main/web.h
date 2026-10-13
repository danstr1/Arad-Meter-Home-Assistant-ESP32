// Built-in web page over Wi-Fi: http://arad.local/ (or the board's IP).
// The page talks to the board through a WebSocket (/ws) that carries exactly the same JSON lines as the USB serial port,
// and sends commands back (same commands as typed on the serial port). The board also keeps a short history of every
// meter heard since boot and replays it to each new page, so the meter table is full as soon as the page opens.
#pragma once
#include "esp_http_server.h"
#include "mdns.h"
#include "freertos/queue.h"

extern const uint8_t page_gz_start[] asm("_binary_arad_page_html_gz_start");
extern const uint8_t page_gz_end[] asm("_binary_arad_page_html_gz_end");

static httpd_handle_t web = nullptr;
static QueueHandle_t web_cmdq = nullptr;
#define WEB_CMD_LEN 160

// ---------------- history of all meters heard (RAM, since boot)
#define WH_MAX 64
#define WH_CH 24
struct WPkt { uint32_t ms, seq; double vol; float rssi, snr; uint8_t leak, corr; };
struct WMeter { char id[16]; char unit[4]; float gear; WPkt last; WPkt ch[WH_CH]; int n, head; };
static WMeter wh[WH_MAX];
static int nwh = 0;
static SemaphoreHandle_t wmtx = nullptr;

static void web_record(const char *id, const AradResult &r, float rssi, float snr, uint32_t sq) {
  if (!wmtx) return;
  xSemaphoreTake(wmtx, portMAX_DELAY);
  int i = 0;
  for (; i < nwh; i++) if (!strcmp(wh[i].id, id)) break;
  if (i == nwh) {
    if (nwh < WH_MAX) nwh++;
    else { i = 0; for (int k = 1; k < nwh; k++) if ((int32_t)(wh[k].last.ms - wh[i].last.ms) < 0) i = k; }   // replace oldest
    memset(&wh[i], 0, sizeof wh[i]);
    strncpy(wh[i].id, id, sizeof wh[i].id - 1);
  }
  WMeter &m = wh[i];
  strncpy(m.unit, r.unit, sizeof m.unit - 1); m.gear = r.gear;
  WPkt p = {millis(), sq, r.volume, rssi, snr, (uint8_t)r.leaking, (uint8_t)(r.corrections < 0 ? 0 : r.corrections)};
  bool changed = m.n == 0 || fabs(m.ch[(m.head + WH_CH - 1) % WH_CH].vol - r.volume) > 1e-6;
  if (changed) { m.ch[m.head] = p; m.head = (m.head + 1) % WH_CH; if (m.n < WH_CH) m.n++; }
  m.last = p;
  xSemaphoreGive(wmtx);
}
static int64_t epoch_of(uint32_t ms) {
  if (!time_ok()) return 0;
  return (int64_t)time(NULL) - (int64_t)(millis() - ms) / 1000;
}
static int fmt_pkt(char *b, int n, const WMeter &m, const WPkt &p) {
  return snprintf(b, n, "{\"model\":\"AradMsMeter-Dialog3G\",\"id\":\"%s\",\"leaking\":%u,\"gear\":%.3f,\"volume\":%.4f,\"unit\":\"%s\",\"corrections\":%u,"
                  "\"rssi\":%.1f,\"snr\":%.1f,\"seq\":%lu,\"ts\":%lld,\"age_s\":%lu,\"scale\":%g,\"scale_own\":%d,\"replay\":1}",
                  m.id, p.leak, m.gear, p.vol, m.unit, p.corr, p.rssi, p.snr, (unsigned long)p.seq, (long long)epoch_of(p.ms),
                  (unsigned long)((millis() - p.ms) / 1000), scale_of(m.id), scale_own(m.id) ? 1 : 0);
}

// ---------------- WebSocket
static volatile int ws_clients = 0;
static void ws_send_fd(int fd, const char *s) {
  httpd_ws_frame_t f = {};
  f.type = HTTPD_WS_TYPE_TEXT; f.payload = (uint8_t *)s; f.len = strlen(s); f.final = true;
  httpd_ws_send_frame_async(web, fd, &f);
}
static void replay_work(void *arg) {   // runs in the httpd task
  int fd = (int)(intptr_t)arg;
  static char line[320];
  xSemaphoreTake(wmtx, portMAX_DELAY);
  for (int i = 0; i < nwh; i++) {
    WMeter &m = wh[i];
    for (int k = 0; k < m.n; k++) {
      const WPkt &p = m.ch[(m.head - m.n + k + WH_CH) % WH_CH];
      fmt_pkt(line, sizeof line, m, p); ws_send_fd(fd, line);
    }
    if (m.n == 0 || m.last.seq != m.ch[(m.head + WH_CH - 1) % WH_CH].seq) { fmt_pkt(line, sizeof line, m, m.last); ws_send_fd(fd, line); }
  }
  xSemaphoreGive(wmtx);
}
static void bcast_work(void *arg) {    // runs in the httpd task
  char *s = (char *)arg;
  int fds[CONFIG_LWIP_MAX_SOCKETS]; size_t n = CONFIG_LWIP_MAX_SOCKETS; int cnt = 0;
  if (httpd_get_client_list(web, &n, fds) == ESP_OK)
    for (size_t i = 0; i < n; i++)
      if (httpd_ws_get_fd_info(web, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) { ws_send_fd(fds[i], s); cnt++; }
  ws_clients = cnt;   // recount: closed pages drop out here
  free(s);
}
static void web_broadcast(const char *s) {
  if (!web || ws_clients <= 0) return;
  char *c = strdup(s);
  if (c && httpd_queue_work(web, bcast_work, c) != ESP_OK) free(c);
}
static void web_push_cmd(const char *s) {
  if (!web_cmdq) return;
  char b[WEB_CMD_LEN] = {0}; strncpy(b, s, WEB_CMD_LEN - 1);
  xQueueSend(web_cmdq, b, 0);
}
static esp_err_t ws_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {           // handshake
    ws_clients = ws_clients + 1;
    httpd_queue_work(web, replay_work, (void *)(intptr_t)httpd_req_to_sockfd(req));
    web_push_cmd("status"); web_push_cmd("meters");
    return ESP_OK;
  }
  httpd_ws_frame_t f = {}; f.type = HTTPD_WS_TYPE_TEXT;
  if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_FAIL;
  if (f.type == HTTPD_WS_TYPE_CLOSE) return ESP_OK;
  if (f.len == 0 || f.len >= WEB_CMD_LEN) return ESP_OK;
  char b[WEB_CMD_LEN] = {0}; f.payload = (uint8_t *)b;
  if (httpd_ws_recv_frame(req, &f, f.len) != ESP_OK) return ESP_FAIL;
  if (f.type == HTTPD_WS_TYPE_TEXT) web_push_cmd(b);
  return ESP_OK;
}
// ---------------- HTTP
static esp_err_t page_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, (const char *)page_gz_start, page_gz_end - page_gz_start);
}
// plain JSON list of every meter heard: http://arad.local/api/meters
static esp_err_t api_meters_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  static char line[320];
  httpd_resp_send_chunk(req, "[", 1);
  xSemaphoreTake(wmtx, portMAX_DELAY);
  for (int i = 0; i < nwh; i++) {
    int n = fmt_pkt(line, sizeof line, wh[i], wh[i].last);
    if (i) httpd_resp_send_chunk(req, ",", 1);
    httpd_resp_send_chunk(req, line, n);
  }
  xSemaphoreGive(wmtx);
  httpd_resp_send_chunk(req, "]", 1);
  return httpd_resp_send_chunk(req, nullptr, 0);
}

static void web_start() {
  if (web) return;
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.stack_size = 6144;
  c.max_open_sockets = 6;
  c.lru_purge_enable = true;
  if (httpd_start(&web, &c) != ESP_OK) { web = nullptr; OUT("{\"type\":\"info\",\"msg\":\"web server failed to start\"}"); return; }
  httpd_uri_t u1 = {}; u1.uri = "/"; u1.method = HTTP_GET; u1.handler = page_handler; httpd_register_uri_handler(web, &u1);
  httpd_uri_t u2 = {}; u2.uri = "/ws"; u2.method = HTTP_GET; u2.handler = ws_handler; u2.is_websocket = true; httpd_register_uri_handler(web, &u2);
  httpd_uri_t u4 = {}; u4.uri = "/favicon.ico"; u4.method = HTTP_GET;
  u4.handler = [](httpd_req_t *r) { httpd_resp_set_status(r, "204 No Content"); return httpd_resp_send(r, nullptr, 0); };
  httpd_register_uri_handler(web, &u4);
  httpd_uri_t u3 = {}; u3.uri = "/api/meters"; u3.method = HTTP_GET; u3.handler = api_meters_handler; httpd_register_uri_handler(web, &u3);
  if (mdns_init() == ESP_OK) {
    mdns_hostname_set("arad");
    mdns_instance_name_set("Arad Water Receiver");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
  }
  OUT("{\"type\":\"info\",\"msg\":\"web page: http://arad.local/  or  http://%s/\"}", ip_str);
}
// reading scale of a meter changed: convert its stored history
static void web_rescale(const char *id, double ratio) {
  xSemaphoreTake(wmtx, portMAX_DELAY);
  for (int i = 0; i < nwh; i++) if (!strcmp(wh[i].id, id)) {
    for (int k = 0; k < WH_CH; k++) wh[i].ch[k].vol *= ratio;
    wh[i].last.vol *= ratio;
  }
  xSemaphoreGive(wmtx);
}
// scale all <f> | scale <meter id> <f> | scale <meter id> default
static bool scale_cmd(const char *args) {
  char a[16] = {0}, b[16] = {0};
  if (sscanf(args, "%15s %15s", a, b) != 2) { OUT("{\"type\":\"info\",\"msg\":\"usage: scale all <1|0.1|0.01|0.001>  or  scale <meter id> <1|0.1|0.01|0.001|default>\"}"); return true; }
  if (!strcmp(a, "all")) {
    int32_t nm = scale_factor_parse(b), om = scale_global();
    if (!nm) { OUT("{\"type\":\"info\",\"msg\":\"scale: use 1, 0.1, 0.01 or 0.001\"}"); return true; }
    scale_set_global(nm);
    if (nm != om) {
      double ratio = (double)nm / om;
      for (int i = 0; i < ntrk; i++) if (!scale_own(trk[i].id)) track_rescale(trk[i].id, ratio);   // meters that follow the default
      char ids[WH_MAX][16]; int n = 0;
      xSemaphoreTake(wmtx, portMAX_DELAY);
      for (int i = 0; i < nwh; i++) strcpy(ids[n++], wh[i].id);
      xSemaphoreGive(wmtx);
      for (int i = 0; i < n; i++) if (!scale_own(ids[i])) {
        web_rescale(ids[i], ratio);
        OUT("{\"type\":\"scale\",\"id\":\"%s\",\"scale\":%g,\"scale_own\":0}", ids[i], nm / 1000.0);
      }
    }
    OUT("{\"type\":\"info\",\"msg\":\"reading scale for all meters: x%g (meters with their own scale keep it)\"}", nm / 1000.0);
    return true;
  }
  int32_t own = strcmp(b, "default") ? scale_factor_parse(b) : 0;
  if (own == 0 && strcmp(b, "default")) { OUT("{\"type\":\"info\",\"msg\":\"scale: use 1, 0.1, 0.01, 0.001 or default\"}"); return true; }
  int32_t om = scale_milli(a);
  scale_set_own(a, own);
  int32_t nm = scale_milli(a);
  if (nm != om) { double ratio = (double)nm / om; track_rescale(a, ratio); web_rescale(a, ratio); }
  OUT("{\"type\":\"scale\",\"id\":\"%s\",\"scale\":%g,\"scale_own\":%d}", a, nm / 1000.0, own ? 1 : 0);
  OUT("{\"type\":\"info\",\"msg\":\"meter %s: reading scale x%g%s\"}", a, nm / 1000.0, own ? "" : " (same as all meters)");
  return true;
}
static int nwh_count() { return nwh; }
static int web_clients_count() { return ws_clients; }
static void web_init() {
  wmtx = xSemaphoreCreateMutex();
  web_cmdq = xQueueCreate(8, WEB_CMD_LEN);
}

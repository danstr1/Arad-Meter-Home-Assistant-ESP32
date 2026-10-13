// SX1262 (Heltec V3) in GFSK mode at the Arad Manchester chip rate.
#pragma once
#define RADIO_NAME "SX1262"

static spi_device_handle_t spi;
static uint32_t busy_timeouts = 0;

static void wait_busy() {
  int64_t t = esp_timer_get_time();
  while (gpio_get_level((gpio_num_t)P_BUSY)) {
    if (esp_timer_get_time() - t > 100000) { busy_timeouts++; return; }
  }
}
static void sx_xfer(const uint8_t *tx, uint8_t *rx, int n) {
  wait_busy();
  spi_transaction_t t = {};
  t.length = n * 8; t.tx_buffer = tx; t.rx_buffer = rx;
  spi_device_polling_transmit(spi, &t);
}
static void sx_cmd(uint8_t op, const uint8_t *args, int n) {
  uint8_t tx[32], rx[32];
  tx[0] = op; if (n) memcpy(tx + 1, args, n);
  sx_xfer(tx, rx, n + 1);
}
static void sx_cmd1(uint8_t op, uint8_t a) { sx_cmd(op, &a, 1); }
static void sx_read(uint8_t op, uint8_t *out, int n) {
  uint8_t tx[32] = {0}, rx[32] = {0};
  tx[0] = op; sx_xfer(tx, rx, n + 2); memcpy(out, rx + 2, n);
}
static void sx_write_reg(uint16_t addr, const uint8_t *d, int n) {
  uint8_t tx[32], rx[32];
  tx[0] = 0x0D; tx[1] = addr >> 8; tx[2] = addr & 0xFF; memcpy(tx + 3, d, n);
  sx_xfer(tx, rx, n + 3);
}
static void sx_read_buf(uint8_t off, uint8_t *d, int n) {
  uint8_t tx[80] = {0}, rx[80] = {0};
  tx[0] = 0x1E; tx[1] = off; sx_xfer(tx, rx, n + 3); memcpy(d, rx + 3, n);
}
static uint16_t sx_irq() { uint8_t b[2]; sx_read(0x12, b, 2); return (b[0] << 8) | b[1]; }
static void sx_clear_irq(uint16_t m) { uint8_t a[2] = {(uint8_t)(m >> 8), (uint8_t)m}; sx_cmd(0x02, a, 2); }
static uint16_t sx_errors() { uint8_t b[2]; sx_read(0x17, b, 2); return (b[0] << 8) | b[1]; }

static float radio_rssi_now() { uint8_t b; sx_read(0x15, &b, 1); return -b / 2.0f; }
static uint32_t radio_errors() { return busy_timeouts; }
static int radio_bw_default() { return 373; }
#define RADIO_BW_LIST "467|373|312|234|187|156"

static uint8_t bw_code(int khz) {
  if (khz >= 467) return 0x09;
  if (khz >= 373) return 0x11;
  if (khz >= 312) return 0x19;
  if (khz >= 234) return 0x0A;
  if (khz >= 187) return 0x12;
  return 0x1A;
}

static void radio_bus_init() {
  gpio_reset_pin((gpio_num_t)P_RST); gpio_set_direction((gpio_num_t)P_RST, GPIO_MODE_OUTPUT); gpio_set_level((gpio_num_t)P_RST, 1);
  gpio_reset_pin((gpio_num_t)P_BUSY); gpio_set_direction((gpio_num_t)P_BUSY, GPIO_MODE_INPUT);
  spi_bus_config_t bus = {};
  bus.mosi_io_num = P_MOSI; bus.miso_io_num = P_MISO; bus.sclk_io_num = P_SCK;
  bus.quadwp_io_num = -1; bus.quadhd_io_num = -1; bus.max_transfer_sz = 128;
  spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = 2000000; dev.mode = 0; dev.spics_io_num = P_NSS; dev.queue_size = 1;
  spi_bus_add_device(SPI2_HOST, &dev, &spi);
}

static void radio_config() {
  gpio_set_level((gpio_num_t)P_RST, 0); vTaskDelay(pdMS_TO_TICKS(2));
  gpio_set_level((gpio_num_t)P_RST, 1); vTaskDelay(pdMS_TO_TICKS(10));
  wait_busy();
  sx_cmd1(0x80, 0x00);                                                // SetStandby(STDBY_RC)
  { uint8_t a[4] = {0x02, 0x00, 0x01, 0x40}; sx_cmd(0x97, a, 4); }    // DIO3 -> TCXO 1.8 V
  { uint8_t a[2] = {0, 0}; sx_cmd(0x07, a, 2); }                     // ClearDeviceErrors
  sx_cmd1(0x89, 0x7F); vTaskDelay(pdMS_TO_TICKS(10));                 // Calibrate all
  sx_cmd1(0x9D, 0x01);                                                // DIO2 = RF switch
  sx_cmd1(0x96, 0x01);                                                // DC-DC regulator
  sx_cmd1(0x8A, 0x00);                                                // packet type GFSK
  { uint32_t f = (uint32_t)llround((double)freq_mhz * 1e6 * 33554432.0 / 32e6);
    uint8_t a[4] = {(uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f}; sx_cmd(0x86, a, 4); }
  { uint8_t a[2] = {0xE1, 0xE9}; sx_cmd(0x98, a, 2); vTaskDelay(pdMS_TO_TICKS(5)); }   // image cal 902-928 MHz
  { uint32_t br = (uint32_t)llround(32.0 * 32e6 / CHIP_RATE);
    uint32_t fd = (uint32_t)llround(dev_khz * 1000.0 * 33554432.0 / 32e6);
    uint8_t a[8] = {(uint8_t)(br >> 16), (uint8_t)(br >> 8), (uint8_t)br, 0x00, bw_code(bw_khz),
                    (uint8_t)(fd >> 16), (uint8_t)(fd >> 8), (uint8_t)fd};
    sx_cmd(0x8B, a, 8); }
  { uint8_t a[9] = {0x00, 0x20, 0x05, 32, 0x00, 0x00, PAYLOAD_LEN, 0x01, 0x00};   // 16-bit preamble det., 32-bit sync, fixed len, no CRC
    sx_cmd(0x8C, a, 9); }
  { uint8_t s[4]; sync_bytes(s); sx_write_reg(0x06C0, s, 4); }
  { uint8_t g = 0x96; sx_write_reg(0x08AC, &g, 1); }                 // RX boosted gain
  { uint8_t a[2] = {0x00, 0x00}; sx_cmd(0x8F, a, 2); }
  { uint8_t a[8] = {0x02, 0x0E, 0x02, 0x0E, 0, 0, 0, 0}; sx_cmd(0x08, a, 8); }   // IRQ: RxDone|Preamble|SyncValid|Timeout
  sx_clear_irq(0xFFFF);
  { uint8_t a[3] = {0xFF, 0xFF, 0xFF}; sx_cmd(0x82, a, 3); }         // RX continuous
  uint16_t err = sx_errors();
  OUT("{\"type\":\"info\",\"msg\":\"SX1262 configured: %.3f MHz, bw %d kHz, dev %d kHz, pol %d, device errors 0x%04x%s\"}",
      freq_mhz, bw_khz, dev_khz, cur_pol, err, busy_timeouts ? " - radio BUSY timeouts, check board" : "");
}

// returns true and fills buf (PAYLOAD_LEN bytes) when a packet was received
static bool radio_poll(uint8_t *buf, float *rssi) {
  uint16_t irq = sx_irq();
  if (irq & 0x0004) n_pre++;
  if (irq & 0x0008) n_sync++;
  if (irq & 0x0002) {
    uint8_t st[2]; sx_read(0x13, st, 2);
    uint8_t ps[3]; sx_read(0x14, ps, 3);
    int len = st[0] > PAYLOAD_LEN ? PAYLOAD_LEN : st[0];
    memset(buf, 0, PAYLOAD_LEN);
    sx_read_buf(st[1], buf, len);
    sx_clear_irq(0xFFFF);
    *rssi = -ps[1] / 2.0f;
    return len == PAYLOAD_LEN;
  }
  if (irq) sx_clear_irq(irq);
  return false;
}

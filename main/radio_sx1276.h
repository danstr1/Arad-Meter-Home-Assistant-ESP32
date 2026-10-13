// SX1276 (Heltec V2) in FSK packet mode at the Arad Manchester chip rate.
// FXOSC 32 MHz, Fstep 61.035 Hz. Fixed max LNA gain (no AGC), 32-bit sync word, fixed 40-byte payload, CRC off.
#pragma once
#define RADIO_NAME "SX1276"

static spi_device_handle_t spi;
static uint32_t radio_errs = 0;
static uint8_t radio_version = 0;

static void wr(uint8_t a, uint8_t v) {
  uint8_t tx[2] = {(uint8_t)(a | 0x80), v}, rx[2];
  spi_transaction_t t = {}; t.length = 16; t.tx_buffer = tx; t.rx_buffer = rx;
  spi_device_polling_transmit(spi, &t);
}
static uint8_t rd(uint8_t a) {
  uint8_t tx[2] = {(uint8_t)(a & 0x7F), 0}, rx[2] = {0, 0};
  spi_transaction_t t = {}; t.length = 16; t.tx_buffer = tx; t.rx_buffer = rx;
  spi_device_polling_transmit(spi, &t);
  return rx[1];
}
static void rd_fifo(uint8_t *d, int n) {
  uint8_t tx[1 + PAYLOAD_LEN] = {0}, rx[1 + PAYLOAD_LEN] = {0};
  spi_transaction_t t = {}; t.length = (n + 1) * 8; t.tx_buffer = tx; t.rx_buffer = rx;
  spi_device_polling_transmit(spi, &t);
  memcpy(d, rx + 1, n);
}

static float radio_rssi_now() { return -rd(0x11) / 2.0f; }
static uint32_t radio_errors() { return radio_errs; }
static int radio_bw_default() { return 200; }
#define RADIO_BW_LIST "250|200|166|125"

static uint8_t bw_code(int khz) {        // single-side RX bandwidth = 32 MHz / (mant * 2^(exp+2))
  if (khz >= 250) return 0x01;           // mant 16, exp 1
  if (khz >= 200) return 0x09;           // mant 20, exp 1
  if (khz >= 166) return 0x11;           // mant 24, exp 1
  return 0x02;                           // 125 kHz
}
static const uint8_t RXCFG = 0x00;       // AGC off (fixed gain), no RX trigger

static void radio_bus_init() {
  gpio_reset_pin((gpio_num_t)P_RST); gpio_set_direction((gpio_num_t)P_RST, GPIO_MODE_OUTPUT); gpio_set_level((gpio_num_t)P_RST, 1);
  gpio_reset_pin((gpio_num_t)P_DIO0); gpio_set_direction((gpio_num_t)P_DIO0, GPIO_MODE_INPUT);
  spi_bus_config_t bus = {};
  bus.mosi_io_num = P_MOSI; bus.miso_io_num = P_MISO; bus.sclk_io_num = P_SCK;
  bus.quadwp_io_num = -1; bus.quadhd_io_num = -1; bus.max_transfer_sz = 128;
  spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = 4000000; dev.mode = 0; dev.spics_io_num = P_NSS; dev.queue_size = 1;
  spi_bus_add_device(SPI2_HOST, &dev, &spi);
}

static void radio_config() {
  gpio_set_level((gpio_num_t)P_RST, 0); vTaskDelay(pdMS_TO_TICKS(2));
  gpio_set_level((gpio_num_t)P_RST, 1); vTaskDelay(pdMS_TO_TICKS(10));
  radio_version = rd(0x42);                                           // 0x12 = SX1276/77/78/79
  wr(0x01, 0x00); vTaskDelay(pdMS_TO_TICKS(10));                      // sleep, FSK, HF
  wr(0x01, 0x01); vTaskDelay(pdMS_TO_TICKS(2));                       // standby
  { uint32_t frf = (uint32_t)llround((double)freq_mhz * 1e6 / (32e6 / 524288.0));
    wr(0x06, frf >> 16); wr(0x07, frf >> 8); wr(0x08, frf); }
  { double brf = 32e6 / CHIP_RATE; uint16_t br = (uint16_t)brf; uint8_t frac = (uint8_t)lround((brf - br) * 16) & 0x0F;
    wr(0x02, br >> 8); wr(0x03, br); wr(0x5D, frac); }
  { uint16_t fd = (uint16_t)lround(dev_khz * 1000.0 / (32e6 / 524288.0));
    wr(0x04, (fd >> 8) & 0x3F); wr(0x05, fd); }
  wr(0x3B, 0xC2);                                                      // image calibration at this frequency
  for (int i = 0; i < 50 && (rd(0x3B) & 0x20); i++) vTaskDelay(pdMS_TO_TICKS(1));
  wr(0x0C, 0x23);                                                      // LNA: max gain G1, HF boost
  wr(0x0D, RXCFG);
  wr(0x0E, 0x02);                                                      // RSSI smoothing 8 samples
  wr(0x12, bw_code(bw_khz));                                           // RX bandwidth
  wr(0x13, bw_code(bw_khz));                                           // AFC bandwidth (AFC off)
  wr(0x1A, 0x00);
  wr(0x1F, 0xAA);                                                      // preamble detector on, 2 bytes, tolerance 10 chips
  wr(0x20, 0); wr(0x21, 0); wr(0x22, 0);                               // no RX timeouts
  wr(0x24, 0x07);                                                      // ClkOut off
  wr(0x27, 0x53);                                                      // auto restart RX, sync on, 4-byte sync
  { uint8_t s[4]; sync_bytes(s); for (int i = 0; i < 4; i++) wr(0x28 + i, s[i]); }
  wr(0x30, 0x00);                                                      // fixed length, no DC-free, CRC off, no address
  wr(0x31, 0x40);                                                      // packet mode
  wr(0x32, PAYLOAD_LEN);
  wr(0x40, 0x00);                                                      // DIO0 = PayloadReady
  wr(0x3E, 0xFF); wr(0x3F, 0xFF);                                      // clear flags / FIFO
  wr(0x01, 0x05);                                                      // RX
  vTaskDelay(pdMS_TO_TICKS(2));
  bool ok = radio_version == 0x12;
  if (!ok) radio_errs++;
  OUT("{\"type\":\"info\",\"msg\":\"SX1276 configured: %.3f MHz, bw %d kHz, dev %d kHz, pol %d, version 0x%02x%s\"}",
      freq_mhz, bw_khz, dev_khz, cur_pol, radio_version, ok ? "" : " - radio not answering, check board / pins");
}

static bool in_pkt = false;
static uint32_t t_sync = 0;
static float rssi_sync = 0;

static bool radio_poll(uint8_t *buf, float *rssi) {
  uint8_t f1 = rd(0x3E), f2 = rd(0x3F);
  if (f1 & 0x02) { n_pre++; wr(0x3E, 0x02); }
  if ((f1 & 0x01) && !in_pkt) { in_pkt = true; t_sync = millis(); rssi_sync = radio_rssi_now(); n_sync++; }
  if (f2 & 0x04) {                                                     // PayloadReady
    rd_fifo(buf, PAYLOAD_LEN);
    *rssi = in_pkt ? rssi_sync : radio_rssi_now();
    in_pkt = false;
    wr(0x3E, 0x0B);
    return true;
  }
  if (f2 & 0x10) wr(0x3F, 0x10);                                      // FIFO overrun
  if (in_pkt && millis() - t_sync > 30) { in_pkt = false; wr(0x0D, RXCFG | 0x40); }   // lost packet: restart RX
  return false;
}

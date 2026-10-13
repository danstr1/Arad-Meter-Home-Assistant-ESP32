// Board pin maps. The ESP-IDF target selects the board:
//   esp32   -> Heltec WiFi LoRa 32 V2 / V2.1 (ESP32 + SX1276, 868/915 MHz version)
//   esp32s3 -> Heltec WiFi LoRa 32 V3       (ESP32-S3 + SX1262, 863-928 MHz)
#pragma once
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3
  #define BOARD_NAME "Heltec WiFi LoRa 32 V3"
  #define RADIO_SX1262 1
  #define P_NSS 8
  #define P_SCK 9
  #define P_MOSI 10
  #define P_MISO 11
  #define P_RST 12
  #define P_BUSY 13
  #define P_LED 35
  #define P_SDA 17
  #define P_SCL 18
  #define P_OLED_RST 21
  #define P_VEXT 36      // low = OLED power on
  #define P_BTN 0        // PRG button
#elif CONFIG_IDF_TARGET_ESP32
  #define BOARD_NAME "Heltec WiFi LoRa 32 V2"
  #define RADIO_SX1276 1
  #define P_NSS 18
  #define P_SCK 5
  #define P_MOSI 27
  #define P_MISO 19
  #define P_RST 14
  #define P_DIO0 26
  #define P_LED 25
  #define P_SDA 4
  #define P_SCL 15
  #define P_OLED_RST 16
  #define P_VEXT 21      // low = Vext on
  #define P_BTN 0        // PRG button
#else
  #error "Unsupported target: build for esp32 (Heltec V2) or esp32s3 (Heltec V3)"
#endif

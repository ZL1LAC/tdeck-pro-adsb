// Pin map for the LilyGO T-Deck Pro v1.0 (hardware rev v1.0-241106).
// Values transcribed from the vendor header:
//   https://github.com/Xinyuan-LilyGO/T-Deck-Pro/blob/master/examples/factory/utilities.h
#pragma once

// ---------------------------------------------------------------- I2C bus ---
#define BOARD_I2C_SDA 13
#define BOARD_I2C_SCL 14

#define ADDR_TOUCH_CST328 0x1A
#define ADDR_ALS_LTR553   0x23
#define ADDR_IMU_BHI260   0x28
#define ADDR_KEYPAD_TCA8418 0x34
#define ADDR_GAUGE_BQ27220  0x55
#define ADDR_PMU_BQ25896    0x6B

// --------------------------------------------------------------- SPI bus ----
// Shared by the e-paper panel, the SD slot and the SX1262.
#define BOARD_SPI_SCK  36
#define BOARD_SPI_MOSI 33
#define BOARD_SPI_MISO 47

// ------------------------------------------------------------- E-paper -----
// GDEQ031T10, 240x320, UC8253 controller. No reset line is wired out.
#define BOARD_EPD_CS   34
#define BOARD_EPD_DC   35
#define BOARD_EPD_BUSY 37
#define BOARD_EPD_RST  (-1)

// ------------------------------------------------------------- Keyboard ----
#define BOARD_KEYPAD_INT 15
#define BOARD_KEYPAD_LED 42

// ----------------------------------------------------------------- Touch ---
#define BOARD_TOUCH_INT 12
#define BOARD_TOUCH_RST 45

// ------------------------------------------------------------------ GNSS ---
// u-blox MIA-M10Q on UART2. RXD/TXD are the ESP32 side of the link.
#define BOARD_GPS_RXD 44
#define BOARD_GPS_TXD 43
#define BOARD_GPS_PPS 1

// ------------------------------------------------------------------ LoRa ---
// SX1262. Sub-GHz only -- it cannot receive 1090 MHz ADS-B, so this project
// does not use it. Kept here so the rail stays powered down.
#define BOARD_LORA_CS   3
#define BOARD_LORA_BUSY 6
#define BOARD_LORA_RST  4
#define BOARD_LORA_INT  5

// ---------------------------------------------------------------- SD card ---
#define BOARD_SD_CS 48

// ----------------------------------------------------------- A7682E modem ---
#define BOARD_MODEM_RI     7
#define BOARD_MODEM_ITR    8
#define BOARD_MODEM_RST    9
#define BOARD_MODEM_RXD    10
#define BOARD_MODEM_TXD    11
#define BOARD_MODEM_PWRKEY 40

// ------------------------------------------------------------ Power rails ---
#define BOARD_GPS_EN  39  // GNSS module supply
#define BOARD_1V8_EN  38  // IMU 1.8 V rail
#define BOARD_6609_EN 41  // A7682E modem supply
#define BOARD_LORA_EN 46  // SX1262 supply

// ----------------------------------------------------------------- Misc -----
#define BOARD_BOOT_PIN  0
#define BOARD_MOTOR_PIN 2
#define BOARD_MIC_DATA  17
#define BOARD_MIC_CLOCK 18

// ------------------------------------------------------------ Panel size ---
#define EPD_WIDTH  240
#define EPD_HEIGHT 320

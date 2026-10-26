#pragma once

#include "board_common.h"
#include "sdkconfig.h"

#define BOARD_NAME "Freenove ESP32-S3 2.8\" CYD (ES3N28P)"
#define BOARD_DESCRIPTION \
  "Freenove ESP32-S3 2.8\" CYD, ILI9341 display + FT6336G touch, no onboard " \
  "speaker — used as an AirPlay visualizer, not an audio output device"

// No DAC on this build: the CYD has no onboard speaker, and this board is
// used to visualize the decoded PCM (via components/visualizer), not play
// it. iot_board_init() below does not register a DAC at all.

// I2C bus shared by the FT6336G touch controller (and, on the full ES3C28P
// datasheet, the audio codec — unused here since we don't drive audio out).
// Pins confirmed against the official QDtech ES3C28P/ES3N28P datasheet.
#define BOARD_I2C_TOUCH_SDA_GPIO 16
#define BOARD_I2C_TOUCH_SCL_GPIO 15
#define BOARD_I2C_TOUCH_RST_GPIO 18
#define BOARD_I2C_TOUCH_INT_GPIO 17

// LCD (ILI9341V, 4-line SPI). Handled by our own components/visualizer_panel
// (not this project's components/display, which only drives ST7789/OLED) —
// these are here for reference/reuse, not consumed by iot_board.c itself.
#define BOARD_LCD_CS_GPIO   10
#define BOARD_LCD_DC_GPIO   46
#define BOARD_LCD_SCLK_GPIO 12
#define BOARD_LCD_MOSI_GPIO 11
#define BOARD_LCD_MISO_GPIO 13 // "2nd SPI data line" per datasheet; unused (write-only panel)
#define BOARD_LCD_BL_GPIO   45 // high = backlight on

// Battery ADC (voltage sense only — no charge-status pin broken out on this
// variant per the datasheet; board_battery_read() below is unimplemented).
#define BOARD_BAT_ADC_GPIO 9

// LED / control GPIOs this project's generic Kconfig options may reference.
// All disabled (-1) — this board doesn't use any of them.
#define BOARD_LED_STATUS_GPIO CONFIG_LED_STATUS_GPIO
#define BOARD_LED_ERROR_GPIO  CONFIG_LED_ERROR_GPIO
#define BOARD_LED_RGB_GPIO    CONFIG_LED_RGB_GPIO
#define BOARD_JACK_GPIO       CONFIG_JACK_GPIO
#define BOARD_SPKFAULT_GPIO   CONFIG_SPKFAULT_GPIO
#define BOARD_MUTE_GPIO       CONFIG_MUTE_GPIO
#define BOARD_MUTE_GPIO_LEVEL CONFIG_MUTE_GPIO_LEVEL

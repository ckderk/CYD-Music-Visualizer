/**
 * @file board.c
 * @brief Freenove ESP32-S3 2.8" CYD (ES3N28P) board implementation
 *
 * This board has no onboard speaker — it's used as an AirPlay visualizer,
 * not an audio output device, so unlike every other board in this project
 * there is no DAC to register. The only hardware this file brings up is:
 *   - the I2C bus shared with the FT6336G touch controller
 *   - the battery voltage ADC
 *
 * The ILI9341 panel itself is NOT initialized here — that's handled by
 * components/visualizer_panel (a separate, simpler driver than this
 * project's components/display, which only supports ST7789/OLED). This
 * file just brings up the I2C bus and hands its handle out via
 * iot_board_get_handle(BOARD_I2C_TOUCH_ID) for that component to share.
 *
 * Pins confirmed against the official QDtech ES3C28P/ES3N28P datasheet.
 */

#include "iot_board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char TAG[] = "CYD-ES3N28P";

static bool s_board_initialized = false;
static i2c_master_bus_handle_t s_i2c_touch_bus = NULL;

// ============================================================================
// Battery monitor (GPIO9 ADC, no charge-status pin on this variant)
// ============================================================================

static adc_oneshot_unit_handle_t s_bat_adc = NULL;
static adc_cali_handle_t s_bat_cali = NULL;
static bool s_bat_calibrated = false;

#define BAT_ADC_CHANNEL ADC_CHANNEL_8 // GPIO9 on ESP32-S3 is ADC1 channel 8

static void board_battery_init(void) {
  adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ADC_UNIT_1};
  if (adc_oneshot_new_unit(&unit_cfg, &s_bat_adc) != ESP_OK) {
    ESP_LOGW(TAG, "Battery ADC init failed");
    s_bat_adc = NULL;
    return;
  }
  adc_oneshot_chan_cfg_t chan_cfg = {
      .bitwidth = ADC_BITWIDTH_DEFAULT,
      .atten = ADC_ATTEN_DB_12,
  };
  adc_oneshot_config_channel(s_bat_adc, BAT_ADC_CHANNEL, &chan_cfg);

  adc_cali_curve_fitting_config_t cali_cfg = {
      .unit_id = ADC_UNIT_1,
      .chan = BAT_ADC_CHANNEL,
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  s_bat_calibrated =
      (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_bat_cali) == ESP_OK);
  ESP_LOGI(TAG, "Battery monitor initialized (cali=%s)",
           s_bat_calibrated ? "yes" : "no");
}

bool board_battery_read(int *percent, bool *charging) {
  if (!s_bat_adc || !s_bat_calibrated) {
    return false;
  }
  int raw = 0;
  if (adc_oneshot_read(s_bat_adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) {
    return false;
  }
  int mv = 0;
  if (adc_cali_raw_to_voltage(s_bat_cali, raw, &mv) != ESP_OK) {
    return false;
  }
  // NOTE: the datasheet doesn't document this ADC's divider ratio (unlike
  // the Waveshare board's documented 2:1 divider) — this reads raw cell
  // voltage assuming a direct connection. If readings look exactly half (or
  // double) what a multimeter shows on the battery terminals, there's a
  // divider on this pin after all and this needs a scale factor.
  float v = mv / 1000.0f;

  if (percent) {
    static const struct {
      float v;
      int pct;
    } curve[] = {
        {3.30f, 0},  {3.50f, 10}, {3.60f, 20}, {3.68f, 35}, {3.74f, 50},
        {3.82f, 65}, {3.92f, 80}, {4.02f, 90}, {4.12f, 98}, {4.20f, 100},
    };
    const size_t n = sizeof(curve) / sizeof(curve[0]);
    int pct;
    if (v <= curve[0].v) {
      pct = curve[0].pct;
    } else if (v >= curve[n - 1].v) {
      pct = curve[n - 1].pct;
    } else {
      pct = curve[n - 1].pct;
      for (size_t i = 1; i < n; i++) {
        if (v < curve[i].v) {
          float span = curve[i].v - curve[i - 1].v;
          float frac = (v - curve[i - 1].v) / span;
          pct = curve[i - 1].pct +
                (int)(frac * (float)(curve[i].pct - curve[i - 1].pct) + 0.5f);
          break;
        }
      }
    }
    *percent = pct;
  }
  if (charging) {
    *charging = false; // no charge-status GPIO documented for this variant
  }
  return true;
}

// ============================================================================
// Board resource lookup / init
// ============================================================================

board_res_handle_t iot_board_get_handle(int id) {
  switch (id) {
    case BOARD_I2C_TOUCH_ID:
      return (board_res_handle_t)s_i2c_touch_bus;
    default:
      return NULL;
  }
}

const char *iot_board_get_info(void) { return BOARD_NAME; }

bool iot_board_is_init(void) { return s_board_initialized; }

esp_err_t iot_board_init(void) {
  if (s_board_initialized) {
    ESP_LOGW(TAG, "Board already initialized");
    return ESP_OK;
  }

  // Touch controller reset: hold low briefly, then release (FT6336G is
  // reset-active-low, same convention as the CST816S this project already
  // drives on the Waveshare board).
  gpio_reset_pin((gpio_num_t)BOARD_I2C_TOUCH_RST_GPIO);
  gpio_set_direction((gpio_num_t)BOARD_I2C_TOUCH_RST_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)BOARD_I2C_TOUCH_RST_GPIO, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level((gpio_num_t)BOARD_I2C_TOUCH_RST_GPIO, 1);
  vTaskDelay(pdMS_TO_TICKS(50));

  i2c_master_bus_config_t i2c_cfg = {
      .i2c_port = 0,
      .sda_io_num = (gpio_num_t)BOARD_I2C_TOUCH_SDA_GPIO,
      .scl_io_num = (gpio_num_t)BOARD_I2C_TOUCH_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  esp_err_t err = i2c_new_master_bus(&i2c_cfg, &s_i2c_touch_bus);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init touch I2C bus: %s", esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "Touch I2C bus initialized: sda=%d, scl=%d",
           BOARD_I2C_TOUCH_SDA_GPIO, BOARD_I2C_TOUCH_SCL_GPIO);

  board_battery_init();

  s_board_initialized = true;
  ESP_LOGI(TAG, "%s initialized (no DAC — visualizer-only board)", BOARD_NAME);
  return ESP_OK;
}

esp_err_t iot_board_deinit(void) {
  s_board_initialized = false;
  return ESP_OK;
}

// This board has no LVGL-dependent deferred resources (touch is read
// directly by components/visualizer_panel, not through LVGL's input device
// API), so this is a no-op — but the symbol must exist, main.c calls it
// unconditionally after display_init().
void iot_board_init_lvgl_resources(void) {}

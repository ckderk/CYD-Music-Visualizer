/**
 * @file visualizer_panel.c
 * @brief ILI9341 + FT6336G bring-up for the CYD visualizer.
 *
 * SPI/panel init follows the same esp_lcd call pattern as
 * components/display/display_st7789.c in this project (just swapping
 * esp_lcd_new_panel_st7789 for esp_lcd_new_panel_ili9341, and skipping
 * esp_lvgl_port entirely).
 *
 * Touch init/read follows the same i2c_master_transmit_receive pattern as
 * the CST816S driver in components/boards/waveshare-esp32s3/board.c, with
 * the FT6336G's own (different) register map:
 *   0x02 = TD_STATUS  (number of touch points, low nibble)
 *   0x03 = TOUCH1_XH  (event flag in bits[7:6], X high nibble in bits[3:0])
 *   0x04 = TOUCH1_XL
 *   0x05 = TOUCH1_YH  (touch ID in bits[7:4], Y high nibble in bits[3:0])
 *   0x06 = TOUCH1_YL
 * This is the standard FT62xx/FT63xx family map; NOT yet verified against
 * this exact board at runtime -- that's the next real bring-up step.
 */

#include "visualizer_panel.h"
#include "iot_board.h"
#include "board_common.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "visualizer_render.h"
#include "visualizer_feed.h"

static const char *TAG = "visualizer_panel";

// The ILI9341V itself is native portrait (240 cols x 320 rows per the
// datasheet), but this board sits in its case landscape, USB-C on the
// right. swap_xy()+mirror() below (in panel_init) rotate the panel's own
// coordinate space to match, so PANEL_WIDTH/HEIGHT here -- and everywhere
// else in this file, visualizer_render and visualizer_metadata -- already
// mean the physical landscape width/height.
#define PANEL_WIDTH  320
#define PANEL_HEIGHT 240
#define LCD_HOST          SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

#define FT6336G_I2C_ADDR     0x38
#define FT6336G_REG_TD_STATUS 0x02
#define FT6336G_REG_TOUCH1_XH 0x03

static uint16_t *s_fb = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static i2c_master_dev_handle_t s_touch_dev = NULL;
static bool s_touch_was_down = false;

// Signaled by on_color_trans_done() once the SPI DMA engine has actually
// finished reading s_fb for the in-flight esp_lcd_panel_draw_bitmap() call.
// esp_lcd_panel_draw_bitmap() itself can return well before that -- the I/O
// config below gives it a transaction queue (trans_queue_depth = 4) so it
// can queue a transfer and hand control back immediately -- and this
// project reuses a single framebuffer across frames with no double
// buffering, so without waiting on this semaphore the render loop starts
// overwriting s_fb while the panel is still streaming the previous frame
// out of it. See visualizer_panel_flush().
static SemaphoreHandle_t s_flush_done = NULL;

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                           esp_lcd_panel_io_event_data_t *edata,
                                           void *user_ctx) {
  (void)panel_io;
  (void)edata;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &woken);
  return woken == pdTRUE;
}

// ============================================================================
// Panel (SPI, ILI9341)
// ============================================================================

static esp_err_t panel_init(void) {
  // Backlight: GPIO45, high = on (per datasheet).
  gpio_config_t bl_cfg = {
      .pin_bit_mask = BIT64(BOARD_LCD_BL_GPIO),
      .mode = GPIO_MODE_OUTPUT,
  };
  ESP_ERROR_CHECK(gpio_config(&bl_cfg));
  gpio_set_level(BOARD_LCD_BL_GPIO, 0); // off until panel is fully up

  spi_bus_config_t buscfg = {
      .mosi_io_num = BOARD_LCD_MOSI_GPIO,
      .miso_io_num = -1, // write-only panel; GPIO13 not used as SPI MISO here
      .sclk_io_num = BOARD_LCD_SCLK_GPIO,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = PANEL_WIDTH * 40 * sizeof(uint16_t),
  };
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_handle_t io_handle = NULL;
  esp_lcd_panel_io_spi_config_t io_cfg = {
      .dc_gpio_num = BOARD_LCD_DC_GPIO,
      .cs_gpio_num = BOARD_LCD_CS_GPIO,
      .pclk_hz = LCD_PIXEL_CLOCK_HZ,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .spi_mode = 0,
      .trans_queue_depth = 4,
  };
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg,
                                &io_handle));

  // Binary semaphore starts "empty" by default, which is what we want:
  // the first visualizer_panel_flush() call issues a transfer and then
  // waits for it, same as every call after it.
  s_flush_done = xSemaphoreCreateBinary();
  if (s_flush_done != NULL) {
    esp_lcd_panel_io_callbacks_t trans_cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(
        io_handle, &trans_cbs, s_flush_done));
  } else {
    ESP_LOGW(TAG, "flush-done semaphore alloc failed -- frame tearing risk");
  }

  // RST shares CHIP_PU (no separate GPIO) -- see iot_board.h / porting notes.
  esp_lcd_panel_dev_config_t panel_cfg = {
      .reset_gpio_num = -1,
      .rgb_endian = LCD_RGB_ENDIAN_RGB,
      .bits_per_pixel = 16,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
  // Rotate the panel's native portrait (240x320) coordinate space into the
  // landscape orientation the board is actually housed in, USB-C on the
  // right. swap_xy is the 90-degree turn; mirror_x corrects the flip that
  // the turn alone leaves on this panel. If this ever comes up mirrored or
  // rotated the wrong way again, try the other 3 combinations of
  // esp_lcd_panel_mirror()'s two bools before touching swap_xy.
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
  // IPS panel (per datasheet) -- invert like display_st7789.c does for its
  // panel; if colors look wrong/inverted on first boot, this is the first
  // thing to flip.
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

  gpio_set_level(BOARD_LCD_BL_GPIO, 1); // backlight on
  ESP_LOGI(TAG, "ILI9341 panel initialized (%dx%d)", PANEL_WIDTH,
           PANEL_HEIGHT);
  return ESP_OK;
}

// ============================================================================
// Touch (I2C, FT6336G) -- shares the bus iot_board.c already brought up
// ============================================================================

static esp_err_t touch_init(void) {
  i2c_master_bus_handle_t bus =
      (i2c_master_bus_handle_t)iot_board_get_handle(BOARD_I2C_TOUCH_ID);
  if (bus == NULL) {
    ESP_LOGE(TAG, "Touch I2C bus not available (call iot_board_init() first)");
    return ESP_ERR_INVALID_STATE;
  }

  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = FT6336G_I2C_ADDR,
      .scl_speed_hz = 400000,
  };
  esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_touch_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add FT6336G I2C device: %s",
              esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "FT6336G touch controller registered at 0x%02X",
           FT6336G_I2C_ADDR);
  return ESP_OK;
}

static bool ft6336g_read_reg(uint8_t reg, uint8_t *val, size_t len) {
  return i2c_master_transmit_receive(s_touch_dev, &reg, 1, val, len, 50) ==
         ESP_OK;
}

bool visualizer_panel_poll_tap(void) {
  if (s_touch_dev == NULL) {
    return false;
  }
  uint8_t touch_count = 0;
  if (!ft6336g_read_reg(FT6336G_REG_TD_STATUS, &touch_count, 1)) {
    return false;
  }
  touch_count &= 0x0F;
  bool down_now = touch_count > 0;

  bool new_tap = down_now && !s_touch_was_down;
  s_touch_was_down = down_now;
  return new_tap;
}

// ============================================================================
// Framebuffer / flush
// ============================================================================

esp_err_t visualizer_panel_init(void) {
  s_fb = (uint16_t *)heap_caps_malloc(
      (size_t)PANEL_WIDTH * PANEL_HEIGHT * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (s_fb == NULL) {
    ESP_LOGW(TAG, "PSRAM framebuffer alloc failed, trying internal RAM");
    s_fb = (uint16_t *)heap_caps_malloc(
        (size_t)PANEL_WIDTH * PANEL_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_8BIT);
  }
  if (s_fb == NULL) {
    ESP_LOGE(TAG, "Framebuffer allocation failed");
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = panel_init();
  if (err != ESP_OK) {
    return err;
  }
  err = touch_init();
  if (err != ESP_OK) {
    // Non-fatal: visualizer still runs, just can't cycle modes by touch.
    ESP_LOGW(TAG, "Continuing without touch input");
  }
  return ESP_OK;
}

uint16_t *visualizer_panel_get_framebuffer(void) { return s_fb; }

void visualizer_panel_flush(void) {
  if (s_panel == NULL || s_fb == NULL) {
    return;
  }
  esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PANEL_WIDTH, PANEL_HEIGHT, s_fb);
  // Block here until THIS transfer (the one just submitted above) has
  // actually finished -- draw_bitmap() can return as soon as it's queued
  // (trans_queue_depth lets it queue up to 4), well before the SPI DMA
  // engine is done reading s_fb. Without this wait, the caller loops back
  // into the next render pass and starts mutating s_fb while it's still
  // mid-transfer.
  if (s_flush_done != NULL) {
    xSemaphoreTake(s_flush_done, portMAX_DELAY);
  }
}

void visualizer_panel_task(void *arg) {
  (void)arg;
  int tick = 0;
  while (1) {
    visualizer_render_tick();
    visualizer_panel_flush();
    if (visualizer_panel_poll_tap()) {
      visualizer_mode_t mode = visualizer_render_next_mode();
      ESP_LOGI(TAG, "Touch -> mode %d", (int)mode);
    }
    // TEMP diagnostic: once a second, log whether the feed thinks audio is
    // actively arriving. Remove once we've confirmed the hook is firing.
    if (++tick >= 40) {
      tick = 0;
      ESP_LOGI(TAG, "feed active=%d mode=%d",
               (int)visualizer_feed_is_active(1500),
               (int)visualizer_render_get_mode());
    }
    vTaskDelay(pdMS_TO_TICKS(25)); // ~40fps
  }
}

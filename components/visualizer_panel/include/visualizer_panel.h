/**
 * @file visualizer_panel.h
 * @brief ILI9341 + FT6336G bring-up for the CYD visualizer, bypassing LVGL.
 *
 * This drives the panel directly via esp_lcd (no esp_lvgl_port) for the 3
 * audio-reactive modes, which is lower-risk and avoids per-widget redraw
 * overhead for what's effectively full-frame animation at ~40fps. The 4th
 * mode (metadata) is handled separately, via
 * visualizer_render_set_metadata_drawer() -- see INTEGRATION.md.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Bring up the ILI9341 panel (SPI) and FT6336G touch (I2C), and
 * allocate the 240x320 RGB565 framebuffer in PSRAM.
 *
 * Must be called after iot_board_init() (so the shared touch I2C bus from
 * iot_board_get_handle(BOARD_I2C_TOUCH_ID) already exists).
 */
esp_err_t visualizer_panel_init(void);

/** @return the 240x320 RGB565 framebuffer visualizer_render draws into. */
uint16_t *visualizer_panel_get_framebuffer(void);

/** Push the current framebuffer contents to the physical panel. */
void visualizer_panel_flush(void);

/**
 * @brief Poll the FT6336G for a *new* touch-down edge (debounced).
 * @return true exactly once per physical tap, false otherwise.
 */
bool visualizer_panel_poll_tap(void);

/**
 * @brief The visualizer's own FreeRTOS task: render tick -> flush -> poll
 * touch (cycling the mode on each new tap) -> ~40fps delay, forever.
 *
 * Call visualizer_panel_init() and visualizer_render_init() before
 * spawning this as a task (see INTEGRATION.md step 1).
 */
void visualizer_panel_task(void *arg);

#pragma once
/**
 * @file visualizer_render.h
 * @brief Draws audio-reactive visuals into a caller-owned RGB565
 *        framebuffer. Deliberately knows nothing about SPI/esp_lcd/LVGL —
 *        your board's display glue owns the panel and just calls
 *        visualizer_render_tick() once per frame, then flushes the buffer
 *        (e.g. esp_lcd_panel_draw_bitmap(), or blit into an lv_canvas and
 *        lv_obj_invalidate() it).
 *
 * Pulls PCM from visualizer_feed_pull() internally — call
 * visualizer_feed_init() once at startup before the first tick.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  VIZ_MODE_SPECTRUM = 0, // FFT bar graph (classic EQ look)
  VIZ_MODE_WAVEFORM,     // scrolling oscilloscope trace
  VIZ_MODE_PARTICLES,    // generative particle flow, amplitude/beat reactive
  VIZ_MODE_METADATA,     // caller-supplied screen (track info, etc.)
  VIZ_MODE_COUNT
} visualizer_mode_t;

/**
 * Optional hook for VIZ_MODE_METADATA. If set, this is called instead of
 * visualizer_render's own drawing whenever the active mode is
 * VIZ_MODE_METADATA, so you can reuse an existing metadata/track-info
 * screen (e.g. this project's own display_st7789.c LVGL widgets) rather
 * than reimplementing it here. Receives the same framebuffer/dimensions
 * passed to visualizer_render_init(). If no drawer is registered,
 * VIZ_MODE_METADATA just shows a "no metadata screen" placeholder.
 */
typedef void (*visualizer_metadata_drawer_t)(uint16_t *fb, int width, int height);
void visualizer_render_set_metadata_drawer(visualizer_metadata_drawer_t drawer);

/**
 * width/height in pixels; fb must be width*height uint16_t (RGB565),
 * already allocated by the caller (e.g. in PSRAM) and must stay valid for
 * as long as the renderer is used.
 */
esp_err_t visualizer_render_init(uint16_t *fb, int width, int height);

/** Explicitly select a mode. */
void visualizer_render_set_mode(visualizer_mode_t mode);

/** Cycle to the next mode (wraps around) — wire this to your touch/button. */
visualizer_mode_t visualizer_render_next_mode(void);

visualizer_mode_t visualizer_render_get_mode(void);

/**
 * Pull available PCM, update internal state (FFT bins / scroll position /
 * particle sim) and redraw the current mode into the framebuffer. Call this
 * once per display frame from a dedicated task, e.g. at 30-45Hz:
 *
 *   while (1) {
 *     visualizer_render_tick();
 *     <flush fb to panel>
 *     vTaskDelay(pdMS_TO_TICKS(25));
 *   }
 *
 * Safe to call even when nothing is streaming — falls back to a gentle idle
 * animation driven by visualizer_feed_is_active().
 */
void visualizer_render_tick(void);

#ifdef __cplusplus
}
#endif

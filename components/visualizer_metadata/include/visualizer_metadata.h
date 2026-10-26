/**
 * @file visualizer_metadata.h
 * @brief The 4th visualizer mode: now-playing track info instead of an
 * audio-reactive render.
 *
 * Subscribes to this project's existing RTSP event system (the same one
 * components/display/display_st7789.c uses) and draws title/artist/album
 * plus a progress bar directly into the visualizer's shared RGB565
 * framebuffer -- no LVGL involved, consistent with how the other 3 modes
 * are drawn.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

/** Register for RTSP metadata/playback-state events. Call once at boot,
 *  after rtsp_server/rtsp_events are available (i.e. from app_main(), same
 *  as the other visualizer init calls). */
esp_err_t visualizer_metadata_init(void);

/**
 * Draw the current now-playing screen into fb (width x height, RGB565).
 * Matches visualizer_metadata_drawer_t from visualizer_render.h -- pass
 * this to visualizer_render_set_metadata_drawer().
 */
void visualizer_metadata_draw(uint16_t *fb, int width, int height);

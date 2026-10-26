/**
 * @file visualizer_metadata.c
 * @brief Now-playing screen for the 4th visualizer mode.
 *
 * Subscribes to the project's existing RTSP event observer pattern
 * (rtsp_events_register) -- the same mechanism components/display's
 * display_st7789.c already uses -- and renders title/artist/album plus a
 * progress bar straight into the shared RGB565 framebuffer with a small
 * hand-authored 5x7 bitmap font (font5x7.h), proofread as ASCII art before
 * being packed into that table (see components/visualizer_metadata's
 * font generator notes). No LVGL involved here, same as the other 3 modes.
 */

#include "visualizer_metadata.h"
#include "rtsp_events.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>

#include "font5x7.h"

static const char *TAG = "viz_metadata";

typedef enum {
  MD_STANDBY,   // no AirPlay client connected yet
  MD_CONNECTED, // connected, not yet playing (between tracks, buffering)
  MD_PLAYING,
  MD_PAUSED,
} md_state_t;

static struct {
  char title[METADATA_STRING_MAX];
  char artist[METADATA_STRING_MAX];
  char album[METADATA_STRING_MAX];
  uint32_t duration_secs;
  uint32_t position_secs;
  int64_t position_sync_us; // esp_timer_get_time() when position_secs was set
  md_state_t state;
} s_md;

static SemaphoreHandle_t s_lock;

// ============================================================================
// RTSP event subscription
// ============================================================================

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)user_data;
  if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;
  }
  switch (event) {
    case RTSP_EVENT_CLIENT_CONNECTED:
      s_md.state = MD_CONNECTED;
      break;
    case RTSP_EVENT_PLAYING:
      s_md.state = MD_PLAYING;
      // Playback may have (re)started without a fresh METADATA event right
      // behind it; re-sync the extrapolation clock so the progress bar
      // doesn't jump based on a stale timestamp.
      s_md.position_sync_us = esp_timer_get_time();
      break;
    case RTSP_EVENT_PAUSED:
      s_md.state = MD_PAUSED;
      break;
    case RTSP_EVENT_DISCONNECTED:
      s_md.state = MD_STANDBY;
      s_md.title[0] = '\0';
      s_md.artist[0] = '\0';
      s_md.album[0] = '\0';
      s_md.duration_secs = 0;
      s_md.position_secs = 0;
      break;
    case RTSP_EVENT_METADATA:
      if (data) {
        strncpy(s_md.title, data->metadata.title, sizeof(s_md.title) - 1);
        s_md.title[sizeof(s_md.title) - 1] = '\0';
        strncpy(s_md.artist, data->metadata.artist, sizeof(s_md.artist) - 1);
        s_md.artist[sizeof(s_md.artist) - 1] = '\0';
        strncpy(s_md.album, data->metadata.album, sizeof(s_md.album) - 1);
        s_md.album[sizeof(s_md.album) - 1] = '\0';
        s_md.duration_secs = data->metadata.duration_secs;
        s_md.position_secs = data->metadata.position_secs;
        s_md.position_sync_us = esp_timer_get_time();
      }
      break;
    default:
      break;
  }
  xSemaphoreGive(s_lock);
}

esp_err_t visualizer_metadata_init(void) {
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) {
    return ESP_ERR_NO_MEM;
  }
  memset(&s_md, 0, sizeof(s_md));
  if (rtsp_events_register(on_rtsp_event, NULL) != 0) {
    ESP_LOGW(TAG, "rtsp_events_register failed (max listeners reached?)");
  }
  ESP_LOGI(TAG, "metadata screen ready");
  return ESP_OK;
}

// ============================================================================
// Framebuffer text/drawing primitives (local to this file -- the drawer
// callback gets a raw fb+dimensions, not visualizer_render's internals)
// ============================================================================

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline void put_px(uint16_t *fb, int w, int h, int x, int y,
                          uint16_t color) {
  if ((unsigned)x < (unsigned)w && (unsigned)y < (unsigned)h) {
    fb[y * w + x] = color;
  }
}

static void fill_rect(uint16_t *fb, int fb_w, int fb_h, int x, int y, int rw,
                      int rh, uint16_t color) {
  for (int yy = y; yy < y + rh; yy++) {
    for (int xx = x; xx < x + rw; xx++) {
      put_px(fb, fb_w, fb_h, xx, yy, color);
    }
  }
}

#define GLYPH_W 5
#define GLYPH_H 7
#define GLYPH_GAP 1 // extra column of spacing after each glyph

static int draw_char(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                     int scale, uint16_t color, char c) {
  const uint8_t *cols = font5x7[font5x7_index(c)];
  for (int col = 0; col < GLYPH_W; col++) {
    uint8_t bits = cols[col];
    for (int row = 0; row < GLYPH_H; row++) {
      if (bits & (1 << row)) {
        fill_rect(fb, fb_w, fb_h, x + col * scale, y + row * scale, scale,
                  scale, color);
      }
    }
  }
  return (GLYPH_W + GLYPH_GAP) * scale; // advance width
}

// Returns total pixel width the string would occupy at this scale, without
// drawing anything -- used to center text and to truncate it to fit.
static int text_width(const char *s, int scale) {
  int n = (int)strlen(s);
  return n > 0 ? n * (GLYPH_W + GLYPH_GAP) * scale - GLYPH_GAP * scale : 0;
}

// Draws left-aligned text starting at (x, y); truncates with ".." if it
// would overflow max_w pixels. Returns the width actually drawn.
static int draw_text_clipped(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                             int scale, uint16_t color, const char *s,
                             int max_w) {
  char buf[METADATA_STRING_MAX];
  size_t len = strlen(s);
  if (len >= sizeof(buf)) {
    len = sizeof(buf) - 1;
  }
  memcpy(buf, s, len);
  buf[len] = '\0';

  while (text_width(buf, scale) > max_w && strlen(buf) > 2) {
    size_t l = strlen(buf);
    buf[l - 1] = '\0';
    if (l >= 3) {
      buf[l - 2] = '.';
      buf[l - 3] = '.';
    }
  }

  int cx = x;
  for (const char *p = buf; *p; p++) {
    cx += draw_char(fb, fb_w, fb_h, cx, y, scale, color, *p);
  }
  return cx - x;
}

// Centers text horizontally within [0, fb_w), clipping/truncating to margin.
static void draw_text_centered(uint16_t *fb, int fb_w, int fb_h, int y,
                               int scale, uint16_t color, const char *s,
                               int margin) {
  int max_w = fb_w - 2 * margin;
  int w = text_width(s, scale);
  if (w > max_w) {
    w = max_w;
  }
  int x = (fb_w - w) / 2;
  draw_text_clipped(fb, fb_w, fb_h, x, y, scale, color, s, max_w);
}

// ============================================================================
// Drawer
// ============================================================================

void visualizer_metadata_draw(uint16_t *fb, int width, int height) {
  const uint16_t bg = rgb565(0, 0, 0);
  const uint16_t fg = rgb565(230, 230, 230);
  const uint16_t dim = rgb565(120, 120, 120);
  const uint16_t accent = rgb565(40, 200, 120);

  fill_rect(fb, width, height, 0, 0, width, height, bg);

  char title[METADATA_STRING_MAX];
  char artist[METADATA_STRING_MAX];
  char album[METADATA_STRING_MAX];
  uint32_t duration_secs, position_secs;
  md_state_t state;
  int64_t sync_us;

  if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
    strcpy(title, s_md.title);
    strcpy(artist, s_md.artist);
    strcpy(album, s_md.album);
    duration_secs = s_md.duration_secs;
    position_secs = s_md.position_secs;
    state = s_md.state;
    sync_us = s_md.position_sync_us;
    xSemaphoreGive(s_lock);
  } else {
    // Couldn't get the lock in time -- draw nothing new this frame rather
    // than risk tearing/garbage; previous frame's content (or black from
    // the fill above) stays on screen.
    return;
  }

  const char *status_text = "WAITING FOR AIRPLAY...";
  switch (state) {
    case MD_CONNECTED:
      status_text = "CONNECTED";
      break;
    case MD_PLAYING:
      status_text = "PLAYING";
      break;
    case MD_PAUSED:
      status_text = "PAUSED";
      break;
    case MD_STANDBY:
    default:
      status_text = "WAITING FOR AIRPLAY...";
      break;
  }

  // Layout is expressed as fractions of height/width rather than fixed
  // pixel offsets, so it holds up whether this framebuffer is portrait
  // (240x320) or landscape (320x240) -- the panel's actual orientation is
  // a visualizer_panel.c concern, not this file's.
  const int margin = width / 24 > 6 ? width / 24 : 6;

  draw_text_centered(fb, width, height, height * 5 / 100, 1, dim, status_text,
                     margin);

  if (state == MD_STANDBY || title[0] == '\0') {
    draw_text_centered(fb, width, height, height / 2 - 4, 2, dim,
                       "NO TRACK YET", margin);
    return;
  }

  int title_y = height * 28 / 100;
  int artist_y = height * 41 / 100;
  int album_y = height * 47 / 100;
  int bar_y = height * 81 / 100;

  draw_text_centered(fb, width, height, title_y, 2, fg, title, margin);
  draw_text_centered(fb, width, height, artist_y, 1, fg, artist, margin);
  draw_text_centered(fb, width, height, album_y, 1, dim, album, margin);

  // Progress bar + elapsed/total time, only meaningful once we know the
  // duration (duration_secs == 0 before the first METADATA event arrives).
  if (duration_secs > 0) {
    uint32_t position = position_secs;
    if (state == MD_PLAYING) {
      int64_t elapsed_us = esp_timer_get_time() - sync_us;
      if (elapsed_us > 0) {
        position += (uint32_t)(elapsed_us / 1000000);
      }
    }
    if (position > duration_secs) {
      position = duration_secs;
    }

    const int bar_x = margin * 2, bar_w = width - bar_x * 2, bar_h = 6;
    fill_rect(fb, width, height, bar_x, bar_y, bar_w, bar_h,
              rgb565(50, 50, 50));
    int filled_w = (int)((int64_t)bar_w * position / duration_secs);
    if (filled_w > 0) {
      fill_rect(fb, width, height, bar_x, bar_y, filled_w, bar_h, accent);
    }

    char pos_str[8], dur_str[8], time_str[24];
    rtsp_format_time_mmss(position, pos_str, sizeof(pos_str));
    rtsp_format_time_mmss(duration_secs, dur_str, sizeof(dur_str));
    snprintf(time_str, sizeof(time_str), "%s / %s", pos_str, dur_str);
    draw_text_centered(fb, width, height, bar_y + 14, 1, dim, time_str,
                       margin);
  }
}

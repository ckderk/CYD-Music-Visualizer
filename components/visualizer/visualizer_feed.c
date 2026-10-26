/**
 * @file visualizer_feed.c
 * @brief Lock-free-ish SPSC ring buffer: audio pipeline writes, visualizer
 *        task reads. See visualizer_feed.h for the contract.
 */

#include "visualizer_feed.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "viz_feed";

// One "frame" = one interleaved L+R int16 pair = 4 bytes.
#define FRAME_BYTES 4

static int16_t *s_ring = NULL;       // capacity_frames * 2 int16 samples
static size_t s_capacity_frames = 0;

// Single-producer (audio pipeline), single-consumer (visualizer task).
// Plain volatile indices are sufficient for one writer + one reader on
// Xtensa/RISC-V with aligned 32-bit-ish accesses; we don't need a mutex on
// the hot path and must never block the audio pipeline.
static volatile size_t s_write_idx = 0; // next frame slot to write, mod capacity
static volatile size_t s_read_idx = 0;  // next frame slot to read, mod capacity
static volatile size_t s_available = 0; // frames currently buffered (<= capacity)

static volatile int64_t s_last_push_us = 0;

esp_err_t visualizer_feed_init(size_t capacity_frames) {
  if (s_ring) {
    ESP_LOGW(TAG, "already initialized");
    return ESP_OK;
  }
  if (capacity_frames == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  // PSRAM is plentiful on this board (8MB) and this buffer is not
  // latency-critical per-access (just a memcpy in/out), so prefer PSRAM and
  // leave internal SRAM for things that actually need it (DMA, WiFi, etc).
  s_ring = heap_caps_malloc(
      capacity_frames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
  );
  if (!s_ring) {
    // Fall back to internal RAM if PSRAM alloc fails for any reason, rather
    // than silently running without a visualizer feed at all.
    s_ring = heap_caps_malloc(capacity_frames * 2 * sizeof(int16_t), MALLOC_CAP_8BIT);
  }
  if (!s_ring) {
    ESP_LOGE(TAG, "failed to allocate %u-frame ring buffer", (unsigned)capacity_frames);
    return ESP_ERR_NO_MEM;
  }

  s_capacity_frames = capacity_frames;
  s_write_idx = 0;
  s_read_idx = 0;
  s_available = 0;
  s_last_push_us = 0;

  ESP_LOGI(
      TAG, "ring buffer ready: %u frames (%u bytes, %s)", (unsigned)capacity_frames,
      (unsigned)(capacity_frames * 2 * sizeof(int16_t)),
      esp_ptr_external_ram(s_ring) ? "PSRAM" : "internal"
  );
  return ESP_OK;
}

void visualizer_feed_push(const void *data, size_t bytes) {
  if (!s_ring || !data || bytes < FRAME_BYTES) {
    return;
  }

  size_t frames = bytes / FRAME_BYTES;
  const int16_t *src = (const int16_t *)data;

  // If the producer is handing us more than we can ever hold in one go
  // (shouldn't happen in practice — audio_output_write buffers are small),
  // just keep the most recent capacity_frames worth.
  if (frames > s_capacity_frames) {
    src += (frames - s_capacity_frames) * 2;
    frames = s_capacity_frames;
  }

  for (size_t i = 0; i < frames; i++) {
    size_t slot = (s_write_idx + i) % s_capacity_frames;
    s_ring[slot * 2 + 0] = src[i * 2 + 0];
    s_ring[slot * 2 + 1] = src[i * 2 + 1];
  }
  s_write_idx = (s_write_idx + frames) % s_capacity_frames;

  // Advance "available", dropping the oldest frames on overflow instead of
  // blocking — a lagging visualizer must never slow audio down.
  size_t new_available = s_available + frames;
  if (new_available > s_capacity_frames) {
    size_t dropped = new_available - s_capacity_frames;
    s_read_idx = (s_read_idx + dropped) % s_capacity_frames;
    new_available = s_capacity_frames;
  }
  s_available = new_available;

  s_last_push_us = esp_timer_get_time();
}

size_t visualizer_feed_pull(int16_t *dst, size_t max_frames) {
  if (!s_ring || !dst || max_frames == 0) {
    return 0;
  }

  size_t frames = s_available < max_frames ? s_available : max_frames;
  for (size_t i = 0; i < frames; i++) {
    size_t slot = (s_read_idx + i) % s_capacity_frames;
    dst[i * 2 + 0] = s_ring[slot * 2 + 0];
    dst[i * 2 + 1] = s_ring[slot * 2 + 1];
  }
  s_read_idx = (s_read_idx + frames) % s_capacity_frames;
  s_available -= frames;

  return frames;
}

bool visualizer_feed_is_active(uint32_t silence_timeout_ms) {
  if (s_last_push_us == 0) {
    return false;
  }
  int64_t age_us = esp_timer_get_time() - s_last_push_us;
  return age_us < ((int64_t)silence_timeout_ms * 1000);
}

/**
 * @file visualizer_render.c
 * @brief Spectrum / waveform / particle-flow renderers over a raw RGB565
 *        framebuffer, driven by PCM pulled from visualizer_feed.
 *
 * FFT path uses Espressif's esp-dsp component (dsps_fft2r_fc32 real-FFT
 * recipe, straight from esp-dsp's own `examples/fft`), not a hand-rolled
 * FFT — see idf_component.yml for the dependency.
 */

#include "visualizer_render.h"
#include "visualizer_feed.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "dsps_fft2r.h"
#include "dsps_wind_hann.h"

static const char *TAG = "viz_render";

// ============================================================================
// Shared state
// ============================================================================

#define FFT_N 512 // 512-point real FFT -> 256 usable bins, ~86Hz/bin @44.1kHz
#define SPECTRUM_BARS 24
#define WAVEFORM_HISTORY 320 // one column per pixel on a 320px-wide panel
#define PARTICLE_COUNT 48

static uint16_t *s_fb = NULL;
static int s_w = 0, s_h = 0;

static visualizer_mode_t s_mode = VIZ_MODE_SPECTRUM;
static visualizer_metadata_drawer_t s_metadata_drawer = NULL;

// Scratch PCM pull buffer (stereo frames)
#define PULL_FRAMES 512
static int16_t s_pcm[PULL_FRAMES * 2];

// ---- Spectrum state ----
static float s_fft_buf[FFT_N * 2]; // interleaved re/im
static float s_hann[FFT_N];
static float s_bar_level[SPECTRUM_BARS];  // smoothed, 0..1
static float s_bar_peak[SPECTRUM_BARS];   // slow-falling peak markers, 0..1
static int16_t s_fft_history[FFT_N];      // rolling window of mono samples
static size_t s_fft_history_fill = 0;

// ---- Waveform state ----
static int16_t s_wave_col[WAVEFORM_HISTORY]; // per-column peak amplitude
static int s_wave_write = 0;

// ---- Particle state ----
typedef struct {
  float x, y;
  float vx, vy;
  float life; // 0..1, fades out
  uint16_t color;
} particle_t;
static particle_t s_particles[PARTICLE_COUNT];
static uint32_t s_rand_state = 0x9E3779B9u;

static inline uint32_t xorshift32(void) {
  uint32_t x = s_rand_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  s_rand_state = x;
  return x;
}
static inline float randf(void) { return (float)(xorshift32() & 0xFFFF) / 65535.0f; }

// ============================================================================
// Tiny framebuffer helpers (RGB565)
// ============================================================================

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline void px(int x, int y, uint16_t c) {
  if ((unsigned)x >= (unsigned)s_w || (unsigned)y >= (unsigned)s_h) return;
  s_fb[y * s_w + x] = c;
}

static void fill(uint16_t c) {
  for (int i = 0; i < s_w * s_h; i++) s_fb[i] = c;
}

static void fade(uint8_t amount) {
  // Cheap persistence/trail effect: pull every pixel a bit toward black.
  for (int i = 0; i < s_w * s_h; i++) {
    uint16_t c = s_fb[i];
    uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = (r > amount) ? r - amount : 0;
    g = (g > amount * 2) ? g - amount * 2 : 0;
    b = (b > amount) ? b - amount : 0;
    s_fb[i] = (uint16_t)((r << 11) | (g << 5) | b);
  }
}

static void fill_rect(int x0, int y0, int w, int h, uint16_t c) {
  for (int y = y0; y < y0 + h; y++)
    for (int x = x0; x < x0 + w; x++)
      px(x, y, c);
}

static void draw_line(int x0, int y0, int x1, int y1, uint16_t c) {
  // Bresenham
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (1) {
    px(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// ============================================================================
// Public setup
// ============================================================================

esp_err_t visualizer_render_init(uint16_t *fb, int width, int height) {
  if (!fb || width <= 0 || height <= 0) return ESP_ERR_INVALID_ARG;
  s_fb = fb;
  s_w = width;
  s_h = height;

  esp_err_t err = dsps_fft2r_init_fc32(NULL, FFT_N);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "dsps_fft2r_init_fc32 failed: %d", err);
    return err;
  }
  dsps_wind_hann_f32(s_hann, FFT_N);

  memset(s_bar_level, 0, sizeof(s_bar_level));
  memset(s_bar_peak, 0, sizeof(s_bar_peak));
  memset(s_fft_history, 0, sizeof(s_fft_history));
  memset(s_wave_col, 0, sizeof(s_wave_col));

  for (int i = 0; i < PARTICLE_COUNT; i++) {
    s_particles[i].life = 0; // dead, will be (re)spawned on first beat
  }

  fill(0x0000);
  ESP_LOGI(TAG, "renderer ready: %dx%d, FFT_N=%d", s_w, s_h, FFT_N);
  return ESP_OK;
}

void visualizer_render_set_metadata_drawer(visualizer_metadata_drawer_t drawer) {
  s_metadata_drawer = drawer;
}

void visualizer_render_set_mode(visualizer_mode_t mode) {
  if (mode >= 0 && mode < VIZ_MODE_COUNT) {
    s_mode = mode;
    fill(0x0000); // clear so the old mode's pixels don't bleed through
  }
}

visualizer_mode_t visualizer_render_next_mode(void) {
  s_mode = (visualizer_mode_t)((s_mode + 1) % VIZ_MODE_COUNT);
  fill(0x0000);
  return s_mode;
}

visualizer_mode_t visualizer_render_get_mode(void) { return s_mode; }

// ============================================================================
// Idle state (nothing currently streaming)
// ============================================================================

static void draw_idle(void) {
  fade(6);
  // Slow single "breathing" dot in the center — cheap, calm, unmistakably
  // not a crash (unlike a frozen frame).
  static float phase = 0.0f;
  phase += 0.03f;
  float r = (sinf(phase) * 0.5f + 0.5f) * (s_h / 6.0f) + 4.0f;
  int cx = s_w / 2, cy = s_h / 2;
  uint16_t c = rgb565(40, 60, 90);
  for (int a = 0; a < 360; a += 6) {
    float rad = a * (float)M_PI / 180.0f;
    px(cx + (int)(cosf(rad) * r), cy + (int)(sinf(rad) * r), c);
  }
}

// ============================================================================
// Mode 1: Spectrum bars
// ============================================================================

static void update_fft(void) {
  size_t got = visualizer_feed_pull(s_pcm, PULL_FRAMES);
  for (size_t i = 0; i < got; i++) {
    int16_t mono = (int16_t)(((int32_t)s_pcm[i * 2] + (int32_t)s_pcm[i * 2 + 1]) / 2);
    // shift history left, append newest sample — simple rolling window.
    if (s_fft_history_fill < FFT_N) {
      s_fft_history[s_fft_history_fill++] = mono;
    } else {
      memmove(s_fft_history, s_fft_history + 1, (FFT_N - 1) * sizeof(int16_t));
      s_fft_history[FFT_N - 1] = mono;
    }
  }
  if (s_fft_history_fill < FFT_N) return; // not enough samples yet

  for (int i = 0; i < FFT_N; i++) {
    float sample = (float)s_fft_history[i] / 32768.0f;
    s_fft_buf[i * 2 + 0] = sample * s_hann[i];
    s_fft_buf[i * 2 + 1] = 0.0f;
  }

  dsps_fft2r_fc32(s_fft_buf, FFT_N);
  dsps_bit_rev_fc32(s_fft_buf, FFT_N);
  dsps_cplx2reC_fc32(s_fft_buf, FFT_N);

  // Group the 256 usable bins into SPECTRUM_BARS log-spaced bars (bass
  // gets more bars than treble, matching how an EQ display usually looks).
  const int usable_bins = FFT_N / 2;
  for (int bar = 0; bar < SPECTRUM_BARS; bar++) {
    float f0 = powf((float)usable_bins, (float)bar / SPECTRUM_BARS);
    float f1 = powf((float)usable_bins, (float)(bar + 1) / SPECTRUM_BARS);
    int bin0 = (int)f0 < 1 ? 1 : (int)f0; // skip DC bin 0
    int bin1 = (int)f1 < bin0 + 1 ? bin0 + 1 : (int)f1;
    if (bin1 > usable_bins) bin1 = usable_bins;

    float mag_sum = 0.0f;
    int n = 0;
    for (int b = bin0; b < bin1; b++) {
      float re = s_fft_buf[b * 2 + 0], im = s_fft_buf[b * 2 + 1];
      mag_sum += sqrtf(re * re + im * im);
      n++;
    }
    float mag = n ? (mag_sum / n) : 0.0f;

    // Rough dB-ish scaling into 0..1. These constants are a starting point
    // tuned by ear/eye, not derived from a calibrated reference — expect to
    // nudge them once you're looking at real music on the actual panel.
    float db = 20.0f * log10f(mag + 1e-6f);
    float level = (db + 60.0f) / 50.0f; // -60dB..-10dB -> 0..1
    if (level < 0) level = 0;
    if (level > 1) level = 1;

    // Attack fast, decay slow (classic VU-meter ballistics).
    if (level > s_bar_level[bar]) {
      s_bar_level[bar] = s_bar_level[bar] * 0.4f + level * 0.6f;
    } else {
      s_bar_level[bar] = s_bar_level[bar] * 0.85f + level * 0.15f;
    }
    if (s_bar_level[bar] > s_bar_peak[bar]) {
      s_bar_peak[bar] = s_bar_level[bar];
    } else {
      s_bar_peak[bar] -= 0.01f; // slow-falling peak marker
      if (s_bar_peak[bar] < s_bar_level[bar]) s_bar_peak[bar] = s_bar_level[bar];
    }
  }
}

static void draw_spectrum(void) {
  fill(0x0000);
  int margin = 4;
  int bar_area_w = s_w - margin * 2;
  int bar_w = bar_area_w / SPECTRUM_BARS;
  int gap = bar_w > 4 ? 2 : 0;

  for (int bar = 0; bar < SPECTRUM_BARS; bar++) {
    int x = margin + bar * bar_w;
    int bh = (int)(s_bar_level[bar] * (s_h - 8));
    int peak_y = s_h - 4 - (int)(s_bar_peak[bar] * (s_h - 8));

    // Simple vertical gradient: green (quiet) -> yellow -> red (loud),
    // picked per-pixel-row so a tall bar visibly climbs through the colors.
    for (int y = 0; y < bh; y++) {
      float frac = (float)y / (float)(s_h - 8);
      uint16_t c;
      if (frac < 0.6f) c = rgb565(30, 220, 90);
      else if (frac < 0.85f) c = rgb565(230, 210, 40);
      else c = rgb565(230, 60, 50);
      fill_rect(x, s_h - 4 - y, bar_w - gap, 1, c);
    }
    fill_rect(x, peak_y, bar_w - gap, 2, rgb565(255, 255, 255));
  }
}

// ============================================================================
// Mode 2: Waveform / oscilloscope
// ============================================================================

static void update_waveform(void) {
  size_t got = visualizer_feed_pull(s_pcm, PULL_FRAMES);
  if (got == 0) return;

  // Collapse this pull into a small number of columns (peak amplitude per
  // chunk) so a burst of audio since the last tick still shows as one
  // smooth scroll step rather than a sudden jump.
  size_t chunk = got / 1 > 0 ? got : 1;
  int16_t peak = 0;
  for (size_t i = 0; i < got; i++) {
    int16_t mono = (int16_t)(((int32_t)s_pcm[i * 2] + (int32_t)s_pcm[i * 2 + 1]) / 2);
    int16_t a = mono < 0 ? -mono : mono;
    if (a > peak) peak = a;
  }
  (void)chunk;
  s_wave_col[s_wave_write] = peak;
  s_wave_write = (s_wave_write + 1) % WAVEFORM_HISTORY;
}

static void draw_waveform(void) {
  fill(0x0000);
  int mid = s_h / 2;
  int cols = s_w < WAVEFORM_HISTORY ? s_w : WAVEFORM_HISTORY;
  int prev_y = mid;
  for (int i = 0; i < cols; i++) {
    int idx = (s_wave_write + (WAVEFORM_HISTORY - cols) + i) % WAVEFORM_HISTORY;
    float amp = (float)s_wave_col[idx] / 32768.0f;
    int half = (int)(amp * (mid - 4));
    uint16_t c = rgb565(80, 180, 255);
    draw_line(i, mid - half, i, mid + half, c);
    draw_line(i > 0 ? i - 1 : i, prev_y, i, mid - half, rgb565(150, 220, 255));
    prev_y = mid - half;
  }
  draw_line(0, mid, s_w - 1, mid, rgb565(30, 40, 60));
}

// ============================================================================
// Mode 3: Particle flow (amplitude/beat reactive)
// ============================================================================

static float s_beat_envelope = 0.0f;

static void spawn_particle(float energy) {
  for (int i = 0; i < PARTICLE_COUNT; i++) {
    if (s_particles[i].life <= 0.0f) {
      s_particles[i].x = s_w / 2.0f;
      s_particles[i].y = s_h / 2.0f;
      float angle = randf() * 2.0f * (float)M_PI;
      float speed = 0.5f + energy * 4.0f;
      s_particles[i].vx = cosf(angle) * speed;
      s_particles[i].vy = sinf(angle) * speed;
      s_particles[i].life = 1.0f;
      uint8_t hue = (uint8_t)(randf() * 255);
      // cheap hue spread across warm colors
      s_particles[i].color = rgb565(
          180 + (hue % 60), 80 + ((hue * 3) % 150), 120 + ((hue * 7) % 120)
      );
      return;
    }
  }
}

static void update_particles(void) {
  size_t got = visualizer_feed_pull(s_pcm, PULL_FRAMES);
  float energy = 0.0f;
  for (size_t i = 0; i < got; i++) {
    float l = s_pcm[i * 2] / 32768.0f, r = s_pcm[i * 2 + 1] / 32768.0f;
    energy += fabsf(l) + fabsf(r);
  }
  if (got) energy /= (float)(got * 2);

  // Simple envelope-follower "beat" detector: a sharp rise over the recent
  // smoothed average spawns a burst of particles. Not real beat-tracking,
  // just enough to feel reactive without heavy analysis.
  float smoothed = s_beat_envelope * 0.9f + energy * 0.1f;
  if (energy > smoothed * 1.5f && energy > 0.02f) {
    int burst = 1 + (int)(energy * 10);
    for (int i = 0; i < burst; i++) spawn_particle(energy);
  }
  s_beat_envelope = smoothed;

  for (int i = 0; i < PARTICLE_COUNT; i++) {
    if (s_particles[i].life <= 0.0f) continue;
    s_particles[i].x += s_particles[i].vx;
    s_particles[i].y += s_particles[i].vy;
    s_particles[i].life -= 0.015f;
    if (s_particles[i].x < 0 || s_particles[i].x >= s_w || s_particles[i].y < 0 ||
        s_particles[i].y >= s_h) {
      s_particles[i].life = 0;
    }
  }
}

static void draw_particles(void) {
  fade(10);
  for (int i = 0; i < PARTICLE_COUNT; i++) {
    if (s_particles[i].life <= 0.0f) continue;
    int x = (int)s_particles[i].x, y = (int)s_particles[i].y;
    px(x, y, s_particles[i].color);
    px(x + 1, y, s_particles[i].color);
    px(x, y + 1, s_particles[i].color);
  }
}

// ============================================================================
// Tick dispatch
// ============================================================================

void visualizer_render_tick(void) {
  bool active = visualizer_feed_is_active(1500);

  if (!active && s_mode != VIZ_MODE_METADATA) {
    draw_idle();
    return;
  }

  switch (s_mode) {
    case VIZ_MODE_SPECTRUM:
      update_fft();
      draw_spectrum();
      break;
    case VIZ_MODE_WAVEFORM:
      update_waveform();
      draw_waveform();
      break;
    case VIZ_MODE_PARTICLES:
      update_particles();
      draw_particles();
      break;
    case VIZ_MODE_METADATA:
      if (s_metadata_drawer) {
        s_metadata_drawer(s_fb, s_w, s_h);
      } else {
        fill(0x0000);
      }
      break;
    default:
      break;
  }
}

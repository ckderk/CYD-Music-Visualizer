#pragma once
/**
 * @file visualizer_feed.h
 * @brief Tap point for decoded AirPlay PCM audio, feeding the visualizer.
 *
 * Call visualizer_feed_init() once at startup, then call
 * visualizer_feed_push() from inside audio_output_write() (see the one-line
 * patch in docs/INTEGRATION.md) so every PCM buffer that would have gone to
 * the DAC/I2S also lands in a lock-free ring buffer the visualizer task
 * reads from. This does NOT replace audio_output_write()'s normal job (your
 * board may still have no DAC connected at all, which is fine — this just
 * mirrors the data).
 *
 * Format assumption (matches audio_output_write()'s own contract):
 *   interleaved stereo, 16-bit signed PCM, native endianness.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate the ring buffer. capacity_frames is in STEREO FRAMES (1 frame =
 * 1 left sample + 1 right sample = 4 bytes). ~4096 frames (~93ms at 44.1kHz)
 * is a reasonable default — enough for a stable FFT window plus some slack
 * for the visualizer task being a little late, without adding noticeable
 * extra audio-to-visual latency.
 */
esp_err_t visualizer_feed_init(size_t capacity_frames);

/**
 * Push decoded PCM into the ring buffer. Safe to call from the audio
 * pipeline's own task/priority — this only ever does a bounded memcpy plus
 * atomic index updates, never blocks, never allocates.
 *
 * If the visualizer task is falling behind, this silently drops the oldest
 * unread frames rather than blocking audio playback (or the writer, if
 * nothing is actually being played back) — the visualizer missing a few ms
 * of audio is harmless, stalling the audio pipeline is not.
 *
 * @param data   interleaved stereo int16 PCM, as passed to audio_output_write
 * @param bytes  number of bytes in data (must be a multiple of 4)
 */
void visualizer_feed_push(const void *data, size_t bytes);

/**
 * Pull up to max_frames stereo frames out of the ring buffer into dst
 * (interleaved int16, L,R,L,R,...). Returns the number of frames actually
 * copied (may be less than max_frames, including 0 if nothing new has
 * arrived — e.g. AirPlay is idle/paused).
 *
 * Intended to be called once per render frame (~30-60Hz) by the visualizer
 * task.
 */
size_t visualizer_feed_pull(int16_t *dst, size_t max_frames);

/**
 * True if at least one PCM frame has been pushed in the last
 * silence_timeout_ms milliseconds. The visualizer screens use this to show
 * an "idle / nothing streaming" state instead of a flat-line spectrum when
 * no one is AirPlaying anything.
 */
bool visualizer_feed_is_active(uint32_t silence_timeout_ms);

#ifdef __cplusplus
}
#endif

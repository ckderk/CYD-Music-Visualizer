# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## This fork

This is Derk's fork of [`rbouteiller/airplay-esp32`](https://github.com/rbouteiller/airplay-esp32), built
around one specific device: a **Freenove ESP32-S3 2.8" CYD (ES3N28P)** that shows up on his iPhone as an
AirPlay 2 speaker but, instead of playing the audio, renders audio-reactive visuals (spectrum / waveform /
particles / now-playing metadata) on its own ILI9341 screen, cycled by touch. It has no onboard speaker in
this use case — no DAC is registered.

Everything upstream still builds and works unmodified; the CYD support is additive (`env:cyd-es3n28p`,
`components/visualizer*`, `components/boards/cyd-es3n28p`). See **[CYD Visualizer](#cyd-visualizer-es3n28p)**
below before touching any of it, and read
[`docs/boards/cyd-es3n28p.md`](docs/boards/cyd-es3n28p.md) for the hardware/pin reference.

## Project Overview

ESP32 AirPlay 2 Receiver — firmware that turns ESP32/ESP32-S3/ESP32-P4 boards into AirPlay 2 speakers. Supports ALAC and AAC decoding, Bluetooth A2DP (ESP32 only), W5500 Ethernet (Esparagus Audio Brick), OLED/TFT displays, hardware buttons, and OTA updates.

## Build & Flash

**PlatformIO** (recommended):
```bash
pio run -e <env>                   # Build firmware (no -t; "-t build" is not a real
                                    # PlatformIO target and fails with a confusing
                                    # SCons "Do not know how to make target" error)
pio run -e <env> -t upload          # Build + flash via USB
pio run -e <env> -t monitor         # Serial monitor (115200 baud)
pio run -e <env> -t uploadfs        # Flash SPIFFS from data/
pio run -e <env> -t menuconfig      # Kconfig configuration
```

**ESP-IDF** (native):
```bash
source /path/to/esp-idf/export.sh
idf.py set-target esp32s3           # or esp32, esp32p4
idf.py build
idf.py -p /dev/ttyUSB0 flash
idf.py -p /dev/ttyUSB0 monitor
```

## Build Environments

| Environment | Board | Notes |
|---|---|---|
| `esp32s3` | ESP32-S3 + external DAC (e.g. PCM5102A) | Default |
| `esp32s3-jtag` | ESP32-S3 with JTAG | Extends esp32s3 |
| `squeezeamp` | ESP32 + TAS5756 DAC/amp | 8MB flash |
| `squeezeamp-bt` | Same + Bluetooth A2DP | |
| `squeezeamp-4m` | SqueezeAMP with 4MB flash | |
| `esparagus-audio-brick` | ESP32 + TAS5825M DAC/amp | |
| `esparagus-audio-brick-bt` | Same + Bluetooth + Ethernet | |
| `esparagus-audio-brick-s3` | ESP32-S3 + TAS5825M | |
| `esparagus-audio-brick-dual-dac` | ESP32-S3 + 2x TAS5825M (rev D) | Stereo @0x4C + PBTL mono sub @0x4D |
| `esparagus-louder` | ESP32 + TAS5825M + extra gain | |
| `esparagus-louder-bt` | Louder + Bluetooth | |
| `esparagus-louder-s3` | ESP32-S3 + Louder | |
| `cyd-es3n28p` | Freenove ESP32-S3 2.8" CYD — AirPlay **visualizer**, no DAC/speaker | This fork's own board, see below |

Board configs live in `config/` (the generated `sdkconfig` stays at the project root). Sdkconfig defaults are layered via `cmake_extra_args` (left-to-right override). Custom board config: create `config/sdkconfig.user.<name>` + `user_platformio.ini` to extend any environment without modifying the main config.

## Architecture

```
main/
├── main.c                  # Entry point — initializes NVS, WiFi, starts AirPlay services
├── settings.c              # NVS persistence for device name, WiFi credentials, volume
├── audio/                  # Audio pipeline
│   ├── audio_receiver.c    # RTSP session manager — orchestrates streams (buffered/unbuffered)
│   ├── audio_stream.c      # Base stream abstraction
│   ├── audio_stream_buffered.c   # AirPlay 2 AAC (deep jitter buffer)
│   ├── audio_stream_realtime.c     # AirPlay 1 ALAC (low latency UDP)
│   ├── audio_decoder.c     # ALAC and AAC decoders
│   ├── audio_buffer.c      # Frame buffering between receiver and output
│   ├── audio_timing.c      # PTP-based timing — early/late frame handling
│   ├── audio_resample.c    # Sample rate conversion (44.1→48kHz)
│   ├── audio_output.c      # I2S output
│   ├── audio_output_spdif.c # S/PDIF output
│   ├── audio_output_usb.c  # USB audio output
│   ├── audio_crypto.c      # AirPlay encryption
│   ├── a2dp_sink.c         # Bluetooth A2DP sink
│   └── eq_events.c         # EQ parameter changes (TAS58xx)
├── rtsp/                   # RTSP protocol server
│   ├── rtsp_server.c       # RTSP connection handler
│   ├── rtsp_conn.c         # Connection management
│   ├── rtsp_handlers.c     # RTSP method handlers (OPTIONS, SETUP, PLAY, etc.)
│   ├── rtsp_events.c       # RTSP event handling (including BT passthrough)
│   ├── rtsp_crypto.c       # RTSP-level encryption
│   ├── rtsp_fairplay.c     # Apple FairPlay integration
│   └── rtsp_rsa.c          # RSA crypto
├── hap/                    # HomeKit Accessory Protocol
│   ├── hap.c               # Core HAP
│   ├── hap_pair_setup.c    # Pairing setup (SRP handshake)
│   ├── hap_pair_verify.c   # Pair verify (Ed25519)
│   ├── hap_crypto.c        # HAP encryption
│   └── srp.c               # SRP-6a key exchange
├── plist/                  # Apple Property List parsing
├── network/                # Network stack
│   ├── wifi.c              # WiFi AP+STA, captive portal, auto-reconnect
│   ├── ethernet.c          # W5500 SPI Ethernet driver
│   ├── mdns_airplay.c      # mDNS AirPlay service advertisement
│   ├── ptp_clock.c         # Precision Time Protocol clock
│   ├── ntp_clock.c         # NTP time sync fallback
│   ├── web_server.c        # HTTP config/control server
│   ├── ota.c               # OTA firmware updates
│   ├── dns_server.c        # Captive portal DNS
│   └── log_stream.c        # Remote log streaming
├── dacp_client.c           # DACP (Digital Audio Control Protocol) — button/remote commands
├── playback_control.c      # Unified playback control abstraction
├── buttons.c               # Hardware button input with debounce + auto-repeat
└── led.c                   # LED status indicator

components/
├── dac/                    # Abstract DAC API (Kconfig-selected implementation)
│   └── dac.c               # Dispatch layer → TAS57xx or TAS58xx driver
├── dac_tas57xx/            # TI TAS57xx (TAS5756/5754/5751) DAC driver with hybrid flow DSP
├── dac_tas58xx/            # TI TAS58xx (TAS5825M) DAC driver with on-chip DSP + 15-band EQ
├── display/                # Display drivers
│   ├── display.c           # Common display API
│   ├── display_st7789.c    # ST7789 TFT with LVGL 9 rendering (ESP32-S3)
│   └── display_stub.c      # No-op stub when display disabled
├── boards/                 # Board support (HAL)
│   ├── board_common.c      # Shared board utilities
│   ├── esp32-generic/      # ESP32 generic board init
│   ├── esp32s3-generic/    # ESP32-S3 generic board init
│   ├── waveshare-esp32p4/  # Waveshare ESP32-P4 board init
│   ├── squeezeamp/         # SqueezeAMP (ESP32 + TAS5756)
│   ├── esparagus-audio-brick/ # Esparagus Audio Brick (ESP32 + TAS5825M + W5500)
│   └── cyd-es3n28p/        # Freenove CYD — no DAC, owns the touch I2C bus for the visualizer
├── spiffs_storage/         # SPIFFS filesystem mount (stores web pages + DSP configs)
├── audio-resampler/        # sinc-based audio resampler (44.1→48kHz)
├── board_utils/            # Board-level utilities
├── visualizer/             # Hardware-independent: ring buffer tap + FFT/waveform/particle
│                           #   renderers, draw into a plain RGB565 framebuffer
├── visualizer_panel/       # ILI9341 (esp_lcd, no LVGL) + FT6336G touch bring-up for the CYD
└── visualizer_metadata/    # 4th mode: now-playing screen, own 5x7 bitmap font, RTSP events
```

## Key Conventions

- **CMake/Kconfig**: Board selection is via `CONFIG_` Kconfig options. DAC driver is auto-selected (`CONFIG_DAC_TAS57XX` or `CONFIG_DAC_TAS58XX`). Display, buttons, BT, Ethernet are all Kconfig-gated.
- **Component structure**: Each component has its own `CMakeLists.txt` with `idf_component_register()`.
- **Git submodules**: `u8g2` (OLED graphics) and `u8g2-hal-esp-idf` (ESP-IDF HAL for u8g2) are submodules — always clone with `--recursive`.
- **SPIFFS**: `data/` directory contents are flashed to SPIFFS. `data/www/` = web UI, `data/hf/` = hybrid flow DSP binaries for TAS57xx.
- **Audio pipeline**: AudioReceiver (rtsp) → decoder → AudioBuffer → AudioOutput (I2S/SPDIF/USB). Buffered streams (AAC) use deep jitter buffer; realtime streams (ALAC) use low-latency UDP with early/late timing thresholds.
- **AirPlay/Bluetooth coexistence**: Mutually exclusive at runtime. BT connection suspends AirPlay; disconnect resumes it.
- **Eth/WiFi failover**: Ethernet preferred at boot; WiFi fallback if no cable. Hot-swap at runtime.

## CYD Visualizer (ES3N28P)

Status: **all 4 modes working on real hardware** — spectrum, waveform, particles, and a
metadata (now-playing) screen, cycled by touch. Display orientation (landscape, USB-C
right) is also fixed and confirmed. Remaining open items:

- [ ] Remove the temporary `feed active=`/mode diagnostic log line in `visualizer_panel.c`
- [ ] Produce a merged `.bin` (`esptool.py merge_bin`) and reinstall via bmorcelli/Launcher
      from the SD card — currently flashed straight over USB during bring-up, which
      overwrites Launcher on the board (expected, reversible; see
      [`docs/boards/cyd-es3n28p.md`](docs/boards/cyd-es3n28p.md#installing-via-launcher))

### How it's wired in

- `main/audio/audio_output.c`'s `playback_task()` calls `visualizer_feed_push()` right next
  to the existing `led_audio_feed()` call. **This is the real tap point** — it writes PCM
  via `i2s_channel_write()` directly. The public `audio_output_write()` function is a red
  herring for this board: only the (unused, on this board) Bluetooth A2DP path calls it.
  If audio-reactive modes ever go dead again, check this first.
- `main/main.c`'s `app_main()`, inside `#ifdef CONFIG_BOARD_CYD_ES3N28P`, brings up the
  feed ring buffer, the panel, the metadata drawer, and the render task, in that order.
- `components/visualizer/visualizer_render.c` is written generically against whatever
  `width`/`height` `visualizer_render_init()` is given — it has no portrait/landscape
  assumption baked in (confirmed by its own `WAVEFORM_HISTORY` comment, written for a
  "320px-wide panel" before the orientation bug below was even found). Don't special-case
  orientation in there; fix it at the panel-init layer instead.

### Display orientation: swap_xy + mirror, not literals

The ILI9341V is native **portrait** (240×320 per the datasheet), but this board sits in
its case **landscape, USB-C on the right**. `visualizer_panel.c`'s `panel_init()` corrects
this with:

```c
ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
```

If this ever looks wrong again (rotated, mirrored, upside down, any combination), **don't
guess from first principles** — the two `mirror()` bools' visual effect is not independent
of `swap_xy`, so reasoning about MX/MY bits in the abstract is unreliable. Instead, treat it
as a 4-way search: `swap_xy` fixes gross 90°-rotation first (ask the user whether it's still
rotated after setting it `true`), then cycle `mirror()`'s two bools through
`(T,F)`/`(F,T)`/`(T,T)`/`(F,F)` one flash-and-look at a time. Each report narrows it —
e.g. "upside down AND mirrored" at one combo versus "only mirrored" at another tells you
which bit controls which screen axis for this board, and the 4th untested combo is usually
derivable from the other 3 without even flashing it. `PANEL_WIDTH`/`PANEL_HEIGHT` and the
`visualizer_render_init()` call in `main.c` must agree with whichever axis is "width" after
`swap_xy` — getting this backwards is a different bug (stride/aspect wrong) from mirroring.

`visualizer_metadata.c`'s layout is written as fractions of the `width`/`height` the drawer
callback receives, specifically so it survives this kind of orientation change without
needing its own fix.

## Working through the remote device bridge

If you're a Claude agent iterating on this board with the user connected via the Claude
desktop app's device bridge (`mcp__remote-devices__device_*` tools) rather than a normal
git remote:

- **`device_commit_files` can report `"written": [...]` success while the write silently
  did not persist.** This happened multiple times in this project's history, including
  twice in immediate succession while debugging the orientation issue above. Never trust
  that response alone for anything the user is about to spend a flash-and-reboot cycle
  verifying. After every push: `device_stage_files` the same path back and diff/grep its
  actual content against what you intended to write, *before* telling the user to rebuild.
  A matching byte count is not sufficient evidence either — two different one-line changes
  can coincidentally produce identical file sizes.
- `device_bash` on this bridge is a sandboxed companion environment with the connected
  folder mounted (under `~/mnt/<folder-name>` in that sandbox, not the user's literal Mac
  path), not a real shell on the user's Mac — no `gh`, often no outbound GitHub access, and
  it cannot delete files in the connected folder without `device_request_delete_permission`
  (move to a `_to_delete/` subfolder instead if a delete isn't worth spending that prompt).
  Anything that needs real GitHub credentials or push access has to happen either in your
  own cloud sandbox or by handing the user exact commands to run in their own real terminal.
- The MCP connection to the bridge can drop and reconnect mid-task (tools disappear from
  the list, then reappear after `RefreshMcpTools`) — this is transient, not a sign the user
  disconnected; retry rather than giving up.

## Code Quality

**Requirements**: ESP-IDF >= 5.5 (tested against v5.5.2). Older versions may need workarounds.

**Formatting**: LLVM-style, 2-space indent, 80-char column limit. See `.clang-format`.

**Linting**: clang-tidy with bugprone, performance, portability, and readability checks. See `.clang-tidy`.

**Pre-commit hook**: auto-formats staged C/H files with clang-format, runs clang-tidy (requires `build/compile_commands.json`). Install via `git config core.hooksPath .githooks`.

**CI** (`.github/workflows/ci-release.yml`): On push/PR to `main`:
- `format-check`: clang-format dry-run on all C/H files (excludes `components/u8g2`)
- `lint-check`: clang-tidy on build output (requires ESP-IDF v5.5 toolchain)
- `build`: compiles 4 target configs (esp32s3, squeezeamp-bt, squeezeamp-4m, esparagus-audio-brick-bt)
- `release`: auto-creates GitHub release with merged firmware bins (only on push)

**Local tooling** (in `scripts/`):
```bash
scripts/format.sh          # Format all C/H files (excludes u8g2 submodule)
scripts/lint.sh            # Run clang-tidy on all C/H files
scripts/lint.sh --fix      # Attempt to auto-fix clang-tidy issues
```

**No unit tests**: This is embedded firmware — no test framework is in place. Manual testing on hardware is required.

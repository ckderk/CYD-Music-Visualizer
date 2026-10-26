<div align="center">

# CYD Music Visualizer

**An AirPlay 2 speaker that doesn't play the music — it draws it.**

Shows up on your iPhone, iPad or Mac as a normal AirPlay speaker. Instead of playing the
audio out loud, it renders live audio-reactive visuals — spectrum bars, a scrolling
waveform, a particle flow, and a now-playing track screen — on its own 2.8" touchscreen.
Tap to cycle between modes. No speaker, no cable, nothing to carry around but the board
itself.

Runs on a **Freenove ESP32-S3 2.8" CYD (ES3N28P)** — about €15 of hardware.

[![License](https://img.shields.io/badge/license-Non--Commercial-blue?style=flat-square)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5+-red?style=flat-square)](https://docs.espressif.com/projects/esp-idf/)
[![Board](https://img.shields.io/badge/board-ESP32--S3%20CYD%20(ES3N28P)-green?style=flat-square)](docs/boards/cyd-es3n28p.md)

### [Board guide](docs/boards/cyd-es3n28p.md) · [Agent / contributor notes](CLAUDE.md)

</div>

---

## Quick start

```bash
git clone --recursive https://github.com/ckderk/CYD-Music-Visualizer
cd CYD-Music-Visualizer
pio run -e cyd-es3n28p -t upload
pio run -e cyd-es3n28p -t uploadfs   # required — writes the WiFi-setup page to SPIFFS
```

First boot: connect to the `ESP32-AirPlay-Setup` WiFi network, open `http://192.168.4.1`,
and set up WiFi. Once connected, the board appears under AirPlay with the name you gave
it. Tap the screen to cycle spectrum → waveform → particles → now-playing.

Full hardware details, pin reference, and how to install it via bmorcelli/Launcher instead
of flashing straight over USB: **[`docs/boards/cyd-es3n28p.md`](docs/boards/cyd-es3n28p.md)**.

## How it works

- Taps the real decoded PCM audio inside the AirPlay playback path (see `CLAUDE.md` for
  exactly where) and mirrors it into a lock-free ring buffer.
- Spectrum, waveform and particle modes draw straight into a raw RGB565 framebuffer via
  `esp_lcd` — no LVGL in that path at all, just FFT/pixel math against the ILI9341.
- The now-playing screen subscribes to the project's RTSP metadata events and renders
  title/artist/album/progress with a small hand-authored 5×7 bitmap font.
- No DAC is registered and no audio is actually played — this board has no speaker in this
  use case.

## Built on [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32)

This project exists because that one does the hard part extremely well: a maintained,
from-scratch **AirPlay 2** receiver for ESP32 — ALAC/AAC decoding, PTP timing, RTSP,
FairPlay, the works — with a clean single tap point for decoded PCM. All of that is used
here completely unmodified; this fork only adds the visualizer board on top.

Everything that project supports — SqueezeAMP, Esparagus Audio Brick, Bluetooth A2DP,
Ethernet, OLED/TFT displays on *other* boards, OTA, the lot — still builds and works from
this repo exactly as upstream intended. See
**[its documentation](https://rbouteiller.github.io/airplay-esp32/)** for all of that; it's
not duplicated here.

## Legal

**Non-commercial use only.** Commercial use requires explicit permission from the upstream
copyright holder — see [LICENSE](LICENSE). The CYD-visualizer-specific files added in this
fork (`components/visualizer*`, `components/boards/cyd-es3n28p`, and this README/CLAUDE.md)
are released under the same non-commercial terms.

This is an independent project based on protocol analysis. Not affiliated with Apple Inc.
Not guaranteed to work with future iOS or macOS versions. Provided as-is without warranty.

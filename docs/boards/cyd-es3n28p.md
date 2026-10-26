# CYD Visualizer (Freenove ESP32-S3 2.8" CYD, ES3N28P)

A **visualizer**, not a speaker: this board shows up on your iPhone as an AirPlay 2
destination, but instead of playing the audio it renders audio-reactive visuals —
spectrum bars, a scrolling waveform, a particle flow, and a now-playing metadata screen —
on its own 2.8" ILI9341 touchscreen. Cycle between the 4 modes by tapping the screen.

No speaker is connected and no DAC is registered — `audio_output_init()` /
`playback_task()` still run (the I2S pins are configured but nothing is listening on
them), which is harmless and keeps this board using the same audio pipeline as every
other board in this project.

## Hardware

- Chip: ESP32-S3, 16 MB flash, 8 MB OPI PSRAM.
- Display: 2.8" IPS, ILI9341V driver, 240×320 native (portrait), 4-line SPI — mounted
  landscape in the case, USB-C on the right.
- Touch: FT6336G capacitive controller, I2C address `0x38`, on the same bus as the (unused
  here) audio codec IC.
- RGB status LED (WS2812-style) on GPIO 42, used here as an AirPlay connection-status
  indicator.

Full pin table (LCD, touch, battery ADC, microSD, expansion headers) is in the project's
own porting notes — not duplicated here since it covers every peripheral on this board,
most of which this firmware doesn't use.

## Flashing

```bash
pio run -e cyd-es3n28p -t upload
pio run -e cyd-es3n28p -t uploadfs   # the WiFi-setup captive portal page lives in SPIFFS —
                                      # flashed separately from firmware, easy to forget
pio run -e cyd-es3n28p -t monitor
```

ESP-IDF refuses any project path containing a space — clone somewhere like `~/dev/` rather
than a folder name with spaces in it.

First boot, no WiFi credentials saved yet: connect to `ESP32-AirPlay-Setup`, open
`http://192.168.4.1`, and set up WiFi from there. Once connected, the board advertises
itself over AirPlay 2 under whatever device name you configured.

## Modes

Tap anywhere on the screen to cycle: **spectrum → waveform → particles → metadata → spectrum...**

The first 3 modes draw straight into a raw RGB565 framebuffer via `esp_lcd`, bypassing
LVGL entirely — there's no LVGL display stack coexisting with this board's screen at all.
The metadata screen uses the same framebuffer and a small hand-authored 5×7 bitmap font
(built from scratch rather than reproducing a known font table), subscribing to this
project's existing RTSP event system for title/artist/album/progress.

## Installing via Launcher

This firmware can also run as an app on an SD card under
[bmorcelli/Launcher](https://github.com/bmorcelli/M5Stick-Launcher), the same way other
ports for this board (Marauder, PorkChop) do. Launcher's installer reads an embedded ESP32
partition table at offset `0x8000` and dynamically creates matching partitions from it, so
any **merged** binary works regardless of what build tool produced it:

```bash
esptool.py --chip esp32s3 merge_bin -o cyd-visualizer-merged.bin \
  --flash_mode qio --flash_size 16MB \
  0x0     .pio/build/cyd-es3n28p/bootloader.bin \
  0x8000  .pio/build/cyd-es3n28p/partitions.bin \
  0x10000 .pio/build/cyd-es3n28p/firmware.bin
```

Drop the merged file on the SD card and use Launcher's **Install** option on it.

Flashing straight over USB during development (`pio run -t upload`) overwrites Launcher on
the board — expected and easily reversed by reflashing Launcher itself and then installing
this firmware's merged `.bin` from the SD card as above.

## Related

- [`CLAUDE.md`](../../CLAUDE.md#cyd-visualizer-es3n28p) — architecture, the real audio tap
  point, and the display-orientation fix, for anyone (human or agent) changing this code
- [Build environments](../reference/build-environments.md)

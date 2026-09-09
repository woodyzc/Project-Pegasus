# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Project Pegasus is a **PlatformIO** firmware project targeting an **ESP32-S3** MCU (Arduino framework), driving a 2.8" ST7789 SPI TFT display (240×320) and reading GPS data via a module parsed with `TinyGPSPlus`. The intended device is a handheld unit with a color display and GPS (e.g. a tracker/dashboard-style gadget).

The codebase is currently at the PlatformIO scaffold stage: `src/main.cpp` is still the default generated template (unused `myFunction`, empty `setup()`/`loop()`), and `include/`, `lib/`, `test/` contain only PlatformIO's placeholder READMEs. There is no application logic yet — display, GPS, and UI code all remain to be written.

## Commands

This project uses the PlatformIO CLI (`pio`). All commands target the single defined environment, `esp32s3`.

```sh
pio run                      # build
pio run -t upload            # build and flash to the connected board
pio run -t clean             # clean build artifacts
pio device monitor -b 115200 # open serial monitor (matches monitor_speed)
pio test                     # run PlatformIO/Unity tests from test/
pio test -f <test_name>      # run a single test (matches test dir/file name)
```

There is only one environment (`esp32s3`), so `-e` is not needed but can be passed explicitly (`pio run -e esp32s3`) if more environments are added later.

## Architecture / configuration notes

All hardware wiring and board configuration lives in `platformio.ini`, not in code — there is no `User_Setup.h`. Key points to know before writing display or GPS code:

- **Board/platform**: `espressif32 @ ^6.6.0` pinned deliberately (per the in-file comment) for build stability; board is the generic `esp32-s3-devkitc-1` ID rather than a vendor-specific board, chosen for broad compatibility.
- **PSRAM**: `BOARD_HAS_PSRAM` is enabled via build flag — required headroom for any graphics/UI library work on this display.
- **USB serial**: `ARDUINO_USB_CDC_ON_BOOT=1` routes serial output over the native USB-C port (not a separate UART-to-USB chip), so `Serial` output shows up over the board's Type-C connection directly.
- **Display driver config**: The ST7789 driver and pin mapping (`TFT_MOSI=45`, `TFT_SCLK=40`, `TFT_CS=42`, `TFT_DC=41`, `TFT_RST=39`, `TFT_BL=5`, `SPI_FREQUENCY=27000000`) are defined as build flags in the classic **TFT_eSPI** `USER_SETUP_LOADED=1` style, matching a Waveshare 2.8" reference schematic. **However, `TFT_eSPI` is not currently listed in `lib_deps`** — only `mikalhart/TinyGPSPlus @ ^1.0.3` is. Adding display code will require adding the `TFT_eSPI` library dependency for these build flags to take effect.
- **GPS**: `TinyGPSPlus` is pulled in via `lib_deps` for parsing NMEA data from the GPS module (board comments reference an M10S module), but no serial port/pins for it are wired up yet in `platformio.ini` or code.

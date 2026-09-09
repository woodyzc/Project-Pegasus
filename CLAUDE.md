# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# CLAUDE.md - Project Pegasus Development & Agent Guide

## 1. Project Overview & Master Specification
- **Project Name**: Project Pegasus (DIY Open-Source GPS Bike Computer)
- **Version**: v1.0
- **Primary Goals**: High modularity, power efficiency, minimal hardware complexity, and software-driven functionality.
- **Reference Spec**: For full architectural details, consult `PROJECT_PEGASUS_SPEC_v1.0.md`.

## 2. Hardware Architecture & Pinout Specifications
- **Core MCU & Display**: Waveshare ESP32-S3-Touch-LCD-2.8 (Dual-Core 240MHz, 16MB Flash, PSRAM, 2.8" ST7789 Touch LCD).
- **GNSS Module**: u-blox MAX-M10S connected via dedicated UART (GPIO43/44).
  - *Constraint*: Force UBX binary protocol only; disable high-overhead NMEA text parsing.
  - *Power*: Retain micro-power RTC backup (~15μA) for <1s hot starts.
- **IMU Sensor**: Onboard QMI8658 6-axis IMU (I2C).
  - *Uses*: Motion detection, inclination/slope calculation, anti-theft alarm, fall detection, and Any-Motion wake-up triggers.
- **Power & Control**:
  - Onboard `BAT` Button (GPIO Interrupt): Soft-switch for manual Deep Sleep entry and wake-up.
  - Power Subsystem: Target ~200μA standby current in Deep Sleep (5–6 months standby on 1000mAh battery).
- **Audio Output**: Onboard PCM5101 I2S decoder & speaker for key clicks, off-route alerts, and turn prompts.

## 3. Wireless Connectivity & Sensor Decoding
- **Pure Software ANT+ (Primary for ANT+ Straps)**:
  - Utilize ESP32-S3 2.4GHz PHY via `esp32-ant` software library.
  - Soft-decode 2.4GHz ANT+ broadcast packets in a dedicated FreeRTOS task on Core 0 to extract BPM from Device Type `0x78`.
  - *No external SPI ANT+/NRF24 hardware required*.
- **BLE Client (Secondary / Galaxy Watch 8)**:
  - Run `NimBLE-Arduino` on Core 0 for point-to-point connection to standard BLE Heart Rate Service (`0x180D`).

## 4. Software Architecture & FreeRTOS Core Rules
- **Core 0 (Background Data Core)**:
  - Task 1: MAX-M10S UBX parsing with low-speed anti-drift and Kalman filtering.
  - Task 2: Software ANT+ / NimBLE BLE client reception.
  - Task 3: Power & IMU monitoring (detect 5-min inactivity to trigger Deep Sleep).
- **Core 1 (UI & Life Cycle Core)**:
  - Task 1: LVGL rendering loop (`lv_timer_handler()`).
  - Task 2: X-TRACK `PageManager` life cycle management.
- **Data Bus Architecture**:
  - Strictly enforce X-TRACK `DataCenter` (Pub/Sub message bus).
  - All inter-core communication and sensor updates must publish to `DataCenter` topics (e.g., `Sensor/HeartRate`, `GPS_Info`), never via direct cross-thread function calls.

## 5. Navigation Strategy
- **BLE Turn-by-Turn**: Accept turn arrows and distance metrics pushed over BLE from mobile app.
- **Offline Breadcrumb Navigation**: Read `.gpx` files from SD card and render breadcrumb trails on LVGL canvas.

## 6. Open-Source Reference Repositories (`deps/`)
- **X-TRACK** (`deps/X-TRACK`): Core framework for `PageManager`, `DataCenter`, `Page_Dashboard`, and GPX breadcrumb rendering.
- **LilyGO T-Watch / ESP32-S3 Repos**: Reference for ST7789/FT6336 LVGL drivers and QMI8658 motion wake-up algorithms.
- **OpenBikeComputer**: Reference for UBX binary parsing and Kalman filter GPS algorithms.
- **esp32-ant**: Reference for 2.4GHz PHY software ANT+ decoding.

## 7. Agent Code Generation & Build Rules
1. **Compilation Validation**: Always run `pio run` after creating or modifying code to verify zero build errors.
2. **Asynchronous & Non-Blocking**: Do NOT use blocking `delay()` calls; rely on FreeRTOS tasks and `vTaskDelay()`.
3. **Power-Safe Storage**: Before entering Deep Sleep, always flush telemetry buffers and safely unmount the SD card.

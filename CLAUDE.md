# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# CLAUDE.md - Project Pegasus Development & Agent Guide

## 1. Project Overview & Master Specification
- **Project Name**: Project Pegasus (DIY Open-Source GPS Bike Computer)
- **Version**: v1.0
- **Primary Goals**: High modularity, power efficiency, minimal hardware complexity, and software-driven functionality.
- **Reference Spec**: For full architectural details, consult `PROJECT_PEGASUS_SPEC_v1.0.md`.

## 2. Hardware Architecture & Pinout Specifications

> ⚠️ **The board on the bench is NOT the board specified below.** Development
> currently runs on a **Hosyond ESP32-S3 2.8" (ES3C28P reference design)**,
> bought as a stand-in until the Waveshare unit arrives. It differs in the
> display controller and every display/touch pin. `platformio.ini` on the
> `board/hosyond-esp32-s3-2.8` branch is the authority on what is actually
> wired; this section describes the *target* hardware.
>
> Confirmed on the Hosyond board (vendor docs + `esptool`): ILI9341V panel,
> SPI on MOSI 11 / SCLK 12 / CS 10 / DC 46, backlight IO45 active-high, no
> panel reset line; FT6336G touch on I2C SDA 16 / SCL 15, INT 17, **RST 18**;
> 16MB flash and **8MB octal PSRAM** ("Embedded PSRAM 8MB (AP_3v3)"); battery
> sense on **GPIO9** through a 2:1 divider with a TP4054 charger; microSD on
> **SDIO** (CLK 38, CMD 40, DATA 39/41/47/48) — so `SD_MMC`, not the SPI `SD`
> library.
>
> Items below marked *(target board only)* have **not** been confirmed to
> exist on the Hosyond board. Do not assume they are present.

- **Core MCU & Display**: Waveshare ESP32-S3-Touch-LCD-2.8 (Dual-Core 240MHz, 16MB Flash, PSRAM, 2.8" ST7789 Touch LCD).
- **GNSS Module**: u-blox MAX-M10S connected via dedicated UART (GPIO43/44).
  - *Constraint*: Force UBX binary protocol only; disable high-overhead NMEA text parsing.
  - *Power*: Retain micro-power RTC backup (~15μA) for <1s hot starts.
- **IMU Sensor** *(target board only)*: Onboard QMI8658 6-axis IMU (I2C).
  - *Uses*: Motion detection, inclination/slope calculation, anti-theft alarm, fall detection, and Any-Motion wake-up triggers.
  - `Page_Dashboard` already renders grade from `IMU_Data_t.pitch`, so the
    INCLINE field stays blank until an IMU exists and publishes.
- **Power & Control**:
  - Onboard `BAT` Button (GPIO Interrupt) *(target board only)*: Soft-switch for manual Deep Sleep entry and wake-up.
  - Power Subsystem: Target ~200μA standby current in Deep Sleep (5–6 months standby on 1000mAh battery).
- **Audio Output** *(target board only)*: Onboard PCM5101 I2S decoder & speaker for key clicks, off-route alerts, and turn prompts.

## 3. Wireless Connectivity & Sensor Decoding

> The "ANT+ primary, BLE secondary" arrangement below was **replaced by a
> mutually exclusive choice**, because the two cannot coexist as built:
> `SoftANT_Start(false)` hands the BLE controller to `esp32-ant` as a raw ANT
> modem, leaving no NimBLE host. The user picks one in Settings, and since
> both are init-time radio configurations, a change only applies on restart.
>
> That choice also gates navigation, enforced as a single rule in
> `Settings.h`: **never (NAV_MODE_TBT and HR_SOURCE_ANT)**. Turn-by-turn is a
> NimBLE GATT server, so choosing ANT+ forces navigation to GPX, and choosing
> TBT forces heart rate to BLE. Both setters repair the conflict and the UI
> reports which setting moved.

- **Pure Software ANT+ (one of two exclusive heart-rate sources)**:
  - Utilize ESP32-S3 2.4GHz PHY via `esp32-ant` software library.
  - Soft-decode 2.4GHz ANT+ broadcast packets in a dedicated FreeRTOS task on Core 0 to extract BPM from Device Type `0x78`.
  - *No external SPI ANT+/NRF24 hardware required*.
- **BLE Client (the other exclusive source / Galaxy Watch 8)**:
  - Run `NimBLE-Arduino` on Core 0 for point-to-point connection to standard BLE Heart Rate Service (`0x180D`).
  - Defaults to this source. ANT+ has never been exercised on hardware, and a
    hang during its bring-up would strand the user on a dead screen with the
    setting unreachable — hence also the NVS bring-up watchdog in
    `Settings_Init()`, which reverts to BLE if a previous boot never completed.

## 4. Software Architecture & FreeRTOS Core Rules
- **Core 0 (Background Data Core)**:
  - Task 1: MAX-M10S UBX parsing with low-speed anti-drift and Kalman filtering.
  - Task 2: Software ANT+ / NimBLE BLE client reception.
  - Task 3: Power & IMU monitoring (detect 5-min inactivity to trigger Deep Sleep).
  - *Known exception — the `esp32-ant` radio task runs on Core 1.* `ant_node`'s
    own receive task (priority `configMAX_PRIORITIES-2`, holds the 32768Hz ANT
    TDMA grid) stays on the library's Core 1 default: Core 0 hosts the BT
    controller that this task hooks, so pinning it there makes a max-priority
    task contend with its own controller, and Core 1 is the configuration the
    library verified live against a real strap. It does not disturb the LVGL
    loop, because in receive mode it blocks in `ulTaskNotifyTake()` rather than
    busy-waiting (it only spins for sub-millisecond *transmit* deadlines, and we
    are a receive-only slave). Our own supervisory task (`SoftANT_Task`) and all
    DataCenter publishing still run on Core 0, which is what this rule is about.
    See the rationale block in `src/sensors/SoftANT.cpp`.
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
Vendored as git submodules. Most are reference only, but **`deps/esp32-ant`
is now a real build dependency**, pulled in via
`symlink://deps/esp32-ant/components/ant` in `platformio.ini` — so the
submodule must be initialised (`git submodule update --init`) or the firmware
will not link. `deps/X-TRACK`'s `PageManager` and `DataCenter` have been
*ported into* `src/` rather than linked; the submodule remains the reference
for both. Each submodule's responsibility:

- **`deps/X-TRACK`** ([FASTSHIFT/X-TRACK](https://github.com/FASTSHIFT/X-TRACK)): Extract `DataCenter` (Pub/Sub message bus), `PageManager` page life-cycle management, and breadcrumb-trail rendering.
- **`deps/NimBLE-Arduino`** ([h2zero/NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)): Low-power BLE client for connecting to the Galaxy Watch 8 / a standard BLE heart-rate strap (`0x180D`), and for receiving turn-by-turn (TBT) navigation data pushed from the phone app.
- **`deps/esp32-ant`** ([RaemondBW/esp32-ant](https://github.com/RaemondBW/esp32-ant)): Soft-decode ANT+ heart-rate data (Device Type `0x78`) directly off the ESP32-S3's 2.4GHz PHY — no external hardware required.
- **`deps/SparkFun_u-blox_GNSS`** ([sparkfun/SparkFun_u-blox_GNSS_Arduino_Library](https://github.com/sparkfun/SparkFun_u-blox_GNSS_Arduino_Library)): Drive the u-blox MAX-M10S module, configure pure UBX binary protocol output, and parse high-precision fix data.
- **`deps/Kalman`** ([balzer82/Kalman](https://github.com/balzer82/Kalman)) and **`deps/Arduino-KalmanFilter`** ([nhatuan84/Arduino-KalmanFilter](https://github.com/nhatuan84/Arduino-KalmanFilter)): Reference for the low-speed anti-drift Kalman-filter logic applied to GPS fixes.
- **`deps/uBloxGPS`** ([SquirrelEng/uBloxGPS](https://github.com/SquirrelEng/uBloxGPS)): Lightweight reference for decoding the UBX binary `NAV-PVT` message directly (no NMEA parsing, smaller footprint than TinyGPS) — the UBX-parsing half of the original `OpenBikeComputer` request.
- **`deps/Waveshare-LCD-2.8`** ([FatihErtugral/esp32s3-waveshare-2.8-touch-lcd](https://github.com/FatihErtugral/esp32s3-waveshare-2.8-touch-lcd)): Reference for driving the ST7789 screen, FT6336 touch panel, and QMI8658 IMU on this exact board. No official `waveshareteam` repo exists for the 2.8" ESP32-S3 board specifically — this is a third-party reference, confirmed with the user.

Together, `deps/Kalman` + `deps/Arduino-KalmanFilter` + `deps/uBloxGPS` replace the originally-requested `deps/OpenBikeComputer` submodule, whose UBX/Kalman-filter code couldn't be located under that repo name (neither [timohueser/OpenBikeComputer](https://github.com/timohueser/OpenBikeComputer) nor [Random90/OpenBikeComputerRTOS_ESP32](https://github.com/Random90/OpenBikeComputerRTOS_ESP32) actually contains it).

## 7. Agent Code Generation & Build Rules
1. **Compilation Validation**: Always run `pio run` after creating or modifying code to verify zero build errors.
2. **Asynchronous & Non-Blocking**: Do NOT use blocking `delay()` calls; rely on FreeRTOS tasks and `vTaskDelay()`.
3. **Power-Safe Storage**: Before entering Deep Sleep, always flush telemetry buffers and safely unmount the SD card.

## 8. Bench Realities (read before debugging hardware)

These were each discovered the slow way. They are not optional trivia.

- **Serial is unusable over USB-Serial-JTAG on the dev Mac.** Opening
  `/dev/cu.usbmodem*` toggles CDC control lines that map to `EN`/`GPIO0`, so
  the host reboots the chip into download mode (`waiting for download`)
  instead of reading it. `pyserial` asserting DTR/RTS by default holds the
  chip in reset outright. A "silent board" is far more often this than a
  firmware fault. Workarounds: wire **UART0 (GPIO43/44)** to a USB-TTL
  adapter — note that collides with the planned GNSS UART — or **print
  diagnostics to the LCD panel**, which is what actually worked.
- **`pio` is not on `PATH`.** Use `~/.platformio/penv/bin/pio`.
- **Three build flags are load-bearing.** Removing any one produces a
  confusing failure a long way from the cause:
  - `-D LV_CONF_INCLUDE_SIMPLE` **and** `-I include` — both, or LVGL compiles
    against upstream defaults while `src/` sees your `lv_conf.h`. Symptom is a
    link error on `lv_font_montserrat_24`, not a config warning.
  - `-D USE_HSPI_PORT` — without it TFT_eSPI's S3 branch shares the Arduino
    global `SPI` object and `tft.begin()` panics
    (`esp_reset_reason() == ESP_RST_PANIC`): black screen, endless reboot.
  - `-D TOUCH_RST_PIN=18` — the FT6336G answers nothing on I2C until its
    reset line is driven high.
- **A setting that hangs at boot outlives a reflash**, because it lives in
  NVS. `Settings_Init()`'s bring-up watchdog exists for exactly this; do not
  remove it when adding radio modes.
- **Register every GATT service before anything scans or connects.**
  NimBLE's `ble_gatts_mutable()` refuses to add a service while an
  advertisement, a scan, a connection attempt or an established connection
  exists, and the refusal is *not* returned to the caller — it lands on
  `SYSINIT_PANIC_ASSERT` inside `ble_svc_gap_init()` and panics the chip. So
  `BLE_TBT_Start()` must precede `BLE_HR_Start()` in `main.cpp`. It only
  misbehaves when a heart-rate peer is actually in range, which makes it look
  like flaky hardware: with no watch nearby nothing connects and the identical
  code registers fine.
- **PageManager caches a page, so `onViewLoad()` reads the world once.**
  `IsCached` defaults true, so a page built during `setup()` keeps whatever was
  true at `Push()` for the life of the boot — it is *not* rebuilt when the
  rider navigates back to it. Anything a page reads at load time must therefore
  already be initialised before its `Push()`. This is why `GpxTrack_MountCard()`
  runs before the first push: it used to run after the radios, and the
  dashboard showed "No SD card" all session while the ROUTE page — pushed
  minutes later by the rider — drew the route correctly off the same card.
  Two screens disagreeing about one piece of hardware is the signature of this
  bug, not of flaky hardware.
- **Never advertise while the heart-rate client is connecting.** The two BLE
  modules share one controller, and asking it to advertise while it stops a
  scan and initiates a link makes an HCI command miss its ack deadline. NimBLE
  answers a missed ack by resetting its host — and `ble_hs_reset()` does not
  cancel the host timer, so that timer fires during the re-sync and hits
  `assert(0)` in `ble_hs_timer_exp`. The chip aborts. That last step is a
  NimBLE defect and we are already on its newest release, so the *load* is the
  only thing we can remove: `BLE_HR_Client` brackets every connect attempt with
  `BLE_TBT_PauseAdvertising()` / `BLE_TBT_ResumeAdvertising()`.
  Scanning while advertising is fine and runs for minutes — it is specifically
  the connect that must be alone. Registration has the *opposite* constraint
  (see `BLE_TBT_Receiver.h`), which is why `BLE_TBT_Start()` and
  `BLE_TBT_StartAdvertising()` are separate calls straddling `BLE_HR_Start()`.
- **The board records its own crashes, and you can read them.** Serial is
  unusable (above), but `esp_reset_reason()` now surfaces on the Settings page,
  and the `coredump` partition at `0xFF0000` holds a full ELF core dump written
  on every panic. Read it *without* the serial console:

  ```
  esptool.py --chip esp32s3 --port /dev/cu.usbmodem101 \
      read_flash 0xFF0000 0x10000 coredump.bin
  # strip the 20-byte header, then:
  xtensa-esp32s3-elf-gdb -batch .pio/build/esp32s3/firmware.elf \
      -c core.elf -ex "thread 1" -ex bt
  ```

  This gave the exact assert and the full backtrace for the bug above, after
  three rounds of guessing had failed. Reach for it first, not last.

## 9. Companion App (`phone/android/`)

A Kotlin app that scrapes Google Maps' navigation notification and writes
turn-by-turn frames to the head unit over BLE — Maps exposes no API, so the
notification is the only route without root. See its own README.

**`src/navigation/TbtParse.h` is the authority on the wire format**; the app's
`TbtFrame.kt` is an encoder for it, and `TbtFrameTest` pins the two together
byte for byte. Change one side and that test should be what notices.

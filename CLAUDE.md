# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# CLAUDE.md - Project Pegasus Development & Agent Guide

## 1. Project Overview & Master Specification
- **Project Name**: Project Pegasus (DIY Open-Source GPS Bike Computer)
- **Version**: v1.0
- **Primary Goals**: High modularity, power efficiency, minimal hardware complexity, and software-driven functionality.
- **Reference Spec**: For full architectural details, consult `PROJECT_PEGASUS_SPEC_v1.0.md`.

## 2. Hardware Architecture & Pinout Specifications

> **The real board arrived on 2026-09-15** and development moved to
> `board/waveshare-esp32-s3-2.8`. The stand-in — a Hosyond ESP32-S3 2.8"
> (ES3C28P) with an ILI9341V panel and an FT6336G — is preserved on
> `board/hosyond-esp32-s3-2.8`; nothing on this branch should refer to its
> pins. `platformio.ini` is the authority on what is wired.
>
> ⚠️ **The pinout below came from the vendor's driver source, not the wiki.**
> Waveshare's wiki publishes no full GPIO table for this board; the only
> pins it states outright are the 12-pin external connector (SCL 10 / SDA 11
> / TXD 43 / RXD 44, spare 15 and 18). Everything else is read out of
> [`jeffvan302/WS_ESP32_Touch28`](https://github.com/jeffvan302/WS_ESP32_Touch28)
> `src/` — Waveshare's own Arduino demo tidied into a library — which agrees
> with the wiki everywhere the two overlap.
>
> ⚠️ **`deps/Waveshare-LCD-2.8` is the wrong board.** See §6. It documents
> the **2.8B**, which is a different product, not a revision of this one.
>
> Nothing below is confirmed against this hardware yet: it is a paper port.
> Items marked *(present, not yet driven)* are wired on the board but have no
> firmware behind them on this branch.

- **Core MCU & Display**: Waveshare ESP32-S3-Touch-LCD-2.8 V1 (ESP32-S3R8, dual-core 240MHz, 16MB Flash, 8MB octal PSRAM, 2.8" 240×320 **ST7789T3** IPS LCD on SPI).
  - MOSI 45, SCLK 40, CS 42, DC 41, **RST 39** (a real reset line, unlike the stand-in), backlight 5 active-high. Vendor drives the panel at 80MHz.
- **Touch**: **CST328** (Hynitron), I2C address `0x1A`, on its **own bus** — SDA 1 / SCL 3, INT 4, RST 2.
  - *Not an FT6336G with different pins.* 16-bit big-endian register addresses, two 12-bit coordinates packed into three bytes, and a touch-count register that latches until written back to zero. `src/hal/Touch.cpp` is a rewrite, not a re-pin.
  - A V2 of this board exists with a **CST3530** instead. If touch never answers, check which one is fitted before checking the wiring.
- **GNSS Module**: u-blox MAX-M10S via UART1 on **RX 18 / TX 15** — the only two spare GPIOs on the board.
  - *Not* GPIO43/44 as the original spec said: that is UART0, and §8 explains why it has to stay free.
  - *Constraint*: Force UBX binary protocol only; disable high-overhead NMEA text parsing.
  - *Power*: Retain micro-power RTC backup (~15μA) for <1s hot starts.
- **Sensor I2C bus** — SDA 11 / SCL 10, separate from the touch bus, and untouched by firmware so far:
  - **IMU** *(present, not yet driven)*: QMI8658 6-axis at `0x6B`. Motion detection, inclination/slope, anti-theft alarm, fall detection, Any-Motion wake.
    - `Page_Dashboard`'s ELEVATION cell carries both readings: `NOW` is grade
      from `IMU_Data_t.pitch` and reads "--" until an IMU publishes, `GAIN` is
      total ascent from `Ascent.h`. Two equal rows rather than one large figure
      over a small one, because which of the pair matters depends on the board,
      and a ranking baked into the layout would be wrong on one of them. This
      board is the first to have the hardware to fill `NOW`.
  - **RTC** *(present, not yet driven)*: PCF85063 at `0x51`, battery-backed.
- **Storage**: microSD on **SDIO, 1-bit only** — CLK 14, CMD 17, D0 16. GPIO21 is *not* a data line: it is a plain enable that firmware must drive high before the card answers. `SD_MMC`, never the SPI `SD` library.
- **Power & Control**:
  - **Power latch on GPIO7 — load-bearing.** On battery the rail is held up by a soft latch, and it stays up only because firmware drives GPIO7 high. `BoardPower_Init()` is the first line of `setup()` for this reason. Invisible on USB, where the host holds the rail up regardless; it bites the first time the cable comes out. Driving it low is what powers the board off.
  - Power key on GPIO6 *(present, not yet driven)*: the vendor reads it for long-press sleep / restart / shutdown. Until that lands there is **no software path to switch this board off**.
  - Battery sense on **GPIO8 through a 3:1 divider** (the stand-in's was 2:1), with a ~0.99 trim the vendor applies. Target ~200μA standby in Deep Sleep (5–6 months on 1000mAh).
- **Audio Output** *(present, not yet driven)*: onboard PCM5101 I2S decoder & speaker — DOUT 47, BCLK 48, LRCK/WS 38 — for key clicks, off-route alerts, and turn prompts.

## 3. Wireless Connectivity & Sensor Decoding

> **Software ANT+ was removed on 2026-09-13, at the user's request.** They use
> a BLE heart-rate monitor and do not need it. What it cost while it existed is
> worth remembering, because it shaped a lot of this file: it could not coexist
> with NimBLE (`SoftANT_Start(false)` handed the BLE controller to `esp32-ant`
> as a raw modem, leaving no host), so heart rate and navigation had to be a
> mutually exclusive pair enforced in `Settings.h`, and turn-by-turn could not
> run at all in ANT+ mode. All of that is gone. Navigation is now a free
> choice, and `git log` before that commit is the record if it ever comes back.
>
> It also never worked. See section 8: the radio and its TDMA grid came up, but
> no ANT+ transmitter was ever in range to prove the decode, because the strap
> on the bench is a 5.3kHz analog treadmill unit.

- **BLE Client (the only heart-rate source)**:
  - Run `NimBLE-Arduino` on Core 0 for point-to-point connection to a standard
    BLE Heart Rate Service (`0x180D`) peer — a strap or the Galaxy Watch 8 —
    and subscribe to Heart Rate Measurement (`0x2A37`).
  - Discovery is separated from reconnection on purpose: `BLE_HR_Start()`
    scans once to learn the peer's address, and every later reconnect dials it
    directly. That split was originally forced by ANT+ coexistence and stays
    because it is better behaviour on its own — it does not put a scan on the
    controller beside a connection attempt, which is the load section 8 warns
    about.
  - The parsing half is `src/sensors/BleHrParse.c`, host-tested with no NimBLE
    dependency.
- **BLE Turn-by-Turn**: a NimBLE GATT server the phone writes into. Chosen in
  Settings against offline GPX, and since both are init-time radio
  configurations a change applies on restart. The NVS bring-up watchdog in
  `Settings_Init()` guards this setting now — it used to guard the heart-rate
  source — and falls back to GPX, which starts no radio at all.

## 4. Software Architecture & FreeRTOS Core Rules
- **Core 0 (Background Data Core)**:
  - Task 1: MAX-M10S UBX parsing with low-speed anti-drift and Kalman filtering.
  - Task 2: NimBLE BLE client reception (heart rate).
  - Task 3: Battery monitoring. The idle/sleep half of this is **not** a Core 0
    task: it changes the backlight and puts a widget on screen, and LVGL here
    has no lock, so `PowerManager` runs on an LVGL timer instead
    (`src/system/PowerManager.h`). The decision logic is pure and host-tested in
    `src/system/IdlePolicy.h`; deep sleep is opt-in because the wake source has
    never been proven. IMU-based motion detection is still absent — but as of
    the Waveshare board that is an omission rather than a hardware limit: the
    QMI8658 is on the sensor bus (§2) waiting to be driven.
- **Core 1 (UI & Life Cycle Core)**:
  - Task 1: LVGL rendering loop (`lv_timer_handler()`).
  - Task 2: X-TRACK `PageManager` life cycle management.
- **Data Bus Architecture**:
  - Strictly enforce X-TRACK `DataCenter` (Pub/Sub message bus).
  - All inter-core communication and sensor updates must publish to `DataCenter` topics (e.g., `Sensor/HeartRate`, `GPS_Info`), never via direct cross-thread function calls.

## 5. Navigation Strategy
- **BLE Turn-by-Turn**: Accept turn arrows and distance metrics pushed over BLE from mobile app.
- **Offline Breadcrumb Navigation**: Read `.gpx` files from SD card and render breadcrumb trails on LVGL canvas.
- **Cached-route fallback**: the phone uploads the whole planned route once at
  ride start, and the head unit navigates from it when the phone stops talking.
  Two wire formats, both with pure host-tested decoders:
  `src/navigation/TbtParse.h` for a live turn, `src/navigation/RouteParse.h`
  for the route download. The geometry is `RouteFollow.h`, the PSRAM store and
  the source arbitration are `NavRoute.h`.

  Three rules there are load-bearing and each cost something to learn:
  - **The handover is a timeout on DATA, not on the connection.** A BLE link
    that is up but silent is exactly as useless to the rider as one that is
    down, and this hardware produces that state.
  - **Snap to the polyline and measure ALONG it.** Nearest-maneuver-in-a-
    straight-line fails on an out-and-back, where the rider is metres from a
    turn they will not reach for an hour. `test_route_parse.c` has that case.
  - **The onboard path needs the same 10s keepalive the phone path has.**
    `Page_Dashboard` drops a turn it has not heard about for 30s, so publishing
    only on change blanks the panel for a rider stopped at a light -- which is
    exactly when they are looking at it.

## 6. Open-Source Reference Repositories (`deps/`)
Vendored as git submodules, reference only — nothing under `deps/` is a build
dependency any more. `deps/esp32-ant` was the one exception and it was removed
with ANT+. `deps/X-TRACK`'s `PageManager` and `DataCenter` have been *ported
into* `src/` rather than linked; the submodule remains the reference for both.
Each submodule's responsibility:

- **`deps/X-TRACK`** ([FASTSHIFT/X-TRACK](https://github.com/FASTSHIFT/X-TRACK)): Extract `DataCenter` (Pub/Sub message bus), `PageManager` page life-cycle management, and breadcrumb-trail rendering.
- **`deps/NimBLE-Arduino`** ([h2zero/NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)): Low-power BLE client for connecting to the Galaxy Watch 8 / a standard BLE heart-rate strap (`0x180D`), and for receiving turn-by-turn (TBT) navigation data pushed from the phone app. Pulled from the registry at `^2.2.3`, not from this submodule.
- **`deps/SparkFun_u-blox_GNSS`** ([sparkfun/SparkFun_u-blox_GNSS_Arduino_Library](https://github.com/sparkfun/SparkFun_u-blox_GNSS_Arduino_Library)): Drive the u-blox MAX-M10S module, configure pure UBX binary protocol output, and parse high-precision fix data.
- **`deps/Kalman`** ([balzer82/Kalman](https://github.com/balzer82/Kalman)) and **`deps/Arduino-KalmanFilter`** ([nhatuan84/Arduino-KalmanFilter](https://github.com/nhatuan84/Arduino-KalmanFilter)): Reference for the low-speed anti-drift Kalman-filter logic applied to GPS fixes.
- **`deps/uBloxGPS`** ([SquirrelEng/uBloxGPS](https://github.com/SquirrelEng/uBloxGPS)): Lightweight reference for decoding the UBX binary `NAV-PVT` message directly (no NMEA parsing, smaller footprint than TinyGPS) — the UBX-parsing half of the original `OpenBikeComputer` request.
- **`deps/Waveshare-LCD-2.8`** ([FatihErtugral/esp32s3-waveshare-2.8-touch-lcd](https://github.com/FatihErtugral/esp32s3-waveshare-2.8-touch-lcd)): ⚠️ **This is the wrong board, and this entry used to claim otherwise.** It was vendored as "the ST7789 / FT6336 / QMI8658 reference for this exact board". It is neither: it targets the **ESP32-S3-Touch-LCD-2.8B**, a different product with a 480×640 **ST7701 RGB parallel** panel, **GT911** touch, and a **TCA9554** I/O expander holding LCD reset, LCD CS, touch reset and SD power. Not one display or touch pin is shared with the board we have, and TFT_eSPI cannot drive an RGB parallel panel at all. The mistake cost nothing only because it was caught before the port started; the lesson is that "2.8" and "2.8B" are product names, not revisions. Still useful for the QMI8658 and PCF85063 register work, which the two boards do share. **For this board, the reference is [`jeffvan302/WS_ESP32_Touch28`](https://github.com/jeffvan302/WS_ESP32_Touch28)** — Waveshare's own Arduino demo as a library, and the source of every pin in §2. It is not vendored; the pins are transcribed into `platformio.ini` with attribution.

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
  - `-D TOUCH_RST_PIN=2` — the CST328 answers nothing on I2C until it is
    given a real reset pulse (high, low 5ms, high). Was pin 18 and an FT6336G
    on the stand-in board; the flag survived the port, the pin did not.
- **A setting that hangs at boot outlives a reflash**, because it lives in
  NVS. `Settings_Init()`'s bring-up watchdog exists for exactly this; do not
  remove it when adding radio modes.

---

**Carried over to the Waveshare board (2026-09-15), and not yet re-verified
there.** Everything above this line was learned on the Hosyond stand-in. The
LVGL, TFT_eSPI and NimBLE items are firmware-level and should hold unchanged;
the serial item is a property of the ESP32-S3's USB-Serial-JTAG peripheral
rather than of any one carrier board, so expect it to hold too. Below are the
things the new board adds.

- **GPIO7 is a power latch, and nothing on battery works without it.** The
  rail is closed by the power key and held closed only by firmware driving
  GPIO7 high; release the key before that happens and the board switches off
  mid-boot. This is why `BoardPower_Init()` is the first statement in
  `setup()`, ahead of the display. It is **completely invisible on USB**,
  where the host holds the rail up regardless — so a board that works
  perfectly on the bench and dies the moment it is unplugged is this, not a
  battery fault. The corollary, until the power key is driven: there is no
  software way to switch this board off.
- **The CST328 latches its touch count until you clear it.** Register `0xD005`
  holds the point count and does not reset on its own; every read path must
  write it back to zero or the panel reports exactly one press and then looks
  dead. `Touch_Init()` clears it once at the end, too — otherwise a finger
  resting on the glass during bring-up leaves a press queued, and the boot
  splash skips itself.
- **The SD card is 1-bit, and GPIO21 is not the fourth data line.** Only D0 is
  brought out. GPIO21 carries a `D3` label but behaves as an enable that must
  be driven high before the card answers at all. Asking for a 4-bit bus here
  trains against floating pins and fails slowly on every boot.
- **The GNSS UART has nowhere else to go.** Between the panel (5, 39–42, 45),
  touch (1–4), the sensor I2C (10, 11), the card (14, 16, 17, 21), battery
  (8), power (6, 7), audio (38, 47, 48) and USB (19, 20), the only free pins
  are **15 and 18** — plus UART0's 43/44, which the serial item above says to
  keep. So GNSS is RX 18 / TX 15 and there is no second choice; anything else
  that wants a pin on this board has to take one away from something.
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
- **ANT+ was removed, and it was never proven either way.** Its last live run
  showed `ant_start=1`, `ant_chan=1`, the MAC ticking past 589k and `ev 0` — the
  soft-PHY and its TDMA grid were demonstrably running. What was missing was a
  transmitter: the strap on the bench is a **SOLE** treadmill unit, which is
  5.3kHz analog, a near-field magnetic pulse rather than 2.4GHz, and no firmware
  can bridge that. It is invisible to BLE too. So "ANT+ works" was never
  established in **either** direction, and the code was deleted on 2026-09-13
  rather than left as an untested radio mode with a UI switch in front of it.
  The lesson that outlives it: a feature nothing can test is a feature that will
  be wrong, and a bench that cannot exercise a radio is worth knowing about
  before the radio is written.

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
- **Never hand a static object to NimBLE's `setCallbacks`.** `NimBLEServer::
  setCallbacks(cb)` defaults its second argument, `deleteCallbacks`, to **true**,
  and `~NimBLEServer` then runs `delete` on whatever it was given. Every
  callback object in `BLE_TBT_Receiver.cpp` is a file-scope static, so that
  delete reaches `free()` with a pointer in no heap and the chip asserts inside
  `heap_caps_free` — a panic that names the heap and never mentions BLE. Pass
  `false`. It only fires on the path that destroys the server (Restart on the
  settings page, via `BLE_HR_Shutdown()` and `NimBLEDevice::deinit()`), so the
  board runs for days before anyone trips it. `NimBLECharacteristic::
  setCallbacks` takes no ownership and needs no flag, and
  `NimBLEClient::setClientCallbacks` has the same defaulted trap as the server.
  Two near-identical APIs where one owns its argument and one does not.
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

## 9. WiFi File Transfer

`src/system/FileServer.h` turns the board into a WPA2 access point serving the
SD card over HTTP, so rides come off and routes and maps go on without pulling
the card. Reached from the settings page; the modal that appears is
`src/ui/Overlay_FileTransfer.h`.

Three constraints shape it, and none are negotiable:

- **It takes the radio rather than sharing it.** Starting the server shuts the
  BLE stack down. WiFi and Bluetooth share one antenna here, and section 8 is
  already a list of what happens when two things ask the controller for
  overlapping work. Consequently **the only way out is a restart** —
  re-initialising NimBLE after a deinit is the same class of teardown that
  panicked the chip before.
- **It is the only reader of the card while it runs.** `SD_MMC` is not
  thread-safe, and the card is otherwise read by the LVGL task and written by
  the ride-log task. Hence the modal on `lv_layer_top()` rather than a page
  (nowhere to navigate to), and the refusal to start while recording.
- **`src/system/FilePath.c` is the security boundary and is host-tested.**
  Three directories, no nesting, no traversal, no dotfiles, and only `.gpx` at
  the card root — the root is the owner's own folder, not a share. It is the
  only input in this firmware that arrives from off-device and reaches the
  filesystem. Widen it there, with tests, or not at all.

`-D PEGASUS_WIFI_FILES=0` in `platformio.ini` removes the whole feature, and
that is the **only** thing that recovers what it costs. Measured on this board:

| | with it | without it |
|---|---|---|
| Static RAM | 140,224 B (42.8%) | 119,388 B (36.4%) |
| Flash | 1,657,877 B (25.3%) | 1,238,549 B (18.9%) |

410KB of flash and 20KB of static RAM, essentially all of it the WiFi stack.
A *runtime* switch would recover none of it: that expense is code linked into
the image and buffers reserved at link time, and the radio itself is already
only started when the button is pressed, so there is no idle cost left to
save. Everything still compiles and links with the flag off — the API answers
false and says why, so the UI carries no conditionals.

The access-point password is fixed at the owner's request, so it can be typed
from memory, and is shown on the panel. It has a digit on the end because WPA2
refuses a passphrase under eight characters outright -- `WiFi.softAP()` returns
false rather than falling back to an open network.

The per-session random password it replaced is worth remembering for the reason
it existed: the access point broadcasts the chip's MAC as its BSSID, so a
MAC-derived password would be published alongside the network it protects. A
fixed word is not derived from anything, so it is only as weak as it is short.
Fine for a few minutes beside its owner; not something to leave running.

## 10. Companion App (`phone/android/`)

A Kotlin app that scrapes Google Maps' navigation notification and writes
turn-by-turn frames to the head unit over BLE — Maps exposes no API, so the
notification is the only route without root. See its own README.

**`src/navigation/TbtParse.h` and `src/navigation/RouteParse.h` are the
authorities on the two wire formats**; the app's `TbtFrame.kt` and
`RouteFrame.kt` are encoders for them, and `TbtFrameTest` / `RouteFrameTest`
pin each pair together byte for byte. Change one side and those tests should be
what notices.

The app has two route sources at very different maturities, and the README
explains the split. Google Maps notifications work and are verified on a real
route, but Maps ships the arrow as a bitmap and a notification carries no
geometry at all, so that path can drive the live display and can never supply
the offline fallback. The Mapbox Navigation SDK plans the route, so it yields
structured maneuvers and the polyline together -- but it is **opt-in and has
never been compiled here**, because Mapbox serves it from a repository that
refuses anonymous access and needs an account with two separate tokens. Build
it with `-PwithMapbox=true` and without `--offline`. Everything that did not
need the SDK is outside it and unit tested.

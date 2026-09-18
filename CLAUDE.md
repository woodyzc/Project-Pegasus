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
  - `Page_Dashboard`'s ELEVATION cell carries both readings: `NOW` is grade
    from `IMU_Data_t.pitch` and reads "--" until an IMU publishes, `GAIN` is
    total ascent from `Ascent.h`. Two equal rows rather than one large figure
    over a small one, because which of the pair matters depends on the board,
    and a ranking baked into the layout would be wrong on one of them.
- **Power & Control**:
  - Onboard `BAT` Button (GPIO Interrupt) *(target board only)*: Soft-switch for manual Deep Sleep entry and wake-up.
  - Power Subsystem: Target ~200μA standby current in Deep Sleep (5–6 months standby on 1000mAh battery).
- **Audio Output** *(target board only)*: Onboard PCM5101 I2S decoder & speaker for key clicks, off-route alerts, and turn prompts.

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
    scans to learn the peer's address, and every later reconnect dials it
    directly. That split was originally forced by ANT+ coexistence and stays
    because it is better behaviour on its own — it does not put a scan on the
    controller beside a connection attempt, which is the load section 8 warns
    about. It is **not** one-shot: the supervisor rescans every couple of
    seconds while it has no peer, and discards the stored address after a few
    failed connects, because a watch's resolvable private address rotates.
  - **A peer can still make itself unfindable, and so far only a watch has.**
    A reset without a goodbye leaves the peer believing the link is up, and a
    peripheral that thinks it is connected stops advertising — so the board
    scans for something deliberately not there. `BLE_HR_Shutdown()` exists to
    prevent that, and writes its outcome to NVS; the settings page shows it as
    "Last disconnect on restart", which is what to read before touching any
    timing. Seen on a Galaxy Watch 8 (2026-09-15), where the only cure was
    switching broadcasting off on the watch. Expect a strap not to share it —
    its supervision timeout is seconds, so it re-advertises on its own — but
    that is reasoning, not a measurement.
  - The parsing half is `src/sensors/BleHrParse.c`, host-tested with no NimBLE
    dependency.
- **BLE Turn-by-Turn**: a NimBLE GATT server the phone writes into. The server
  runs in **both** navigation modes, because it also carries the phone's
  position (§5) and a head unit with no receiver of its own needs that either
  way — a GPX breadcrumb is drawn against a position, so without one it draws
  nothing at all. Only the *display* of turns is mode-dependent; a directive
  arriving in GPX mode is published and ignored by `Page_Dashboard`.
- **BLE phone alerts**: a sixth characteristic on the same GATT server carries
  a call, a text or a chat message as a kind plus a sender name --
  `src/system/AlertFrame.h`, drawn by `src/ui/Overlay_Alert.h`. Two rules there
  are deliberate and should not be "improved" away:
  - **No message body, ever.** The frame has no field for one. A rider cannot
    reply and should not be reading prose in traffic; the only question worth
    answering at speed is whether to stop, and a name answers it.
  - **The banner covers the metric cells and never the navigation region.**
    y184 down is speed, trip, heart rate and elevation, all of which can be
    read again a second later. The 184px above is the turn, which cannot. The
    geometry is hard-coded in `Overlay_Alert.cpp` against `Page_Dashboard`'s
    layout, so the two move together.
  - The NVS bring-up watchdog in `Settings_Init()` used to force the mode to
    GPX, which worked only because GPX happened to start no radio. That
    coincidence silently cost the whole position feature the moment it was
    built. It now raises `Settings_RadiosHeldOff()` instead — one boot with no
    GATT server and no advertisement, which is where every hang in §8 actually
    lived — and leaves the navigation mode alone. **"What navigation do I show"
    and "do I touch the radio" are separate questions; do not re-merge them.**

## 4. Software Architecture & FreeRTOS Core Rules
- **Core 0 (Background Data Core)**:
  - Task 1: MAX-M10S UBX parsing with low-speed anti-drift and Kalman filtering.
  - Task 2: NimBLE BLE client reception (heart rate).
  - Task 3: Battery monitoring. The idle/sleep half of this is **not** a Core 0
    task: it changes the backlight and puts a widget on screen, and LVGL here
    has no lock, so `PowerManager` runs on an LVGL timer instead
    (`src/system/PowerManager.h`). The decision logic is pure and host-tested in
    `src/system/IdlePolicy.h`. Deep sleep and its touch wake are **proven** —
    2026-09-17, slept overnight, woke on a touch, "Last reset: Deep sleep". It
    stays opt-in for a bench reason rather than a risk one: a sleeping board's
    USB-Serial-JTAG is powered down with the rest of the digital domain, so the
    port disappears and it cannot be flashed until something wakes it. IMU-based motion detection is still absent, because the
    board on the bench has no IMU.
- **Core 1 (UI & Life Cycle Core)**:
  - Task 1: LVGL rendering loop (`lv_timer_handler()`).
  - Task 2: X-TRACK `PageManager` life cycle management.
- **Data Bus Architecture**:
  - Strictly enforce X-TRACK `DataCenter` (Pub/Sub message bus).
  - All inter-core communication and sensor updates must publish to `DataCenter` topics (e.g., `Sensor/HeartRate`, `GPS_Info`), never via direct cross-thread function calls.

## 5. Navigation Strategy

> **Verified end to end on 2026-09-17**, and it unblocked most of the firmware
> in one go. With the phone feeding fixes: SPEED left `--` for `0.0`, the clock
> read `GNSS, -240 min` — EDT, *derived from the coordinates* through
> `TimeZone.c`, which had never run on a real position — and the ride log
> opened `/rides/2026-09-18_025422.gpx` by itself and wrote six points at
> exactly 30-second spacing, heart rate included as `<gpxtpx:hr>`. Three BLE
> characteristics ran concurrently on one link throughout.
>
> **The phone can supply the position fix.** The MAX-M10S has never been
> fitted, so everything downstream of a position — speed, the odometer, the
> ride log's whole lifecycle, the map, track-up, route snapping, onboard
> turn-by-turn, ascent, the clock — was written, host-tested and never once run
> against a real fix. `src/sensors/GpsFrame.h` is a fifth GATT characteristic
> the phone writes a fix into, published to `TOPIC_GPS_INFO` exactly as the
> receiver would.
>
> **The module wins, permanently, once it has ever had a valid fix.**
> `GPS_Info_t.from_module` exists for that decision and nothing else: both
> sources land on the same topic, so without it the head unit would take its
> own republished phone fix as proof a receiver exists. The asymmetry against
> the turn handover below is deliberate — a turn going stale is the phone
> falling quiet, which is ordinary and reversible, but a receiver that had a
> fix and lost it is in a tunnel, and there its own "no fix" is the truth.

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
- **Dropping the CPU to 80MHz while the screen is dark works, and is safe for
  every peripheral on this board.** Verified on hardware 2026-09-15: the
  downclock counter on the settings page went to `1x` after a blank, and the
  touch that woke it, the panel that redrew and the backlight that came back
  all behaved normally. So I2C, SPI and LEDC survive the switch — and since
  the GNSS UART hangs off the same APB, it should too (untested, no module).

  **80MHz is a floor, not a starting point.** On the S3, APB is a fixed 80MHz
  for any PLL-sourced CPU frequency (`soc.h`: `APB_CLK_FREQ`, and
  `UART_CLK_FREQ` derives from it), so 240→80 moves nothing. Below 80 the CPU
  sources from the crystal and APB follows it down — and arduino-esp32's S3
  branch of `calculateApb()` returns a hardcoded constant, so its apb-change
  callbacks never fire and no peripheral is told. Pick 40MHz and the GNSS
  silently stops decoding, a long way from the line that caused it.

  **BLE survives it too** — verified the same day with a strap on, across a
  blank of over two minutes: the dashboard showed a live BPM on wake, and it
  blanks the reading after `HR_STALE_MS` (5s), so a measurement had arrived
  within five seconds of the screen coming back. This was the one part
  expected to break, because `setCpuFrequencyMhz()` bypasses `esp_pm` entirely
  and takes no lock the BT controller can hold against it.

  Read that evidence for what it is: it proves data was flowing, not strictly
  that the link never dropped and re-established during the dark. A reconnect
  uses exponential backoff, so a wake landing mid-backoff would more likely
  have shown "--" — but if a disconnect counter is ever wanted, that is what
  would close the gap.
- **Recording is a deliberate act, and that trade runs both ways.** Ride
  logging used to arm itself on the first valid fix and never stop, so the
  device wrote the drive to the start, the walk from the car and the next
  morning's train as rides. It now starts *only* from "Start new ride", and
  "Finish ride" or an hour without movement disarms it again.

  The danger moved rather than disappeared: forgetting to start loses a whole
  ride, where forgetting to finish only left a file to delete.

  **`Trip`, `RideStats` and `Ascent` accumulate only while the ride is armed**,
  and that was not always so. They subscribed to GPS directly and counted from
  the moment a fix existed, which the phone-supplied position made visible
  immediately: the TRIP cell sat amber saying nothing was being recorded with a
  number climbing inside it, and "new ride" popped up a summary of a ride that
  had never been started, because the distance quietly gathered beforehand made
  `RideSummary_IsEmpty()` false. Armed rather than recording — a rider waiting
  on their first fix is on their ride. `RideStats` treats disarmed exactly like
  a dropped fix rather than merely skipping, so the first sample after starting
  opens a fresh interval instead of charging the average for however long the
  device sat on a desk.

  SPEED is deliberately **not** gated: it is a live sensor reading, not a ride
  statistic, and a rider pushing the bike wants to see it move. The TRIP cell is the defence: it is *filled* when, and only
  when, nothing is being written — amber stopped, red while moving — and looks
  like any other cell while recording. Filling the good state too was tried
  and dropped: a rider is recording for hours, and a block of colour held for
  hours stops being seen, besides spending the panel's one loud gesture on the
  situation that is fine. No word is added either way — a 10px "OFF" beside a
  28pt figure is the first thing lost to a glance at speed, in sunlight, on a
  rough road, whereas a filled block a quarter of the screen wide is read from
  outside the point of focus. While filled, its text is `COLOR_BG`: amber and
  red are both light, so only dark is legible on both, which makes this the
  one place on the panel that inverts — and every colour has to be put back
  explicitly on the way out, or the cell stays inverted for the rest of the
  boot. Do not quietly demote it.

  **A restart does not end a ride.** The armed flag is persisted, so recording
  resumes on the next fix — in a new file, because appending would mean reading
  the GPX back and stripping its footer, on a card, at boot. Before that, a
  reboot mid-ride silently stopped recording and the only way to resume,
  pressing "new ride", reset the odometer with it — which `Trip` persists to
  NVS precisely so a restart does not lose it. `RideLog_Shutdown()` closes the
  file first, through the writer queue rather than by touching it, because the
  file belongs to that task. Note the auto-end timeout therefore keys on
  *armed* rather than *recording*: a device armed somewhere with no signal
  opens no file, and a check on recording would leave it armed for ever now
  that a reboot no longer clears the flag.

  An earlier attempt closed the file on the timeout but left recording armed.
  That does not work — movement simply opened a new file, so a forgotten
  device produced one clean ride followed by a string of junk ones. Closing
  without disarming splits the problem up; it does not solve it.
- **Deep sleep is gated on ride state, not on USB.** It used to be gated on
  `Battery_t.on_usb`, which is a threshold at 4500mV on a reading of the *pack*
  voltage — and a 1S charger terminates at 4.2V, so on this hardware that flag
  can never be true. A gate that never closes is worse than no gate: it reads
  as protection while providing none. The rule now asks something the firmware
  knows exactly: an armed ride blocks sleep outright (recording *or* waiting on
  a first fix), a finished ride sleeps after five minutes, and a device that
  has recorded nothing waits thirty — because it has been told nothing and may
  be waiting on a rider who has not started yet.
- **Deep sleep and its wake source work.** Verified 2026-09-17: the board
  slept overnight on USB, a touch woke it, and the panel read `Last reset:
  Deep sleep (boot 8)` with the card remounted and the PSRAM buffers back.
  That one line proves the whole chain, including the `gpio_hold_en` on
  `TOUCH_RST_PIN` in `EnterSleep()` — without it the ESP32 releases every
  non-RTC pin on the way down, resets the touch controller, and nothing is left
  to pull the interrupt line. That hold had only ever been reasoned about.

  It also proves the USB gate really was dead weight: the board was plugged in
  the whole time and slept anyway, which is exactly what removing that gate was
  meant to allow.
- **`PrepareForSleep()` unmounts the card without closing the ride file, and
  gets away with it only because of the sleep gate.** `SD_MMC.end()` runs with
  the writer task still alive and nothing closing anything — safe today purely
  because deep sleep requires `!RideLog_IsArmed()`, and disarmed means
  `CloseRide()` has already run. Loosen that gate and this becomes an unmount
  under an open file, with no compile error and no obvious symptom beyond a
  truncated GPX. Either keep the gate or make the sleep path close the log the
  way the restart path does.
- **An inhibitor that blocks sleep does not block blanking.** `IdlePolicy_Stage()`
  returns BLANK *before* it consults ride state or `sleep_enabled` — those
  only decide whether it goes further. So "never sleeps on USB" has
  never meant "never blanks on USB", and anything tied to blanking (the
  downclock, for one) happens on a bench-powered board exactly as it does on a
  battery. Only the file server inhibits everything. This is worth knowing
  before wiring a diagnostic to the wrong branch, which is how the CPU clock
  readout came to be invisible on the one screen built to show it.
- **Register every GATT service before anything scans or connects.**
  NimBLE's `ble_gatts_mutable()` refuses to add a service while an
  advertisement, a scan, a connection attempt or an established connection
  exists, and the refusal is *not* returned to the caller — it lands on
  `SYSINIT_PANIC_ASSERT` inside `ble_svc_gap_init()` and panics the chip. So
  `BLE_TBT_Start()` must precede `BLE_HR_Start()` in `main.cpp`. It only
  misbehaves when a heart-rate peer is actually in range, which makes it look
  like flaky hardware: with no watch nearby nothing connects and the identical
  code registers fine.
- **The UI is live long before `setup()` finishes.** `LvglTask_Start()` comes
  early on purpose — the radios after it block for fifteen seconds and the
  dashboard should be drawn and refreshing meanwhile — but the consequence is
  that a rider can reach any page and press anything during those seconds.
  Ride logging, the odometer and the ride stats were initialised at the *end*
  of `setup()`, so "new ride" in that window cleared the odometer and then
  found no queue to start a log in, and the ride buttons drew from an armed
  flag NVS had not been read into yet — appearing the wrong way round and
  swapping a second later. Anything a page can touch must be initialised
  before `LvglTask_Start()`, not merely before it is needed.
- **Every live reading on the dashboard must age out, position included.**
  Heart rate goes to `--` after 5s and a turn is dropped after 30s, but the
  position had no expiry at all — so closing the phone app mid-ride left the
  speed frozen at whatever it last was and the map's arrow still claiming to
  know where the rider is. `0.0` is the worst case of it, being
  indistinguishable from having stopped.
  - Stamp the freshness on a **valid fix**, never on a publish. `GPS_Reader`
    publishes without one on purpose — a climbing `num_sv` is how "module
    present, still acquiring" is told from "no module" — so a receiver in a
    tunnel keeps publishing at 1Hz while knowing nothing, and a check on
    publishes alone reads that as a live position for as long as the tunnel.
  - Blank the label rather than skipping the write. `if (have) set(...)` with
    no `else` leaves the last value on screen for ever, which is exactly how
    this one survived so long.
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
- **Stop the heart-rate supervisor before deinitialising NimBLE.** `BleHrTask`
  runs forever and calls into the stack on every pass, and `BLE_HR_Shutdown()`
  used to call `NimBLEDevice::deinit(true)` straight into it — deleting every
  client and server while the task still held pointers to them. The window was
  wide open rather than narrow, because the disconnect immediately above the
  deinit is exactly what wakes the task to try reconnecting. `s_client` and
  `s_server` were both left dangling too, so every null check afterwards passed
  on a corpse. The fix is cooperative parking (`vTaskDelete` is not safe — the
  task may hold NimBLE's mutex), bounded so the Restart button cannot hang, and
  the settings page reports whether the park actually happened. Reachable from
  the Restart button, from deep-sleep entry, and from any `esp_restart()`.
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
| Static RAM | 140,532 B (42.9%) | 119,688 B (36.5%) |
| Flash | 1,896,837 B (28.9%) | 1,477,585 B (22.5%) |

419KB of flash and 20.4KB of static RAM, essentially all of it the WiFi stack.
(Measured 2026-09-16. An earlier table here read 1,657,877 B of flash; the
233KB since is the boot splash, the number fonts and the turn icons, none of
which are WiFi's doing. Re-measure rather than trusting these — the ratio has
held, the absolutes have not.)

**The radio is off unless the rider starts it.** `FileServer_Start()` has
exactly one caller, the modal behind the settings button, and nothing in
`setup()` touches WiFi. What the table costs is paid at link time regardless:
code in the image, and buffers reserved in BSS before `main` runs — the 64KB
`work_mem_int` among them, which is most of that 20KB static-RAM difference.

Where the flash actually goes, for whenever it does get tight: **about 1MB of
font glyph bitmaps** and **150KB of `splash_map`**, which is an uncompressed
240x320 RGB565 frame. Together they are roughly 45% of the image and are the
first places to look — before, say, giving up the file server.

**Three quarters of that font figure is one face.** `src/ui/CjkFont.c` is
751KB: Noto Sans CJK SC at 20px, GB2312 plus ASCII, 2 bits per pixel, and it
exists so a Chinese sender name can be drawn at all. Every other font here is
Montserrat and covers 0x20-0x7F, so without it a WeChat alert from 张三 arrives
intact, parses cleanly and renders as an empty banner. It is generated by
`tools/gencjkfont.py`, which documents why the charset stops at GB2312 rather
than covering the whole 0x4E00-0x9FFF block: LVGL packs `bitmap_index` into 20
bits, so **a font's bitmap data cannot exceed 1MB**, and the full block is past
that — with no build error, just glyphs drawn from a wrapped index.
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
structured maneuvers and the polyline together -- but it is **opt-in**, because
Mapbox serves it from a repository that refuses anonymous access and needs an
account with two separate tokens. Build it with `-PwithMapbox=true` and without
`--offline`. Everything that did not need the SDK is outside it and unit
tested.

⚠️ It **has** been compiled, whatever this paragraph used to say.
`phone/android/app/build/intermediates/merged_native_libs/debug/` holds
`libmapbox-common.so` and `libmapbox-maps.so` dated 2026-09-12, and
`build.gradle.kts` declares the SDK only inside `if (withMapbox)` — a default
build cannot produce them. The app's own README had it right the whole time
("Compiles, but nothing calls it"), and the stale claim was the one quoted to
the user as fact.

Nor is a caller missing: `TbtService.ensureRouteSource()` calls
`RouteSources.create()` and wires both `onRoutePlanned` (uploads the polyline)
and `onInstruction` (sends each live turn). `RouteSources` is build-variant
selected — `src/nomapbox` returns null, `src/mapbox` returns the real thing —
so a default build has no source, not no caller. What is outstanding is an
account with two tokens, and nothing else.

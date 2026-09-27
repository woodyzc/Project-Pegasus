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
- **Touch**: bus is SDA 1 / SCL 3, INT 4, RST 2 — its **own** bus, separate from the sensor bus. **Which controller is on it depends on the board revision, and they are different protocols, not variants.** `src/hal/Touch.cpp` probes for both.
  - **This bench board is a V2: CST3530 at `0x58`** — verified working 2026-09-26. V1 was discontinued 2026-06-05, so V2 is now the default expectation.
  - ⚠️ **Get the V2 demo, not the V1 one.** The archive linked from the wiki's main page is V1 and contains no CST3530 code at all; the V2 archive is a separate download
    (`ESP32-S3-Touch-LCD-2.8-V2-Demo.zip`, from the Resources page) and carries
    `Touch_CST3530.cpp`. Porting the V1 driver to a V2 chip cost most of a day.
  - **CST3530 protocol** (V2): **32-bit register addresses, MSB first** — this is the one that matters; send a 16-bit address and the part ACKs and then clocks out nothing, so every read is an undriven bus that looks like a decode problem. Data is **9 bytes from `0xD0070000`**. No contact when `(buf[3] & 0x0F) == 0` or `(buf[8] & 0xF0) == 0`; count is `buf[3] & 0x0F`. `x = ((buf[7] & 0x0F) << 8) | buf[4]`, `y = ((buf[7] & 0xF0) << 4) | buf[5]`, strength `buf[6]`. End every read — valid or not — by writing register `0xD00002AB` **with no payload**; the 0xAB is part of the address. Reads take a **repeated START**, writes a STOP. Reset is **low 100ms, high 500ms**. There is **no ID register**: the vendor probes the address and stops.
  - **CST328 protocol** (V1, untested here): 16-bit addresses, count at `0xD005` then 27 bytes at `0xD000`, `x = (buf[2] << 4) | (buf[4] >> 4)`, `y = (buf[3] << 4) | (buf[4] & 0x0F)`, clear by writing 0 to `0xD005`, reads take a STOP, reset is high 50 / low 5 / high 50, and it identifies itself with **`0xCACA`** at bytes 10..11 of the 24-byte block at `0xD1F4`.
  - ⚠️ **`0xCACA` is a CST328 constant. Do not gate a CST3530 on it** — it reads `0x0000`, and treating that as a fault turned a working panel into a diagnosis of dead hardware.
  - **Read only from the interrupt.** Both parts are designed for it and the vendor never polls. Polling the data registers asynchronously returns frames caught mid-write, which decode as contacts at coordinates off the panel — i.e. the board presses its own buttons and walks through its own settings pages. Every phantom-touch episode during bring-up was this.
  - **Read the panel on EVERY LVGL read, with no interrupt gate.** The
    vendor's `Lvgl_Touchpad_Read` calls `Touch_Read_Data()` unconditionally;
    its ISR and `Touch_Loop()` feed a separate printf demo, not the input
    device. Gating LVGL's read on the interrupt makes a press begin only on an
    edge and end on the first empty read, so a 300ms swipe arrives as several
    2-to-5-sample fragments and no gesture can accumulate — it reads as
    "swipes need five tries", not as broken touch.
  - **The rule this chase earned:** when a vendor driver exists, match it
    completely and confirm it works *before* improving any part of it. Three
    separate symptoms here — the noise, the phantom contacts, and the stiff
    swipes — were each a deviation of mine from that driver, not a fault in
    the part, and each cost rounds to diagnose as though it were.
  - A **V3** would presumably bring a third part. The probe-and-dispatch shape in `Touch.cpp` is there so that costs one branch, not a rewrite.
- **GNSS Module**: via UART1 on **RX 18 / TX 15** — the only two genuinely spare GPIOs on the board, both on the 12-pin external connector.
  - ⚠️ **The module on the bench is an ATGM336H (中科微电子 GPS+BD), not the
    MAX-M10S this spec asks for** — the M10 still has not arrived. Different
    chipset, not a u-blox part, and it does not speak UBX: no documented
    binary config protocol at all, just plain NMEA-0183 (GGA/RMC, plus
    GSA/GSV/VTG the firmware ignores) at **9600 baud**, with no way to switch
    it to anything else. So the UBX "force binary, disable NMEA" constraint
    below is dormant, not satisfied. `src/sensors/NmeaParse.c` is the
    from-scratch GGA/RMC decoder in use (host-tested,
    `test/host/test_nmea_parse.c`); `UbxParse.c` stays for when the M10
    arrives. Both fill the same `GPS_Info_t`, so nothing downstream of
    `TOPIC_GPS_INFO` cares which is running.
  - **It is verified working — but on the stand-in, not here.** It found
    satellites on the Hosyond board on 2026-09-23. On *this* board it has
    never been powered: different pins, and this branch's first boot.
  - *Not* GPIO43/44, and that decision differs from the stand-in's on
    purpose. The Hosyond board had **no** spare GPIOs, so it had to spend
    UART0 and with it §8's USB-TTL escape hatch. This board has two real
    spares, so 43/44 stays free and **the escape hatch still exists here**.
  - *Constraint (for the M10 when it lands)*: force UBX binary only; disable high-overhead NMEA text parsing.
  - *Power*: Retain micro-power RTC backup (~15μA) for <1s hot starts.
- **Sensor I2C bus** — SDA 11 / SCL 10, separate from the touch bus, and untouched by firmware so far:
  - **IMU** *(present, not yet driven)*: QMI8658 6-axis at `0x6B`. Motion detection, inclination/slope, anti-theft alarm, fall detection, Any-Motion wake.
    - **This is the first board in the project with the hardware to produce a
      grade at all.** INCLINE and ASCENT live on the dashboard's **second**
      page; they shared a two-row ELEVATION cell on the first page until
      cadence took that cell on 2026-09-20, which cost nothing at the time
      precisely because no board had an IMU. That is no longer true, so
      whether INCLINE deserves the first page again is now a real question
      rather than a moot one.
  - **RTC** *(present, not yet driven)*: PCF85063 at `0x51`, battery-backed. Note `TimeZone.c` derives the zone from GNSS coordinates and the clock is fed from the fix, so this would buy time across a flat battery, not first-fix speed.
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
- **BLE cadence (0x1816)**: a second client, for a crank sensor on the bike.
  `src/sensors/BLE_CSC_Client.h` runs the link; `src/sensors/BleCscParse.h` is
  the pure, host-tested half and carries the part that is easy to get wrong.
  The wire has no rate on it -- two free-running 16-bit counters, revolutions
  and a 1/1024s event time, both of which wrap on any real ride -- so the rpm
  is computed from differences, and the arithmetic is the subject of
  `test/host/test_csc_parse.c` rather than something checked by eye.
  - **Zero and "--" say different things and must keep doing so.** A connected
    sensor with a still crank publishes a real 0 after `CADENCE_IDLE_MS`; a
    sensor that has gone away publishes nothing and the dashboard blanks the
    cell five seconds later. A flat battery must not look like a rider
    coasting. That is also why the supervisor republishes the current rpm once
    a second even when nothing changed: a rider holding a steady cadence
    produces no changes at all, and silence on that topic means "gone".
  - **A stop longer than 64 seconds destroys the baseline, and the wrong
    answer it produces looks right.** The crank event time advances only when
    the crank turns and wraps every 65536 ticks, so a rider standing at a long
    light resumes with a gap whose wrap count is unknowable -- and the 16-bit
    subtraction answers anyway. 65s of standing still leaves a residue of 1024
    ticks, so the first stroke back reads as a clean 60rpm: past every sanity
    check, indistinguishable from a measurement, entirely invented.
    `CADENCE_MAX_RPM` cannot catch it, because a stop can land on any residue
    it likes. `CADENCE_BASELINE_MAX_GAP_MS` throws the baseline away instead,
    at a cost of exactly one sample. The same guard covers a live link that
    simply goes quiet for a minute, which is why there is no separate check
    for that. `last_sample_ms` tracks the last sample that *advanced* the
    event time, not the last notification -- a sensor notifies on a timer
    whether or not anyone is pedalling.
  - **The rejected sample still commits its counters, and that is correct.**
    An external review called this a state-machine bug and proposed
    validate-then-commit. It is backwards: the counters are absolute and
    free-running, so what was anomalous is the *interval*, not the packet.
    Committing means the next delta is measured from the most recent real
    crank event and recovers in one notification; not committing measures from
    a baseline that is now even older, and the tracker can stick rejecting for
    ever. `test_csc_parse.c` pins the recovery ("and the next good interval
    still reads").
  - ⚠️ **A dual-mode sensor decides speed-or-cadence at its end, and a
    firmware that only reads cadence cannot tell you so.** Verified on the
    bench 2026-09-20: the sensor connected, subscribed, notified 1,500 times,
    and the panel read a steady 0 -- because every packet carried wheel data
    and no crank data. Flags `0x01`, length 7, crank fields simply absent.
    Switching the sensor to cadence mode fixed it and nothing in this firmware
    changed.

    The settings page now says which of the two it is, in words, and shows the
    raw packet fields **only when every packet has arrived without crank
    data**. Read `SPEED SENSOR, no cadence` as final -- that comes from the
    sensor's own CSC Feature bit 1 -- and `sending SPEED not cadence` as a
    mounting or pairing question at the sensor. `@left crank` versus
    `@rear hub` is the sensor telling you which it believes it is.

    The general lesson is the one §8 keeps re-teaching: a silent wrong answer
    needs a diagnostic on the panel, because this board has no usable serial
    console to put one on.
  - **Three connections is the ceiling and all three are now spoken for** --
    phone, heart rate, cadence -- against NimBLE's
    `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` of 3. A fourth sensor needs that raised
    and the RAM that comes with it.
  - **`src/sensors/BleRadioGate.h` exists because a second client made "two
    connects at once" reachable.** Bracketing one connect with
    `BLE_TBT_PauseAdvertising()` was enough while there was one client; two
    supervisors on independent backoffs will eventually initiate two links
    together, which is the same controller load, with no advertisement
    involved. Both clients take the gate across discovery and connect.
  - **`BLE_HR_Shutdown()` parks the cadence supervisor too.** It owns the
    teardown, and `NimBLEDevice::deinit(true)` destroys every client on the
    stack -- including the one a running cadence task is holding a pointer to.

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
    built. It now raises `Settings_RadiosHeldOff()` instead — one boot with **no
    radio at all**, which is where every hang in §8 actually lived — and leaves
    the navigation mode alone. It held off only the GATT server until
    2026-09-20, which made it useless against the likelier hang: `BLE_HR_Start()`
    runs a synchronous 15s scan on the same stack, so a held-off boot hung too,
    and since the counter is cleared when the hold fires the board settled into
    a permanent three-boot cycle — hang, hang, hang-with-the-server-off — while
    the settings page promised a restart would try again. **"What navigation do I show"
    and "do I touch the radio" are separate questions; do not re-merge them.**

## 4. Software Architecture & FreeRTOS Core Rules
- **Core 0 (Background Data Core)**:
  - Task 1: MAX-M10S UBX parsing with low-speed anti-drift and Kalman filtering.
  - Task 2: NimBLE BLE client reception (heart rate).
  - Task 3: Battery monitoring. The idle/sleep half of this is **not** a Core 0
    task: it changes the backlight and puts a widget on screen, and LVGL here
    has no lock, so `PowerManager` runs on an LVGL timer instead
    (`src/system/PowerManager.h`). The decision logic is pure and host-tested in
    `src/system/IdlePolicy.h`. Deep sleep and its touch wake were **proven on
    the stand-in** — 2026-09-17, slept overnight, woke on a touch, "Last
    reset: Deep sleep" — which settles the ESP32 half of the chain but not the
    wake source here, because this board's touch controller is a CST328 rather
    than the FT6336G that was tested (see `PowerManager.h`). It stays opt-in
    for a bench reason as well as that one: a sleeping board's
    USB-Serial-JTAG is powered down with the rest of the digital domain, so the
    port disappears and it cannot be flashed until something wakes it.
    IMU-based motion detection is still absent — but on this board that is an
    omission rather than a hardware limit: the QMI8658 is on the sensor bus
    (§2) waiting to be driven, and it would also make a wake source that does
    not depend on the touch controller at all.
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
  - **And snapping alone does not finish that job.** On a there-and-back the
    two legs are the same points in the same order, so both segments snap with
    the same cross-track and the global minimum settles it on segment index —
    always the outbound one. The rider is shown the outbound leg's next turn
    for the whole way home, and it never self-corrects because every fix
    re-decides it identically. `RouteFollow_SnapFrom` breaks the tie with where
    the rider already was, and that hint is **bounded**
    (`ROUTE_SNAP_HINT_BUDGET_M`) so a fix that genuinely belongs elsewhere
    still re-acquires immediately — the bound is what keeps the recovery a
    global search gives for free. Note that a single-fix test cannot catch the
    mistake this went through: making forward movement free is silently
    useless, because the return leg's candidate is *always* ahead of the hint.
    Only a test that walks a whole ride through the turnaround, feeding each
    answer back as the next hint, shows it.
  - **The onboard path needs the same 10s keepalive the phone path has.**
    `Page_Dashboard` drops a turn it has not heard about for 30s, so publishing
    only on change blanks the panel for a rider stopped at a light -- which is
    exactly when they are looking at it.
  - **Maneuver order is checked once, when the route lands.**
    `RouteFollow_NextManeuver` scans forwards and takes the first entry at or
    beyond the rider, so an out-of-order array does not fail loudly -- it hands
    back a turn already ridden past, every second, for the rest of the route.
    `RouteFollow_ManeuversOrdered()` turns the assumption into a fact in
    `FinishTransfer()`, and a route that fails it is refused rather than
    navigated badly in silence. Equal distances are allowed: two instructions
    at one coordinate is a real thing a router emits, and only strict
    inversion breaks the scan.
  - **Two publishers share `TOPIC_NAV_TBT`, so the onboard one re-checks
    before it writes.** `NavRoute_Tick` decides under the route lock and
    publishes with it released -- deliberately, so DataCenter's subscribers do
    not run with two buses' locks held -- and in that gap NimBLE's host task
    can publish a live turn that this one then overwrites. `s_live_seq` is
    compared across the gap and the stale publish is dropped. A millisecond
    stamp cannot settle it; two events in one millisecond read as one. The
    keepalive bookkeeping is deliberately *not* rolled back: the phone owns the
    panel for the next `TBT_LIVE_GRACE_MS` anyway, and if it falls silent again
    the keepalive is long overdue, so the onboard path resumes on the next tick
    rather than waiting.

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
  firmware fault. The workaround that has actually worked every time is
  **printing diagnostics to the LCD panel**, and it remains the first choice.

  **The USB-TTL escape hatch exists on this board**, unlike on the stand-in:
  wire **UART0 (GPIO43/44)**, on the 12-pin connector, to a USB-TTL adapter.
  It survived here only because this board has two genuinely spare GPIOs for
  the GNSS (15 and 18), where the Hosyond had none and had to spend 43/44 for
  it — so if you are reading that branch, this paragraph says the opposite
  there, and deliberately. Still theoretical: no diagnostic this project has
  needed has ever gone anywhere but the panel.
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
  statistic, and a rider pushing the bike wants to see it move. The TRIP cell
  is the defence, and it is *always* filled — the colour is the answer:

  | | |
  |---|---|
  | **red** | recording. The record light, in the colour every camera uses. |
  | **amber** | moving, and not one metre of it is being kept. |
  | **green** | nothing is being written and nothing needs to be. |

  Red and amber were the other way round until 2026-09-20, when the owner
  asked to swap them. The old scheme assigned colour by severity — red for the
  one state that demands action — and this one assigns it by convention, on
  the argument that a red dot means REC to anyone who has held a camera and
  that a panel read in a fifth of a second is better served by a learned cue
  than a reasoned one. **What it costs is that the alert for "riding and
  recording nothing" is now the quieter of the two colours**, and that is the
  only state here that loses a whole ride. If it is ever missed on the road,
  that is the trade to revisit.

  Leaving the good state unfilled was tried before either scheme and dropped:
  it could not answer "is it running?" without the rider first deciding
  whether a plain cell meant recording or meant they had misread it. No word
  is added in any state — a 10px "OFF" beside a 28pt figure is the first thing
  lost to a glance at speed, in sunlight, on a rough road, whereas a filled
  block a quarter of the screen wide is read from outside the point of focus.
  Its text is always `COLOR_BG`: all three fills are light, so only dark is
  legible on any of them, which makes this the
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


---

**Carried over to the Waveshare board, and not yet re-verified there.**
Everything above this line was learned on the Hosyond stand-in, including the
GNSS bring-up of 2026-09-21..23. The LVGL, TFT_eSPI and NimBLE items are
firmware-level and should hold unchanged; the serial item is a property of the
ESP32-S3's USB-Serial-JTAG peripheral rather than of any one carrier board, so
expect it to hold too.

Two above the line are known **not** to transfer as written, and both are
called out where they sit: the deep-sleep touch wake was proven against an
FT6336G and this board has a CST328, and the USB-TTL escape hatch is spent on
the stand-in but alive here. Below are the things this board adds. The board
reached the bench on 2026-09-26 and this branch's first boot is still ahead of
it — treat every item below as reasoned, not measured.

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

## 8a. Making the UI feel right (all four found on 2026-09-26)

`LV_USE_PERF_MONITOR` in `include/lv_conf.h` puts FPS and CPU% on screen.
Reach for it first: "33fps at 5%" is what proved the renderer innocent, and
"90% whenever the screen moves" is what found the byte swap. Guessing at
performance cost four rounds before anyone measured.

- **Draw buffers belong in INTERNAL RAM, not PSRAM.** Two full-screen buffers
  in PSRAM is cheap on memory and ruinous on bandwidth: every pixel is written
  to PSRAM by the renderer and read back by the flush, ~300KB of slow-bus
  traffic per frame. One 40-line partial buffer (19KB) in
  `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` is the standard arrangement and took
  the dashboard to 33fps at 5% CPU. Single, not double: `tft.pushColors` is
  blocking, so there is no DMA completion to overlap against.
- **`LV_COLOR_16_SWAP` and `Display_Flush`'s `pushColors(..., swap)` are one
  decision in two files.** With the swap off in LVGL and on in the flush,
  TFT_eSPI byte-swaps all 76,800 pixels in software per full redraw — the CPU
  sat at 90% during any motion. Swap in LVGL (`1`) and pass `false`. Either
  alone gives visibly wrong colours. This is the 16-bit word's endianness and
  is **not** `TFT_RGB_ORDER`, which is the R/B channel order; the panel needs
  both set.
- ⚠️ **A scrollable object suppresses gestures completely.** `indev_gesture()`
  opens with `if (proc->types.pointer.scroll_obj) return;`, so once a drag has
  latched onto anything scrollable, no gesture is ever emitted — the velocity
  and distance thresholds below it are never even read. `lv_obj_create()` sets
  `LV_OBJ_FLAG_SCROLLABLE` by default, exactly as it sets `CLICKABLE`, and
  `MapView`'s container missing it meant the top 184px of the dashboard
  silently ate swipes.
- **Suppress the unwanted click, not the wanted gesture.** The dashboard used
  to discard swipes over the navigation tile so a flick could not open the
  ROUTE page. That threw away 57% of the panel. LVGL still delivers `CLICKED`
  on release after a gesture, so the fix is for the click handler to check
  `lv_indev_get_gesture_dir() != LV_DIR_NONE` and ignore it.

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
  (nowhere to navigate to), and the refusal to start while a ride is **armed**
  — not merely while it is recording. A ride is armed from "Start new ride" and
  opens its file on the first valid fix, which can be a kitchen and half an
  hour apart; gating on recording let the server start in the gap and the
  writer open the card underneath it.
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

⚠️ **Holding a permission is not the same as being allowed to claim its
foreground-service type.** `location`, `camera` and `microphone` are
while-in-use types, and API 34 refuses to start one from the background
however many permissions are granted — `validateForegroundServiceType` throws
`SecurityException`, which kills the process. `MapsNotificationListener.
onListenerConnected()` starts the service from exactly that state, because the
system binds the listener after a reboot before the user has opened anything.
So the automatic start crash-looped the app it existed to rescue, and Android
answered with a thirty-minute restart backoff: a phone that rebooted mid-ride
had no link, no position and no alerts until someone opened the app by hand.

There is no API for "may I claim this type right now", so `TbtService` attempts
the full type set and falls back to `connectedDevice` alone, which is not a
while-in-use type and is always permitted. **The fallback must not throw** —
`startForegroundService` has already promised a `startForeground` within five
seconds. It is also not silent: without the type Android delivers no location
updates to a backgrounded app, with no error and no callback, which reads
exactly like a receiver that has never got a fix. The service says "Bluetooth
only; open the app to send position" instead, and opening the app restarts it
from the foreground where the type is granted.

It also forwards calls, texts and WeChat messages as alerts, from the same
notification stream — notification access is one grant, so a second listener
service would need its own. `AlertClassifier` and `AlertThrottle` hold every
rule and are pure and unit-tested; `MapsNotificationListener` only reads fields
and passes them on.

**The firmware headers are the authorities on the wire formats**, and the app's
`TbtFrame.kt`, `RouteFrame.kt` and `AlertFrame.kt` are encoders for
`src/navigation/TbtParse.h`, `src/navigation/RouteParse.h` and
`src/system/AlertFrame.h`. `TbtFrameTest`, `RouteFrameTest` and
`AlertFrameTest` pin each pair together byte for byte — the alert frame is
pinned hardest, because the same twelve-byte array is written out in both
`AlertFrameTest.kt` and `test/host/test_alert_frame.c`. Change one side and one
of those suites should be what notices.

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
so a default build has no source, not no caller.

**The SDK build works, as of 2026-09-19.** `-PwithMapbox=true assembleDebug`
resolves the SDK and produces a 51.6MB arm64-only APK carrying
`libnavigator-android.so`, `libmapbox-common.so` and `libmapbox-maps.so`,
against 3.3MB for a default build; the unit tests pass on that variant. The
secret `sk.` download token is in the macOS keychain under the service
`mapbox-downloads-token`.

⚠️ **Feed that token in through the environment, not `~/.gradle`.** The
comment in `settings.gradle.kts` says to put `MAPBOX_DOWNLOADS_TOKEN` in
`~/.gradle/gradle.properties`, and that is right for a normal checkout and
wrong here: builds in this project override `GRADLE_USER_HOME` to a scratchpad
toolchain, and Gradle then never reads `~/.gradle` at all. Use
`ORG_GRADLE_PROJECT_MAPBOX_DOWNLOADS_TOKEN`, which maps to the same project
property regardless of where `GRADLE_USER_HOME` points.

What is outstanding is the **public `pk.` token**, which is not a build input
at all — it is typed into the app on the phone and kept in its private
preferences (`MapboxToken.kt`). Until one is entered the SDK is linked in and
plans nothing.

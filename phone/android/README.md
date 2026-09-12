# Pegasus TBT — Android companion

Forwards turn-by-turn prompts to the Pegasus head unit over BLE, and uploads
the whole planned route so the head unit can keep navigating without the phone.

**Status: builds, 67 unit tests pass.** The Google Maps notification half is
verified on a real route — a live route confirmed the maneuver type, the
imperial to metric conversion (200 ft to 61 m) and the street name, the last of
which was wrong on first contact and is now a regression test.

**Not verified: anything past the phone.** No head unit has been powered for
the app to find, so the BLE link, the frame reaching the firmware and the head
unit rendering it are all untested.

**Not compiled at all: the Mapbox adapter.** See *Two route sources* below.

## Two route sources

There are two ways to get maneuvers into this app, and they are at very
different levels of maturity.

**Google Maps notifications** — works, is verified on a real route, and needs
no account or token. It is also permanently limited: Maps ships the maneuver
arrow as a *bitmap*, so the turn type can only be recovered from the
notification's wording. That makes it English only, leaves it guessing on
unusual phrasing, and gives no route geometry whatsoever — so it can drive the
live turn display but can never supply the offline fallback.

**Mapbox Navigation SDK** — plans the route, so it yields structured maneuvers
*and* the polyline the head unit needs to navigate on its own. This is the
route worth investing in, and it is what `MapboxRouteSource.kt` implements.

It is **not built by default and has never been compiled**, because the SDK is
served from Mapbox's own Maven repository, which refuses anonymous access.
Building it needs a Mapbox account and two different tokens. Everything that
could be written without the SDK was, and is unit tested — the maneuver mapping
(`MapboxManeuver`), the route model (`PlannedRoute`), the wire format
(`RouteFrame`) and the upload state machine (`RouteTransfer`). `MapboxRouteSource`
is deliberately thin so the uncompiled surface is as small as possible.

To build it:

1. Sign up at [mapbox.com](https://www.mapbox.com/). A free account is enough
   to start; see *Cost* below.
2. Go to [account.mapbox.com/access-tokens](https://account.mapbox.com/access-tokens/).
   You need **two different tokens**, and this is the part people get wrong.
   - The **public** token (`pk.…`) already exists — it is created with the
     account and labelled *Default public token*. Copy it.
   - The **secret** token (`sk.…`) has to be made: *Create a token*, name it,
     and tick **`DOWNLOADS:READ`** under *Secret scopes*. This is the only
     scope it needs.

   A secret token is displayed **once, at creation**. Copy it then; if you
   lose it, delete it and make another.
3. Put the **secret** one in `~/.gradle/gradle.properties` — outside this repo,
   because it is a credential:
   ```properties
   MAPBOX_DOWNLOADS_TOKEN=sk.ey...
   ```
4. Put the **public** one in `phone/android/local.properties`, which git does
   not track:
   ```properties
   MAPBOX_ACCESS_TOKEN=pk.ey...
   ```
   The build reads it into `BuildConfig.MAPBOX_ACCESS_TOKEN`, and
   `MapboxRouteSource.unavailableReason()` reports in words if it is missing or
   if the two tokens were swapped — which is the failure that otherwise looks
   like being offline.
5. Build with the flag, and **without** `--offline`, since the artifacts are
   not in the local Gradle cache:
   ```sh
   gradle -PwithMapbox=true assembleDebug
   ```

### Which token does what

| | Public `pk.…` | Secret `sk.…` |
|---|---|---|
| Used by | the app, at runtime | Gradle, at build time |
| Purpose | routing requests | downloading the SDK |
| Scope | default | `DOWNLOADS:READ` |
| Lives in | `local.properties` | `~/.gradle/gradle.properties` |
| Shown again? | yes, any time | **no, once only** |

Swapping them fails in two confusing ways: Gradle cannot resolve the
dependency, or routing requests are rejected. Neither says "wrong token".

### Cost

The Navigation SDK is billed per monthly active user with a free allowance, and
the Directions API has its own monthly free request quota. Personal use by one
rider sits far inside both. Mapbox does ask for a card on file before enabling
the Navigation SDK, so check the current rates on their pricing page rather than
trusting this paragraph — they change.

Expect `MapboxRouteSource.kt` to need adjusting on first compile. Its SDK calls
are written from the published API of version 3.6.0 and have not been checked
by a compiler. The SDK's package layout changed at v3 and will change again.

## Why a notification listener

Google Maps exposes no turn-by-turn API. The only way to get maneuvers out of
it without root is to read the ongoing navigation notification it posts while
guiding. That has two consequences worth understanding before relying on it:

1. **The maneuver arrow is a bitmap, not a code.** The turn type can only be
   recovered from the notification's wording, which makes the parser
   locale-specific. `ManeuverParser` matches **English** phrasing only.
2. **The notification layout is private to Maps.** Some versions populate
   `EXTRA_TITLE`/`EXTRA_TEXT`; others ship a custom `RemoteViews`, in which
   case those extras are null and the text has to be scraped by inflating the
   view and walking it. Any Maps release can change this.

Both failure modes degrade to *no turn shown* rather than a wrong turn:
anything the parser cannot confidently identify produces no frame, and the
head unit falls back to `NO ROUTE` after 30 s. That mirrors how the firmware
treats malformed frames — see `src/navigation/TbtParse.c`.

## Setup

1. Open `phone/android` in Android Studio and let it sync.
2. Install on the phone.
3. Grant **notification access** — there is no runtime dialog for this
   permission; the app's button opens
   *Settings → Apps → Special app access → Notification access*.
4. Grant Bluetooth permissions when prompted (Android 12+ asks for
   `BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT`; older releases ask for location,
   which is what gated BLE scanning back then).
5. Power on the head unit with **Navigation = TBT** and **Heart rate = BLE**
   in its settings. Those two are mutually exclusive with ANT+ — the firmware
   enforces it, because ANT+ takes the radio the BLE stack needs.

## Checking it works

Use **Send test turn** on the main screen first. It writes a frame directly,
with no Maps involvement, which separates the two ways this fails:

| Test button | Maps navigation | Diagnosis |
| --- | --- | --- |
| works | works | fine |
| works | nothing | notification parsing — check `Last notification:` on screen |
| nothing | nothing | BLE link — check the status line and the head unit's mode |

The `Last notification:` line shows what the parser made of the most recent
Maps notification, including the raw title/text when it failed. That is the
first thing to look at when Maps changes its format.

## How it stays alive

The BLE link lives in `TbtService`, a foreground service, not in the Activity.
That is not decoration: the phone spends a ride in a pocket, Android destroys
the Activity, and the link has to keep running. An earlier version owned the
link from `MainActivity` and published it through a static field, which leaked
the Activity and left the notification listener writing into an object whose
owner had been torn down -- prompts would have stopped mid-ride with nothing on
screen to say why.

The persistent notification in the shade is the price Android charges for
holding a BLE connection in the background, and it doubles as a status line:
it shows the current connection state without unlocking the phone.

The service is also started from `onListenerConnected()`, so the link comes up
after a reboot without anyone opening the app.

## Wire format

`TbtFrame.kt` encodes it; **`src/navigation/TbtParse.h` in the firmware is the
authority**. If they disagree, the firmware wins.

```
off  size  field
0    1     magic       0x54 ('T')
1    1     version     0x01
2    1     icon_id     0..10, see TBT_Icon_t in src/system/DataCenter.h
3    1     name_len    0..31 bytes of UTF-8 that follow
4    4     distance_m  uint32, little-endian
8    n     street_name UTF-8, no NUL on the wire
```

Service `a3c87500-8ed3-4bdf-8a39-a01bebede295`, device name `pegasus`, with
three characteristics:

| UUID | Direction | Carries |
|---|---|---|
| `a3c87501-…` | write, write-no-response | one live turn |
| `a3c87502-…` | write | one route chunk |
| `a3c87503-…` | read, notify | route transfer progress |

The turn characteristic takes unacknowledged writes and the route one does not,
and that asymmetry is deliberate: a dropped turn is corrected by the next one a
second later, where a dropped route chunk is a permanent hole.

`TbtFrameTest` and `RouteFrameTest` lock the two encoders to their firmware
layouts byte for byte, so if either side drifts, those tests are what should
notice. The authorities are `src/navigation/TbtParse.h` and
`src/navigation/RouteParse.h`.

## Building

**There is no `gradlew` wrapper here, and this Mac has no system Java, Gradle
or Android SDK.** The whole toolchain was downloaded into a scratchpad and
`local.properties` points `sdk.dir` at it. That directory is under `/private/tmp`
and will not survive forever — if it is gone, the toolchain has to be fetched
again (JDK 17, Gradle 8.9, Android cmdline-tools, then `sdkmanager` for the
platform and build-tools).

While it exists, build with:

```sh
K=/private/tmp/claude-501/-Users-woodyzc-Documents-PlatformIO-Projects-Project-Pegasus/\
b0f68de9-bcba-44c3-8a01-10c0b8603552/scratchpad/ktool

cd phone/android
JAVA_HOME="$K/jdk-17.0.20.1+1/Contents/Home" \
GRADLE_USER_HOME="$K/gradle-home" \
"$K/gradle-8.9/bin/gradle" --offline testDebugUnitTest assembleDebug
```

`--offline` matters: the Gradle cache under `gradle-home` already has every
dependency, and without it the build tries to reach the network. It is also the
one flag to drop when building with `-PwithMapbox=true`, because the Mapbox
artifacts are not in that cache and never will be until they are fetched.

Output: `app/build/outputs/apk/debug/app-debug.apk`.

Install over USB with debugging enabled:

```sh
"$K/sdk/platform-tools/adb" install -r app/build/outputs/apk/debug/app-debug.apk
```

## Tests

```
./gradlew :app:test          # or the gradle invocation above
```

Pure JVM, no device needed. They cover the parser (phrase precedence, unit
conversion, refusal to guess) and the encoder (endianness, truncation that
never splits a UTF-8 character). 28 tests as of this writing: 20 in
`ManeuverParserTest`, 8 in `TbtFrameTest`.

## Known gaps

- **English only.** Switching Maps to another language stops the parser
  matching; `ICON_PATTERNS` is where to add phrasing.
- **No lane guidance, no ETA, no next-next maneuver** — the wire format has
  room for one directive at a time.
- **Diagonal arrows don't exist on the head unit.** LVGL's built-in symbol
  font has no slight/sharp glyphs, so those collapse onto plain left/right
  there. The distinction survives in the street line only.

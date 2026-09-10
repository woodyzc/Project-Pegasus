# Pegasus TBT — Android companion

Forwards Google Maps turn-by-turn prompts to the Pegasus head unit over BLE.

**Status: written, never compiled or run.** There is no Android toolchain on
the machine this was authored on, so nothing here has been through a compiler,
let alone a phone. Expect to fix things on first open in Android Studio.

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

Service `a3c87500-8ed3-4bdf-8a39-a01bebede295`,
characteristic `a3c87501-…` (write / write-no-response), device name
`pegasus`.

`TbtFrameTest` locks the encoder to that layout byte for byte, so if either
side drifts, that test is what should notice.

## Tests

```
./gradlew :app:test
```

Pure JVM, no device needed. They cover the parser (phrase precedence, unit
conversion, refusal to guess) and the encoder (endianness, truncation that
never splits a UTF-8 character).

## Known gaps

- **English only.** Switching Maps to another language stops the parser
  matching; `ICON_PATTERNS` is where to add phrasing.
- **No foreground service.** Android may kill the app in the background during
  a long ride. A foreground service with a persistent notification is the
  standard fix and is not implemented yet.
- **No lane guidance, no ETA, no next-next maneuver** — the wire format has
  room for one directive at a time.
- **Diagonal arrows don't exist on the head unit.** LVGL's built-in symbol
  font has no slight/sharp glyphs, so those collapse onto plain left/right
  there. The distinction survives in the street line only.

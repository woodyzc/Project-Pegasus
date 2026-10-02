#pragma once

// The board's power latch.
//
// ---------------------------------------------------------------------------
// Why this file exists at all
// ---------------------------------------------------------------------------
// On the Waveshare ESP32-S3-Touch-LCD-2.8 the battery rail is held up by a
// soft latch, not a mechanical switch. Pressing the power key closes the rail
// long enough for the chip to boot; from then on the rail stays up only
// because firmware drives PWR_LATCH_PIN high. Let go of the key before that
// happens and the board switches off mid-boot.
//
// The Hosyond board had no such circuit -- it ran whenever it had power -- so
// nothing in this firmware ever needed to hold its own rail up, and the
// omission is invisible on USB: the host holds the rail up regardless. It
// appears the first time someone unplugs the cable and rides with it.
//
// So this must be the first thing setup() does, ahead of the display and the
// buses, because every millisecond before it is a millisecond the rider has to
// keep their thumb down.
// ---------------------------------------------------------------------------
//
// The power key (PWR_KEY_PIN) is readable here, but what a press MEANS is
// decided in system/PowerManager.cpp -- it owns the teardown that has to
// happen before the rail goes away, and it already runs the timer to poll on.
// This file keeps the pins and nothing else.

// Latches the power rail on. Call first from setup(), before anything else.
void BoardPower_Init();

// True while the power key is held. Active low, which is the level the vendor
// driver tests at boot to tell "the user pressed the button" from "the rail
// came up on its own".
bool BoardPower_KeyPressed();

// Opens the latch: the rail collapses and the board switches off.
//
// ⚠️ Does not return on battery. Everything that must survive -- the ride
// file closed, the BLE peer told, the card unmounted -- has to have happened
// already. On USB the host may keep the chip alive, so callers must not
// assume this is the last line that ever runs.
void BoardPower_LatchOff();

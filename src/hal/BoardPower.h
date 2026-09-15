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
// NOTE: the power *key* (PWR_KEY_PIN) is not handled here yet. The vendor
// driver reads it for long-press sleep/restart/shutdown; this board branch
// brings up the latch only, and the key is a follow-up along with the IMU and
// the RTC.

// Latches the power rail on. Call first from setup(), before anything else.
void BoardPower_Init();

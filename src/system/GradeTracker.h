#pragma once

#include <stdbool.h>

// The INCLINE cell's source: barometric grade, live.
//
// Subscribes to TOPIC_GPS_INFO, integrates horizontal distance from the
// reported speed, takes altitude from the BMP580, and hands both to the pure
// estimator in Grade.h. That estimator is where the arithmetic and the tests
// live; this file only connects it to the hardware.
//
// ---------------------------------------------------------------------------
// Not gated on the ride being armed
// ---------------------------------------------------------------------------
// Trip, RideStats and Ascent all accumulate only while a ride is armed,
// because they are ride statistics and a number gathered on a desk corrupts
// the summary (CLAUDE.md §8). Grade is not one of those. It is a live reading
// of the road the rider is on, the same kind of thing as SPEED, which is
// deliberately ungated for the same reason -- someone pushing a bike up a
// ramp before pressing "Start new ride" should still see the ramp.
//
// Nothing here is persisted and nothing is summarised, so there is no
// equivalent of the "new ride reports a ride that never happened" failure.
//
// ---------------------------------------------------------------------------
// No barometer, no INCLINE
// ---------------------------------------------------------------------------
// This deliberately does **not** fall back to GNSS altitude. Grade divides
// altitude change by 30m of run, so the receiver's ordinary few metres of
// vertical wander -- the very noise Ascent's band exists to reject -- would
// arrive as tens of percent of grade, swinging either way, on flat ground.
// That is not a degraded reading; it is a number with no relationship to the
// road. The cell reads "--" instead.
void GradeTracker_Init();

// True when a grade is available: a barometer is reading, fixes are arriving,
// and enough ground has been covered to divide by. Ages out, like every live
// reading on this panel.
bool GradeTracker_Have();

// Percent, signed, positive uphill. Meaningless unless GradeTracker_Have().
float GradeTracker_Pct();

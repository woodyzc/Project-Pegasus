#pragma once

#include <stddef.h>
#include <stdint.h>

// Records the ride to a .gpx on the SD card (CLAUDE.md: ride logging).
//
// The counterpart to GpxTrack, which reads a route to follow. Formatting lives
// in GpxWrite.c and is host-tested; this file is the parts that need hardware:
// the card, a queue, and a task to do the writing.
//
// ---------------------------------------------------------------------------
// Why the file is always complete
// ---------------------------------------------------------------------------
// A bike computer is not shut down, it runs out of battery. So the closing
// </trkseg></trk></gpx> is written after every flush and seeked back over
// before the next point is appended. The file on the card therefore parses at
// any instant, and losing power costs at most the few seconds since the last
// flush rather than the whole ride.
//
// The alternative -- write the footer once at the end -- produces a truncated,
// unreadable file in exactly the case that matters most.
// ---------------------------------------------------------------------------
//
// NOTE: no SD card has ever been inserted into this project. GpxWrite.c is
// covered by test/host, but everything below is unverified against hardware.

// Creates the queue and writer task and subscribes to GPS and heart rate.
//
// Position drives the file: a point is written when a fix arrives, and the
// most recent heart rate is attached to it if one arrived in the last ten
// seconds. The strap is never waited for, because the two publish on their own
// unrelated schedules -- so a ride with no strap records a plain track, and a
// strap that drops out mid-ride leaves the rest of the points without a
// reading rather than repeating its last one.
//
// Safe to call with no card present: nothing is recorded and nothing fails.
// Call after the card mount has been attempted.
void RideLog_Init();

// True once a file has been opened and a point written.
bool RideLog_IsRecording();

// Name of the file being written, or "" before the first point.
const char *RideLog_FileName();

// Points written so far, for the settings page to show that it is working.
uint32_t RideLog_PointCount();

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
// Writes a short ride to the card and reads it back.
//
// Recording starts on the first GPS fix, so with no GNSS module attached the
// whole write path is unreachable: the card can be proved readable by the map
// and the route picker, while creating a file on it has never once been tried.
// This closes that gap, and it does so by driving the *real* recorder -- the
// same OpenFile, AppendPoint and footer rewrite a ride uses -- rather than a
// parallel copy that could pass while the real one fails.
//
// The file is a genuine ride file of three synthetic points, stamped
// 2000-01-01 so it is recognisable on a computer and so re-running overwrites
// it rather than filling the card.

typedef enum {
    RIDELOG_SELFTEST_IDLE = 0,
    RIDELOG_SELFTEST_RUNNING,
    RIDELOG_SELFTEST_PASS,
    RIDELOG_SELFTEST_FAIL,
} RideLogSelfTest_t;

// Asks for a run. Returns false, with the reason in the message below, if
// there is no card or a ride is actually being recorded -- the test writes
// through the recorder's own state, so it must never run over a live ride.
//
// The work happens on the writer task, not here: SD writes on the LVGL thread
// are what tripped the task watchdog once already.
bool RideLog_SelfTestStart();

RideLogSelfTest_t RideLog_SelfTestState();

// What happened, in a sentence fit to show on the panel.
const char *RideLog_SelfTestMessage();

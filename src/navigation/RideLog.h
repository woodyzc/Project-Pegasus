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

// Creates the queue and writer task and subscribes to GPS. Safe to call with
// no card present: nothing is recorded and nothing fails. Call after the card
// mount has been attempted.
void RideLog_Init();

// True once a file has been opened and a point written.
bool RideLog_IsRecording();

// Name of the file being written, or "" before the first point.
const char *RideLog_FileName();

// Points written so far, for the settings page to show that it is working.
uint32_t RideLog_PointCount();

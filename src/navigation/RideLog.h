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

// Ends the ride being recorded, so the next fix opens a new file.
//
// Recording starts by itself on the first fix and never stops, which means the
// file is delimited by the power switch: the drive to the trailhead lands in
// it, two rides on one charge become one file, and a reboot splits one ride
// into two. This is the rider saying where a ride actually begins.
//
// Deliberately not a gate on recording. A press that must be remembered is a
// press that will be forgotten, and on a touch-only panel the cost of missing
// it would be the whole ride; the cost of not pressing this is a file with the
// drive at the start of it, which can be deleted afterwards.
//
// Queued to the writer task rather than done here. Returns false only if the
// queue is full, meaning the request did not get through.
bool RideLog_StartNewRide();

// Points written so far, for the settings page to show that it is working.
uint32_t RideLog_PointCount();

// Ends the ride being recorded, so the next fix opens a new file.
//
// Recording starts by itself on the first fix and never stops, which means the
// file is delimited by the power switch: the drive to the trailhead lands in
// it, two rides on one charge become one file, and a reboot splits one ride
// into two. This is the rider saying where a ride actually begins.
//
// Deliberately not a gate on recording. A press that must be remembered is a
// press that will be forgotten, and on a touch-only panel the cost of missing
// it would be the whole ride; the cost of not pressing this is a file with the
// drive at the start of it, which can be deleted afterwards.
//
// Queued to the writer task rather than done here. Returns false only if the
// queue is full, meaning the request did not get through.
bool RideLog_StartNewRide();

// Points written so far, for the settings page to show that it is working.
uint32_t RideLog_PointCount();

// How many rides have been closed by the inactivity timeout since boot.
//
// Recording starts on its own and used to never stop, so a device left in a
// bag wrote the walk home, the drive and the next morning's train into one
// ride -- and held deep sleep off throughout. Fifteen minutes without movement
// now ends it, and moving again starts a new one.
//
// Surfaced because an auto-end is otherwise completely silent: a rider would
// discover at the end of the day that one ride is in two files, with nothing
// anywhere saying why.
uint32_t RideLog_AutoEndCount();


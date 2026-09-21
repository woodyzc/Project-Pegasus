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
// Copies the current ride's file name into `out`, NUL-terminated, and returns
// true when there is one. `out` is left empty and false returned otherwise.
//
// A copy rather than the pointer this used to hand out. s_name is written a
// field at a time by the writer task on Core 0 while the settings page and the
// ride summary read it from Core 1, so a caller holding the raw pointer could
// format a name that was half the old ride and half the new one. Taking the
// same lock the odometer uses makes the read atomic with respect to the write.
bool RideLog_FileName(char *out, size_t out_size);

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

// Why nothing is being written, when the ride is armed and nothing is being
// written. Returns nullptr when there is nothing wrong -- armed, no fix yet,
// which is an ordinary state that needs no explanation.
//
// This exists because the panel used to answer that question with "waiting
// for fix" unconditionally, and was wrong in the one case that matters. A
// road test produced a screen counting 31km/h with the trip climbing and
// turns updating -- a fix beyond any doubt -- while the log wrote nothing,
// and the only thing on the device that could have explained it said to go
// looking for a GPS fault.
const char *RideLog_NotRecordingReason();

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
// ride -- and held deep sleep off throughout. An hour without movement now
// ends it, and it stays ended: the timeout disarms, so moving again does not
// quietly open another file. Only "Start new ride" records again.
//
// Surfaced because an auto-end is otherwise completely silent: a rider would
// discover at the end of the day that one ride is in two files, with nothing
// anywhere saying why.
uint32_t RideLog_AutoEndCount();

// How many ride files have been deleted this boot for holding fewer than two
// points. One point is a place, not a journey.
//
// Surfaced because a silent delete and a failed write look the same from
// outside -- the file is not there either way -- and only one of them is fine.
uint32_t RideLog_DiscardedCount();

// ---- Recording only ever starts by hand ----
//
// RideLog_StartNewRide() is the only thing that arms recording; nothing else
// -- not a fix, not movement, not a reboot -- does. Before that, the first
// valid fix opened a file, so the device recorded the drive to the start, the
// walk from the car, and the next morning's train, all as rides.
//
// The cost of the trade is real and points the other way: forgetting to start
// loses a whole ride, where forgetting to finish only left a file to delete.
// The dashboard's REC indicator and its nudge when the rider is moving and
// not recording exist to pay that cost down, and this state is what they read.
bool RideLog_IsArmed();

// True once a ride has been opened this boot, and stays true afterwards.
//
// With RideLog_IsArmed() it tells the three states apart: armed is a ride
// under way, recorded-and-not-armed is a ride that finished, and neither is a
// device that has done nothing since power-on. PowerManager needs that last
// distinction -- a rider who pressed "finish" has said they are done and the
// board may sleep soon, where a board that has been told nothing may be
// waiting on a rider who has not started yet.
bool RideLog_HasRecorded();

// The rider says the ride is over. Closes the file and disarms, so moving
// again does not start another one. Posts through the writer queue like
// RideLog_StartNewRide(), so a point published a moment earlier still lands
// in the file; false means the queue was full and nothing was ended.
//
// An hour without movement does the same thing on the rider's behalf -- see
// RideLog_AutoEndCount().
bool RideLog_FinishRide();

// Closes the open ride file before the chip restarts. Registered automatically
// as an ESP-IDF shutdown handler by RideLog_Init().
//
// Deliberately does NOT disarm: a restart is not the rider saying they are
// done. The armed flag is persisted, so recording resumes by itself on the
// next fix -- in a NEW file, because appending would mean reading the GPX back
// and stripping its footer, on a card, at boot. Two files for one ride is an
// annoyance a laptop fixes; a corrupted ride is not.
//
// Goes through the writer queue rather than touching the file, because the
// file belongs to that task. Closing it from here would be the same race that
// once let NimBLE be deinitialised under its own supervisor.
void RideLog_Shutdown();


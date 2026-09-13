#pragma once

#include <stdint.h>

// Ride averages and maxima: RideStatsCore's rules, plus the DataCenter
// subscriptions that feed them and the locking that lets Core 0 update while
// Core 1 draws.
//
// It subscribes to GPS and heart rate itself rather than being driven by the
// dashboard, for the reason Trip records: statistics accumulated inside a
// redraw callback stop accumulating whenever the rider is looking at the map
// or the settings page. An average speed that only counts the time you spent
// watching the average is not an average speed.
//
// Not persisted, unlike the trip odometer. These describe the ride in
// progress, and a maximum speed carried over from last week would be a lie
// told with confidence. Trip_Reset() clears them alongside the distance.

void RideStats_Init();

// Clears everything. Called when the rider resets the trip, since the two
// describe the same ride.
void RideStats_Reset();

// Total climbing and dropping so far, in metres. The filtering that makes
// these honest is Ascent.h, which explains why a sum of altitude differences
// is not an option.
float RideStats_AscentM();
float RideStats_DescentM();

// Safe to call from either core.
float RideStats_MaxSpeedKmh();
float RideStats_AvgSpeedKmh();
uint8_t RideStats_MaxBpm();
uint8_t RideStats_AvgBpm();

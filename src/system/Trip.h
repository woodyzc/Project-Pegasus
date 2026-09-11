#pragma once

#include "DataCenter.h"

// The trip odometer: TripAccum's rule, plus the NVS storage that lets it
// survive a reboot, plus the locking that lets Core 0 update it while Core 1
// draws it.
//
// It subscribes to TOPIC_GPS_INFO itself rather than being driven by the
// dashboard. Page_Dashboard used to accumulate distance inside its own redraw
// callback, which meant the odometer silently stopped counting whenever the
// rider was looking at the map or the settings page -- the distance was only
// measured while the screen that displays it happened to be loaded.

// Loads the saved distance and subscribes to GPS. Call after Settings_Init().
void Trip_Init();

// Persists the distance if enough has accumulated since the last write. Call
// from loop(); doing it inline would put an NVS write, which can take tens of
// milliseconds, inside the GPS task's publish path.
void Trip_Service();

// Distance so far. Safe to call from either core.
double Trip_Km();

// Zeroes the trip and writes that through immediately -- a reset the rider
// asked for must not be undone by a flat battery ten seconds later.
void Trip_Reset();

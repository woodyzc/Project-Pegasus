#pragma once

#include "../system/RideSummary.h"

// The report a ride ends with.
//
// Shown two ways, both from the settings page: on demand for the ride in
// progress, and automatically when the rider starts a new one -- which is the
// moment it matters most, because everything in it is about to be zeroed.
//
// A modal on LVGL's top layer, like the file-transfer and sleep panels, but
// unlike those it is dismissible: nothing is held open behind it and there is
// always somewhere to go back to.

// Fills `out` from Trip, RideStats and RideLog as they stand right now. Call
// before resetting anything.
void RideSummary_Capture(RideSummary_t *out);

// Puts the report on screen. `ended` chooses the wording between a ride that
// has just been closed and one still in progress.
void Overlay_RideSummary_Show(const RideSummary_t *summary, bool ended);

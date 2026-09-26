#pragma once

// A banner for a call, a text or a chat message pushed from the phone
// (src/system/AlertFrame.h, TOPIC_PHONE_ALERT).
//
// ---------------------------------------------------------------------------
// Where it draws, and why that is the whole design
// ---------------------------------------------------------------------------
// Over the metric cells -- SPEED, TRIP, HEART RATE, ELEVATION -- and never
// over the navigation region above them.
//
// Those metrics are things a rider can look up again a second later, or work
// out from the road. A turn is not: a banner that hides the arrow at the
// moment the junction arrives has taken something back that cannot be
// recovered, in exchange for telling the rider about a message they cannot
// answer. So the top 184px is off limits, and this occupies the band below it.
//
// That also makes the banner harmless on the map page, where the lower band
// shows the road already ridden.
//
// ---------------------------------------------------------------------------
// It lives on lv_layer_top(), not on a page
// ---------------------------------------------------------------------------
// The phone rings whatever the rider is looking at, and a page would only
// carry it on the dashboard. The same choice Overlay_FileTransfer made, for
// the same reason.

// Starts the LVGL timer that watches TOPIC_PHONE_ALERT.
//
// Call after DataCenter_Init() and BEFORE LvglTask_Start(), for the reason
// PowerManager_Init() has the same precondition: this creates LVGL objects,
// LVGL here has no lock, and everything lv_* belongs to that task once it is
// running. Creating the timer before the task starts keeps even the creation
// single-threaded.
void Overlay_Alert_Init();

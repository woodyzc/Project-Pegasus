#pragma once

#include <lvgl.h>

// Main bike-computer dashboard: speed meter, heart-rate label, slope icon,
// and battery indicator. Subscribes itself to the DataCenter's GPS_Info and
// Sensor/HeartRate topics (see src/system/DataCenter.h) and keeps the
// speed/heart-rate widgets in sync as new data is published.
//
// Preconditions (caller's responsibility):
//   - lv_init() and DataCenter_Init() have already run.
//   - Called from Core 1 only. Like all LVGL object creation, this is not
//     safe to call from Core 0 -- e.g. call it from setup() before
//     LvglTask_Start(), not from a sensor task.
//
// `parent`: object to build the dashboard under, or nullptr for lv_scr_act().
void Page_Dashboard_Create(lv_obj_t *parent);

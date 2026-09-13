#pragma once

#include "../../src/system/DataCenter.h"
#include "../../src/system/Settings.h"
#include "../../src/hal/Battery.h"

// Everything the stubs hand back, in one place so a scene is a few lines.
struct SimState {
    unsigned int millis = 1000;

    SpeedUnit_t speed_unit = SPEED_UNIT_KMH;
    NavMode_t nav_mode = NAV_MODE_TBT;

    double trip_km = 0.0;
    float avg_kmh = 0.0f;
    float max_kmh = 0.0f;
    uint8_t avg_bpm = 0;
    uint8_t max_bpm = 0;
    float ascent_m = 0.0f;

    bool have_tbt = false;
    TBT_Directive_t tbt{};

    bool have_hr = false;
    HeartRate_t hr{};

    bool have_gps = false;
    GPS_Info_t gps{};

    Battery_t battery{};

    // How many .gpx files the card appears to hold, for the route picker.
    size_t gpx_files = 0;
};

extern SimState g_sim;

// Delivers a topic to whoever subscribed, the way DataCenter would on the
// board. The payload is ignored by the page, which pulls the current value
// instead, so passing nullptr is normal.
void Sim_Publish(const char *topic, const void *data, uint32_t size);

// Everything Page_Dashboard reaches for that is not the layout itself.
//
// Deliberately dumb: each of these returns a fixed, obvious value, because the
// simulator exists to answer questions about geometry -- does the unit fit
// beside the number, does the street name clip, do the two columns balance --
// and not about behaviour. Behaviour is covered by the host suites in
// test/host, which compile the real modules.
//
// The values are settable from sim_main so one build can render several
// states: a fresh boot, a ride in progress, an imminent turn.

#include <stdint.h>
#include <string.h>

#include "../../src/system/DataCenter.h"
#include "../../src/system/Settings.h"
#include "../../src/navigation/GpxTrack.h"
#include "../../src/system/Trip.h"
#include "../../src/system/RideStats.h"
#include "../../src/hal/Battery.h"
#include "../../src/ui/MapView.h"
#include "../../src/ui/RoadView.h"

#include "../../src/system/PageManager/PageManager.h"

#include "sim_state.h"
#include "sim_tick.h"

SimState g_sim;

unsigned int sim_millis(void) {
    return g_sim.millis;
}

// ---- Settings ----------------------------------------------------------
SpeedUnit_t Settings_GetSpeedUnit() { return g_sim.speed_unit; }
const char *Settings_SpeedUnitLabel() {
    return g_sim.speed_unit == SPEED_UNIT_MPH ? "mph" : "km/h";
}
const char *Settings_DistanceUnitLabel() {
    return g_sim.speed_unit == SPEED_UNIT_MPH ? "mi" : "km";
}
float Settings_SpeedFromKmh(float kmh) {
    return g_sim.speed_unit == SPEED_UNIT_MPH ? kmh * 0.621371f : kmh;
}
float Settings_DistanceFromKm(float km) {
    return g_sim.speed_unit == SPEED_UNIT_MPH ? km * 0.621371f : km;
}
NavMode_t Settings_GetNavMode() { return g_sim.nav_mode; }
uint8_t Settings_GetHrMaxBpm() { return 185; }
uint8_t Settings_GetHrRestBpm() { return 55; }

// ---- Trip and ride statistics -----------------------------------------
double Trip_Km() { return g_sim.trip_km; }
void Trip_Reset() {}
float RideStats_AvgSpeedKmh() { return g_sim.avg_kmh; }
float RideStats_MaxSpeedKmh() { return g_sim.max_kmh; }
uint8_t RideStats_AvgBpm() { return g_sim.avg_bpm; }
uint8_t RideStats_MaxBpm() { return g_sim.max_bpm; }
void RideStats_Init() {}
void RideStats_Reset() {}

// ---- The card, which the simulator never has --------------------------
size_t GpxTrack_PointCount() { return 0; }
bool GpxTrack_CardMounted() { return false; }

// ---- DataCenter --------------------------------------------------------
// Subscriptions are accepted and ignored; the simulator drives the page by
// calling its render helpers directly, which is also what the refresh timer
// does on the board.
const char *const TOPIC_GPS_INFO = "GPS_Info";
const char *const TOPIC_HEART_RATE = "Sensor/HeartRate";
const char *const TOPIC_IMU_DATA = "Sensor/IMU";
const char *const TOPIC_BATTERY = "Sensor/Battery";
const char *const TOPIC_NAV_TBT = "Nav/TBT";

Account::Account(const char *id, DataCenter_Callback_t callback, void *user_arg)
    : ID(id), Callback(callback), UserArg(user_arg) {}

// Subscriptions are remembered and really delivered, rather than accepted and
// dropped. The page marks itself dirty from these callbacks, so a simulator
// that skipped them would need a test-only hook poked into the page -- and the
// callback path is worth exercising anyway, since it is where the page decides
// what to redraw.
namespace {
struct Sub {
    const char *topic;
    Account *account;
};
Sub s_subs[16];
size_t s_sub_count = 0;
} // namespace

void DataCenter_Init() {}

bool DataCenter_Subscribe(const char *topic, Account *account) {
    if (s_sub_count >= sizeof(s_subs) / sizeof(s_subs[0])) {
        return false;
    }
    s_subs[s_sub_count].topic = topic;
    s_subs[s_sub_count].account = account;
    s_sub_count++;
    return true;
}

bool DataCenter_Unsubscribe(const char *, Account *) { return true; }

bool DataCenter_Publish(const char *topic, const void *data) {
    Sim_Publish(topic, data, 0);
    return true;
}

void Sim_Publish(const char *topic, const void *data, uint32_t size) {
    for (size_t i = 0; i < s_sub_count; i++) {
        if (strcmp(s_subs[i].topic, topic) != 0) {
            continue;
        }
        if (s_subs[i].account->Callback != nullptr) {
            s_subs[i].account->Callback(topic, data, size, s_subs[i].account->UserArg);
        }
    }
}

bool DataCenter_Pull(const char *topic, void *out, uint32_t size) {
    if (strcmp(topic, TOPIC_NAV_TBT) == 0 && size == sizeof(TBT_Directive_t)) {
        memcpy(out, &g_sim.tbt, sizeof(TBT_Directive_t));
        return g_sim.have_tbt;
    }
    if (strcmp(topic, TOPIC_BATTERY) == 0 && size == sizeof(Battery_t)) {
        memcpy(out, &g_sim.battery, sizeof(Battery_t));
        return true;
    }
    if (strcmp(topic, TOPIC_HEART_RATE) == 0 && size == sizeof(HeartRate_t)) {
        memcpy(out, &g_sim.hr, sizeof(HeartRate_t));
        return g_sim.have_hr;
    }
    if (strcmp(topic, TOPIC_GPS_INFO) == 0 && size == sizeof(GPS_Info_t)) {
        memcpy(out, &g_sim.gps, sizeof(GPS_Info_t));
        return g_sim.have_gps;
    }
    return false;
}

// ---- The map, which only the GPX navigation mode builds ----------------
void MapView_Create(MapView_t *, lv_obj_t *, lv_coord_t, lv_coord_t, lv_coord_t, lv_coord_t,
                    lv_point_t *, MapPoint_t *, size_t) {}
void MapView_FitTrack(MapView_t *) {}
void MapView_SetPosition(MapView_t *, const GPS_Info_t *) {}
void RoadView_Attach(MapView_t *) {}
void RoadView_Refresh() {}

// ---- Page navigation ---------------------------------------------------
// The simulator renders one page and never leaves it, so a tap that would
// push another is accepted and ignored.
bool PageManager::Push(const char *, const PageBase::Stash_t *) { return true; }

#include "RideStats.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "DataCenter.h"
#include "RideStatsCore.h"

namespace {

RideStats_t s_stats;
SemaphoreHandle_t s_lock = nullptr;

// When the last speed sample arrived, so the interval between samples can be
// measured. The receiver's own timestamps are not used: they are only valid
// once time is resolved, and the average has to work from the first fix.
uint32_t s_last_speed_ms = 0;
bool s_have_speed = false;

struct Locked {
    Locked() {
        if (s_lock != nullptr) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
    }
    ~Locked() {
        if (s_lock != nullptr) {
            xSemaphoreGive(s_lock);
        }
    }
};

void OnGpsPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)user_arg;

    if (data == nullptr || size != sizeof(GPS_Info_t)) {
        return;
    }
    const GPS_Info_t *gps = (const GPS_Info_t *)data;
    if (!gps->fix_valid) {
        // A dropped fix is a gap, not a stop. Forgetting the timestamp means
        // the next sample starts a fresh interval instead of charging the
        // average for however long the receiver was lost.
        Locked guard;
        s_have_speed = false;
        return;
    }

    const uint32_t now = millis();

    Locked guard;
    if (s_have_speed) {
        // Unsigned, so a tick counter wrapping after 49 days still yields the
        // real interval rather than an enormous one.
        const double dt = (double)(uint32_t)(now - s_last_speed_ms) / 1000.0;
        RideStatsCore_AddSpeed(&s_stats, gps->speed * 3.6f, dt);
    }
    s_last_speed_ms = now;
    s_have_speed = true;
}

void OnHeartRatePublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)user_arg;

    if (data == nullptr || size != sizeof(HeartRate_t)) {
        return;
    }
    const HeartRate_t *hr = (const HeartRate_t *)data;

    Locked guard;
    RideStatsCore_AddHeartRate(&s_stats, hr->bpm);
}

Account s_gps_account("RideStats/GPS", OnGpsPublished);
Account s_hr_account("RideStats/HR", OnHeartRatePublished);

} // namespace

void RideStats_Init() {
    s_lock = xSemaphoreCreateMutex();
    RideStatsCore_Reset(&s_stats);
    s_have_speed = false;

    DataCenter_Subscribe(TOPIC_GPS_INFO, &s_gps_account);
    DataCenter_Subscribe(TOPIC_HEART_RATE, &s_hr_account);
}

void RideStats_Reset() {
    Locked guard;
    RideStatsCore_Reset(&s_stats);
    s_have_speed = false;
}

float RideStats_MaxSpeedKmh() {
    Locked guard;
    return s_stats.max_kmh;
}

float RideStats_AvgSpeedKmh() {
    Locked guard;
    return RideStatsCore_AvgSpeedKmh(&s_stats);
}

uint8_t RideStats_MaxBpm() {
    Locked guard;
    return s_stats.max_bpm;
}

uint8_t RideStats_AvgBpm() {
    Locked guard;
    return RideStatsCore_AvgBpm(&s_stats);
}

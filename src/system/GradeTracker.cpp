#include "GradeTracker.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../hal/Barometer.h"
#include "DataCenter.h"
#include "Grade.h"

namespace {

// Beyond this with no usable fix the reading is stale and the cell blanks.
// Grade.h's own window bounds how long a *stopped* rider keeps a grade; this
// one covers fixes stopping altogether -- a phone app closed mid-ride, or a
// receiver that has lost the sky.
constexpr uint32_t FIX_STALE_MS = 10000;

Grade_t s_grade;
double s_dist_m = 0.0;
uint32_t s_last_fix_ms = 0;
bool s_have_fix = false;
SemaphoreHandle_t s_lock = nullptr;

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

    // A publish without a valid fix carries no speed worth integrating --
    // GPS_Reader publishes anyway so a climbing num_sv can distinguish
    // "acquiring" from "no module" (CLAUDE.md §8).
    // Fresh, not HaveAltitude: the latter never goes false once the sensor has
    // read once, so a BMP580 that falls off the bus mid-ride would freeze the
    // altitude while distance kept advancing -- and rise-over-run with a
    // frozen rise is a confident +0.0% on a climb. Blanking is the honest
    // answer (Barometer.h).
    if (!gps->fix_valid || !Barometer_AltitudeFresh()) {
        Locked guard;
        s_have_fix = false;
        return;
    }

    const uint32_t now = millis();

    Locked guard;
    if (s_have_fix) {
        const double dt = (double)(uint32_t)(now - s_last_fix_ms) / 1000.0;
        // Integrated from the reported speed rather than measured between
        // consecutive coordinates. Both are available; speed is the smoother
        // of the two at the low end, and a run that jitters is a grade that
        // jitters, because the run is the denominator.
        s_dist_m += (double)gps->speed * dt;
        Grade_Feed(&s_grade, s_dist_m, Barometer_AltitudeM(), now);
    }
    s_last_fix_ms = now;
    s_have_fix = true;
}

Account s_gps_account("GradeTracker/GPS", OnGpsPublished);

} // namespace

void GradeTracker_Init() {
    s_lock = xSemaphoreCreateMutex();
    Grade_Reset(&s_grade);
    s_dist_m = 0.0;
    s_have_fix = false;

    DataCenter_Subscribe(TOPIC_GPS_INFO, &s_gps_account);
}

bool GradeTracker_Have() {
    Locked guard;
    if (!s_have_fix || !Grade_Have(&s_grade)) {
        return false;
    }
    return (uint32_t)(millis() - s_last_fix_ms) <= FIX_STALE_MS;
}

float GradeTracker_Pct() {
    Locked guard;
    return Grade_Pct(&s_grade);
}

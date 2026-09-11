#include "Trip.h"

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "TripAccum.h"

namespace {

constexpr char NVS_NAMESPACE[] = "pegasus";
constexpr char KEY_TRIP_M[] = "trip_m";

// How much riding may go unsaved. At 250m a 50km ride costs 200 writes, which
// against NVS wear levelling is nothing, and the most a power cut can lose is
// 250m of a ride -- a quarter of the smallest unit the dashboard displays.
constexpr double SAVE_INTERVAL_M = 250.0;

// ...and a time bound as well, so a slow rider who stops for lunch after 200m
// still has that 200m written before the battery goes.
constexpr uint32_t SAVE_INTERVAL_MS = 60000;

Preferences s_prefs;
bool s_ready = false;

TripAccum_t s_accum;
SemaphoreHandle_t s_lock = nullptr;

double s_unsaved_m = 0.0;
uint32_t s_last_save_ms = 0;

// Guards every access to s_accum. The GPS task on Core 0 writes it from the
// publish callback while the LVGL task on Core 1 reads it to draw, and a double
// is not written atomically on this chip -- an unlucky read would see half of
// one value and half of another.
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

// Writes the value out. The caller clears the unsaved counter under the lock;
// doing it here would race with the GPS callback adding to it, and metres
// ridden during the write would be discarded instead of carried forward.
void Save(double metres) {
    if (!s_ready) {
        return;
    }
    s_prefs.putDouble(KEY_TRIP_M, metres);
}

void OnGpsPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)user_arg;

    if (data == nullptr || size != sizeof(GPS_Info_t)) {
        return;
    }
    const GPS_Info_t *gps = (const GPS_Info_t *)data;

    Locked guard;
    s_unsaved_m += TripAccum_AddFix(&s_accum, gps->fix_valid, gps->lat, gps->lon);
}

Account s_gps_account("Trip/GPS", OnGpsPublished);

} // namespace

void Trip_Init() {
    s_lock = xSemaphoreCreateMutex();

    double saved = 0.0;
    s_ready = s_prefs.begin(NVS_NAMESPACE, false);
    if (s_ready) {
        saved = s_prefs.getDouble(KEY_TRIP_M, 0.0);
    }

    // TripAccum_Init screens the stored value: NVS can hand back anything a
    // previous build wrote, and a negative or NaN total would poison every
    // reading after it.
    TripAccum_Init(&s_accum, saved);
    s_unsaved_m = 0.0;
    s_last_save_ms = millis();

    DataCenter_Subscribe(TOPIC_GPS_INFO, &s_gps_account);
}

void Trip_Service() {
    double metres = 0.0;
    bool due = false;

    {
        Locked guard;
        const bool dirty = s_unsaved_m > 0.0;
        due = (s_unsaved_m >= SAVE_INTERVAL_M) ||
              (dirty && (millis() - s_last_save_ms) >= SAVE_INTERVAL_MS);
        if (due) {
            metres = TripAccum_Metres(&s_accum);
            // Cleared here, with the total read, so the two agree. Metres that
            // arrive during the write below land on a fresh counter and are
            // carried into the next save rather than lost.
            s_unsaved_m = 0.0;
            s_last_save_ms = millis();
        }
    }

    if (!due) {
        return;
    }

    // Deliberately outside the lock: an NVS write can take tens of
    // milliseconds, and holding the mutex across it would stall the GPS task
    // mid-publish.
    Save(metres);
}

double Trip_Km() {
    Locked guard;
    return TripAccum_Metres(&s_accum) / 1000.0;
}

void Trip_Reset() {
    {
        Locked guard;
        TripAccum_Reset(&s_accum);
        s_unsaved_m = 0.0;
        s_last_save_ms = millis();
    }
    Save(0.0);
}

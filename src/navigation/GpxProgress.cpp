#include "GpxProgress.h"

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>

#include "../hal/Imu.h"
#include "../system/DataCenter.h"
#include "../system/Stillness.h"
#include "GpxTrack.h"
#include "TrackProgress.h"

namespace {

TrackProgress_t s_progress;

// What the progress was measured against. Point count ALONE is not an
// identity: two routes with the same number of points are not unusual -- a
// re-export of the same ride, two tracks from the same generator -- and the
// second would silently inherit the first's ridden stretch. The name is what
// actually distinguishes them.
size_t s_track_points = 0;
char s_track_name[32] = {0};

// Counted on the GPS task, consumed on the LVGL task. A count rather than a
// flag so the consumer can tell "something new" without a handshake, the same
// way Page_Map's s_fix_seq does.
volatile uint32_t s_fix_seq = 0;
uint32_t s_fed_seq = 0;

uint32_t s_feed_us = 0;

void OnGpsPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)user_arg;
    // A valid fix, never just a publish: GPS_Reader publishes at 1Hz without
    // one while it acquires, and there is nothing to place on the route then.
    if (data == nullptr || size < sizeof(GPS_Info_t)) {
        return;
    }
    if (((const GPS_Info_t *)data)->fix_valid) {
        s_fix_seq++;
    }
}

Account s_gps_account("GpxProgress/GPS", OnGpsPublished);

// Drops the progress when the loaded track is not the one it describes.
// Called from both entry points, so a mark is never read against a track it
// was not measured on -- the route picker can replace the track between two
// fixes, and indoors there may be no next fix at all.
void SyncTrack() {
    const size_t points = GpxTrack_PointCount();
    const char *name = GpxTrack_LoadedName();
    if (name == nullptr) {
        name = "";
    }
    if (points == s_track_points &&
        strncmp(s_track_name, name, sizeof(s_track_name) - 1) == 0) {
        return;
    }
    s_track_points = points;
    strncpy(s_track_name, name, sizeof(s_track_name) - 1);
    s_track_name[sizeof(s_track_name) - 1] = '\0';
    TrackProgress_Reset(&s_progress);
}

void TimerCallback(lv_timer_t *timer) {
    (void)timer;
    GpxProgress_Service();
}

} // namespace

void GpxProgress_Init() {
    TrackProgress_Reset(&s_progress);
    DataCenter_Subscribe(TOPIC_GPS_INFO, &s_gps_account);
    // 250ms against a 1Hz fix: a fix waits at most a quarter of a second, and
    // a tick with nothing new costs one comparison.
    lv_timer_create(TimerCallback, 250, nullptr);
}

void GpxProgress_Service() {
    const uint32_t seq = s_fix_seq;
    if (seq == s_fed_seq) {
        return;
    }
    s_fed_seq = seq;

    SyncTrack();
    if (s_track_points < 2) {
        return;
    }

    // The latest fix, not every one: if two arrived inside a tick, the older
    // is already history. The tracker measures direction between the fixes it
    // is given, so skipping one only lengthens a step.
    GPS_Info_t gps;
    if (!DataCenter_Pull(TOPIC_GPS_INFO, &gps, sizeof(gps)) || !gps.fix_valid) {
        return;
    }
    // A stopped bike's wander is not progress. The mark only moves forward,
    // so a receiver walking a few metres while the rider waits at a junction
    // could push it round the corner they have not taken yet -- the same
    // reason SPEED, the odometer and the climb total all ignore these fixes
    // (CLAUDE.md section 2, Stillness.h).
    if (Stillness_FixIsDrift(Imu_IsStill(), gps.speed)) {
        return;
    }

    const uint32_t started = micros();
    TrackProgress_Feed(&s_progress, GpxTrack_Buffer(), (int32_t)lround(gps.lat * 1e7),
                       (int32_t)lround(gps.lon * 1e7));
    s_feed_us = micros() - started;
}

bool GpxProgress_Mark(size_t *out_mark) {
    SyncTrack();
    if (!s_progress.have_mark || out_mark == nullptr) {
        return false;
    }
    *out_mark = s_progress.mark;
    return true;
}

uint32_t GpxProgress_LastFeedUs() {
    return s_feed_us;
}

#include "RideLog.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "../system/DataCenter.h"
#include "../system/TripAccum.h"
#include "GpxTrack.h"
#include "GpxWrite.h"

namespace {

// One point per second of riding; 32 is half a minute of slack between the GPS
// task and the writer. Deep enough to absorb a slow card, shallow enough that
// a stuck writer is noticed as dropped points rather than as RAM disappearing.
constexpr size_t QUEUE_DEPTH = 32;

// A point is worth recording if the rider has moved this far since the last
// one. Below it, a parked bike would fill the card with the receiver's own
// wander -- the same drift TripAccum's floor exists to reject.
constexpr double MIN_POINT_SPACING_M = 5.0;

// ...but record something at least this often regardless, so a long wait at
// traffic lights still appears in the track as elapsed time rather than as a
// gap that looks like a dropout.
constexpr uint32_t MAX_POINT_INTERVAL_MS = 30000;

// Flush this often. The cost of losing power between flushes is this many
// seconds of track; the cost of flushing more often is card wear.
constexpr uint32_t FLUSH_INTERVAL_MS = 10000;

typedef struct {
    double lat;
    double lon;
    float alt;
    bool has_time;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} RideLogPoint_t;

QueueHandle_t s_queue = nullptr;
File s_file;
// Read from the UI core via RideLog_IsRecording(), written by the writer task.
volatile bool s_recording = false;
// Set once the card has refused us, so the ride is not spent retrying.
bool s_failed = false;
char s_name[48] = "";
volatile uint32_t s_points = 0;

// Where the track points end and the footer begins. Every append seeks here
// first, overwriting the previous footer.
uint32_t s_body_end = 0;
uint32_t s_last_flush_ms = 0;

// Filter state. Touched only by the GPS publish callback, which is a single
// task, so it needs no lock.
double s_last_lat = 0.0;
double s_last_lon = 0.0;
bool s_has_last = false;
uint32_t s_last_queued_ms = 0;

// Picks a name no existing file has. With a resolved time the timestamp is
// already unique; without one, walk the sequence until a free slot appears.
bool ChooseFileName(const RideLogPoint_t &point, char *out, size_t out_size) {
    if (point.has_time) {
        return GpxWrite_FileName(out, out_size, true, point.year, point.month, point.day,
                                 point.hour, point.minute, point.second, 0) > 0;
    }

    // Bounded: a card holding a thousand unnamed rides is a card that needs
    // emptying, and an unbounded search would hang the writer task.
    for (unsigned seq = 1; seq <= 1000; seq++) {
        if (GpxWrite_FileName(out, out_size, false, 0, 0, 0, 0, 0, 0, seq) == 0) {
            return false;
        }
        if (!SD_MMC.exists(out)) {
            return true;
        }
    }
    return false;
}

// Writes the closing tags at the end of the body and leaves the position back
// at the body end, so the next point overwrites them.
//
// Called after *every* point, not on the flush interval. Appending a point
// overwrites the previous footer, so a file whose footer were only rewritten
// every ten seconds would spend those ten seconds with no closing tags at all
// -- which is precisely the state this scheme exists to avoid. Writing the
// footer here costs thirty buffered bytes; flushing is what costs card time,
// and that is still rate-limited.
//
// Honest about the limit: this makes the file valid at every flush boundary,
// not at every instant. The driver pushes whole sectors to the card as its
// buffer fills, so a power cut can still truncate the tail mid-tag. Our own
// GpxParse is a streaming parser and recovers every complete <trkpt> before
// the cut, which is the case that matters.
bool WriteFooter() {
    char footer[GPX_WRITE_MAX_LINE];
    const size_t len = GpxWrite_Footer(footer, sizeof(footer));
    if (len == 0) {
        return false;
    }

    if (!s_file.seek(s_body_end)) {
        return false;
    }
    if (s_file.write((const uint8_t *)footer, len) != len) {
        return false;
    }
    // Back to the body end, ready for the next point to overwrite the footer.
    return s_file.seek(s_body_end);
}

bool OpenFile(const RideLogPoint_t &first) {
    if (!GpxTrack_CardMounted()) {
        return false;
    }

    // mkdir on an existing directory is not an error worth acting on, so the
    // result is deliberately ignored; the open below is the real test.
    SD_MMC.mkdir("/rides");

    if (!ChooseFileName(first, s_name, sizeof(s_name))) {
        s_name[0] = '\0';
        return false;
    }

    s_file = SD_MMC.open(s_name, FILE_WRITE);
    if (!s_file) {
        s_name[0] = '\0';
        return false;
    }

    char header[512];
    const size_t len = GpxWrite_Header(header, sizeof(header), "Pegasus ride");
    if (len == 0 || s_file.write((const uint8_t *)header, len) != len) {
        s_file.close();
        s_name[0] = '\0';
        return false;
    }

    s_body_end = s_file.position();
    s_last_flush_ms = millis();

    // Complete from the very first byte: if power is lost before any point
    // arrives, the card holds a valid, empty GPX rather than a stub.
    if (!WriteFooter()) {
        s_file.close();
        s_name[0] = '\0';
        return false;
    }
    s_file.flush();
    return true;
}

bool AppendPoint(const RideLogPoint_t &point) {
    char line[GPX_WRITE_MAX_LINE];
    const size_t len = GpxWrite_Point(line, sizeof(line), point.lat, point.lon, point.alt,
                                      point.has_time, point.year, point.month, point.day,
                                      point.hour, point.minute, point.second);
    if (len == 0) {
        return false;
    }

    if (!s_file.seek(s_body_end)) {
        return false;
    }
    if (s_file.write((const uint8_t *)line, len) != len) {
        return false;
    }

    s_body_end = s_file.position();
    s_points++;

    // Close the document again straight away: the file must never sit without
    // its footer, since a flush can happen between points.
    return WriteFooter();
}

void WriterTask(void *pv) {
    (void)pv;

    for (;;) {
        RideLogPoint_t point;

        // Waking on the flush interval rather than blocking forever means a
        // ride that ends mid-interval still gets its last points committed.
        if (xQueueReceive(s_queue, &point, pdMS_TO_TICKS(FLUSH_INTERVAL_MS)) == pdTRUE) {
            if (!s_failed && !s_recording) {
                if (!OpenFile(point)) {
                    // Giving up for this power-on rather than retrying. A card
                    // that would not take the file is a steady state, not a
                    // transient, and retrying per point would spend the ride
                    // re-attempting the same failed open.
                    s_failed = true;
                    continue;
                }
                s_recording = true;
            }

            if (s_recording && !AppendPoint(point)) {
                // The card went away mid-ride. Close what exists -- valid GPX
                // up to the last flush -- and stop trying.
                s_file.close();
                s_recording = false;
                s_failed = true;
                continue;
            }
        }

        // Rate-limited because this is what actually costs card time; the
        // footer is already in place after every point, so whatever this
        // commits is a complete document.
        if (s_recording && (millis() - s_last_flush_ms) >= FLUSH_INTERVAL_MS) {
            s_file.flush();
            s_last_flush_ms = millis();
        }
    }
}

void OnGpsPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)user_arg;

    if (s_queue == nullptr || data == nullptr || size != sizeof(GPS_Info_t)) {
        return;
    }
    const GPS_Info_t *gps = (const GPS_Info_t *)data;
    if (!gps->fix_valid) {
        return;
    }

    const uint32_t now = millis();
    if (s_has_last) {
        const double moved =
            TripAccum_DistanceMetres(s_last_lat, s_last_lon, gps->lat, gps->lon);
        if (moved < MIN_POINT_SPACING_M && (now - s_last_queued_ms) < MAX_POINT_INTERVAL_MS) {
            return;
        }
    }

    RideLogPoint_t point;
    point.lat = gps->lat;
    point.lon = gps->lon;
    point.alt = gps->alt;
    point.has_time = gps->time_valid;
    point.year = gps->year;
    point.month = gps->month;
    point.day = gps->day;
    point.hour = gps->hour;
    point.minute = gps->minute;
    point.second = gps->second;

    // Never block the GPS task on a full queue: dropping a breadcrumb is a
    // cosmetic loss, stalling the publish path is not.
    if (xQueueSend(s_queue, &point, 0) != pdTRUE) {
        return;
    }

    s_last_lat = gps->lat;
    s_last_lon = gps->lon;
    s_has_last = true;
    s_last_queued_ms = now;
}

Account s_gps_account("RideLog/GPS", OnGpsPublished);

} // namespace

void RideLog_Init() {
    // No card, no logging, and no task or subscription either. Hot-plug is not
    // supported anywhere in this firmware -- GpxTrack mounts once at boot --
    // so a card absent now will be absent for the whole ride.
    if (!GpxTrack_CardMounted()) {
        return;
    }

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(RideLogPoint_t));
    if (s_queue == nullptr) {
        return;
    }

    // Core 0 with the other background work, at a priority below the GPS
    // reader: a slow card must never delay parsing the fixes themselves.
    xTaskCreatePinnedToCore(WriterTask, "ridelog", 4096, nullptr, 1, nullptr, 0);

    DataCenter_Subscribe(TOPIC_GPS_INFO, &s_gps_account);
}

bool RideLog_IsRecording() {
    return s_recording;
}

const char *RideLog_FileName() {
    return s_name;
}

uint32_t RideLog_PointCount() {
    return s_points;
}

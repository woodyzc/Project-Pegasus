#include "RideLog.h"

#include <Arduino.h>
#include <stdarg.h>
#include <SD_MMC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "../system/DataCenter.h"
#include "../system/TripAccum.h"
#include "GpxParse.h"
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
    uint8_t bpm; // 0 when no recent reading

    // Not a trackpoint but a request to run the self-test. It travels through
    // the same queue so that pressing the button wakes the writer task
    // immediately: the task otherwise sits in a ten-second receive, and a
    // plain flag would leave the panel looking frozen for that long.
    bool selftest;
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

bool OpenFile(const RideLogPoint_t &first, const char *track_name) {
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
    const size_t len = GpxWrite_Header(header, sizeof(header), track_name);
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
                                      point.hour, point.minute, point.second, point.bpm);
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

// ---------------------------------------------------------------------------
// Self-test (RideLog.h explains why it exists)
// ---------------------------------------------------------------------------

volatile RideLogSelfTest_t s_selftest = RIDELOG_SELFTEST_IDLE;

// One byte longer than anything written into it, and that last byte is never
// touched. The writer task fills this and the LVGL task reads it without a
// lock, so a long message replacing a short one passes through an instant with
// the old terminator overwritten and the new one not yet placed. A reader
// caught there would run off the end of a plain 128-byte array; with the guard
// byte it stops at the array's own edge, having read a garbled line for one
// frame. That is the right trade for a diagnostic string.
constexpr size_t SELFTEST_MSG_MAX = 128;
char s_selftest_msg[SELFTEST_MSG_MAX + 1] = "";

// Three points: enough to prove that appending seeks back over the previous
// footer rather than appending after it, which one point would not show.
constexpr int SELFTEST_POINTS = 3;

// Two of the three carry a reading, so the file exercises both branches of the
// trkpt writer -- and reading back exactly two proves the third was omitted
// rather than written as zero.
constexpr uint8_t SELFTEST_BPM[SELFTEST_POINTS] = {0, 142, 151};

void SelfTestFinish(RideLogSelfTest_t state, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_selftest_msg, SELFTEST_MSG_MAX, fmt, args);
    va_end(args);
    // The message is published before the state, so a reader that sees PASS
    // never reads the previous run's text alongside it.
    s_selftest = state;
}

// Returns the recorder to the state it was in before the test, so the first
// real fix opens its own file rather than appending to this one. s_failed is
// deliberately not touched: a self-test that could not write is a reason to
// tell the rider, not a reason to disable logging for the power-on.
void SelfTestReset() {
    if (s_file) {
        s_file.close();
    }
    s_recording = false;
    s_name[0] = '\0';
    s_points = 0;
    s_body_end = 0;
    s_has_last = false;
}

// Reads the file back through GpxParse -- the same parser that loads routes
// off this card. Counting points here rather than trusting the byte count is
// the whole value of the exercise: it is what catches a card that accepts
// every write and returns something else.
bool SelfTestVerify(const char *name, size_t *out_points, size_t *out_rates, bool *out_closed) {
    File f = SD_MMC.open(name, FILE_READ);
    if (!f) {
        return false;
    }

    GpxParser_t parser;
    Gpx_Init(&parser);

    static const char NEEDLE[] = "<gpxtpx:hr>";
    size_t matched = 0;

    *out_points = 0;
    *out_rates = 0;

    // 128 bytes at a time. The writer task's stack is the constraint, and the
    // file is about a kilobyte, so nothing here is worth a bigger buffer.
    char chunk[128];
    int read_len;
    while ((read_len = f.read((uint8_t *)chunk, sizeof(chunk))) > 0) {
        for (int i = 0; i < read_len; i++) {
            double lat;
            double lon;
            if (Gpx_Feed(&parser, chunk[i], &lat, &lon)) {
                (*out_points)++;
            }

            // Substring search carried across chunk boundaries.
            if (chunk[i] == NEEDLE[matched]) {
                matched++;
                if (NEEDLE[matched] == '\0') {
                    (*out_rates)++;
                    matched = 0;
                }
            } else {
                matched = (chunk[i] == NEEDLE[0]) ? 1 : 0;
            }
        }
    }

    // The footer has to be the last thing in the file, not merely present:
    // the seek-back scheme is precisely what could leave a stale copy of it
    // buried in the middle.
    const char CLOSING[] = "</gpx>\n";
    const size_t tail_len = sizeof(CLOSING) - 1;
    char tail[sizeof(CLOSING)] = {0};
    const size_t size = f.size();
    *out_closed = false;
    if (size >= tail_len && f.seek(size - tail_len)) {
        if (f.read((uint8_t *)tail, tail_len) == (int)tail_len) {
            *out_closed = (memcmp(tail, CLOSING, tail_len) == 0);
        }
    }

    f.close();
    return true;
}

void RunSelfTest() {
    RideLogPoint_t point;
    memset(&point, 0, sizeof(point));
    // Germantown, to match the road extract already on this card, so the file
    // draws over something if it is ever loaded as a route.
    point.lat = 39.1834;
    point.lon = -77.2617;
    point.alt = 120.0f;
    // A fixed date rather than the current clock: the name is then stable, so
    // running the test twice overwrites one file instead of filling /rides,
    // and 2000 is obviously not a ride anyone took.
    point.has_time = true;
    point.year = 2000;
    point.month = 1;
    point.day = 1;

    if (!OpenFile(point, "Pegasus self-test")) {
        SelfTestFinish(RIDELOG_SELFTEST_FAIL, "could not create the file in /rides");
        SelfTestReset();
        return;
    }

    // Copied because SelfTestReset clears s_name before the message is built.
    char name[sizeof(s_name)];
    snprintf(name, sizeof(name), "%s", s_name);

    for (int i = 0; i < SELFTEST_POINTS; i++) {
        point.second = (uint8_t)i;
        point.lat += 0.0002;
        point.bpm = SELFTEST_BPM[i];
        if (!AppendPoint(point)) {
            SelfTestFinish(RIDELOG_SELFTEST_FAIL, "%s: write failed after %d of %d points", name,
                           i, SELFTEST_POINTS);
            SelfTestReset();
            return;
        }
    }

    s_file.flush();
    const uint32_t bytes = (uint32_t)s_file.size();
    s_file.close();

    size_t points = 0;
    size_t rates = 0;
    bool closed = false;
    if (!SelfTestVerify(name, &points, &rates, &closed)) {
        SelfTestFinish(RIDELOG_SELFTEST_FAIL, "%s: written, but will not reopen for reading", name);
        SelfTestReset();
        return;
    }

    SelfTestReset();

    if (points != SELFTEST_POINTS || rates != 2 || !closed) {
        SelfTestFinish(RIDELOG_SELFTEST_FAIL, "%s: read back %u/%d points, %u/2 rates, %s",
                       name, (unsigned)points, SELFTEST_POINTS, (unsigned)rates,
                       closed ? "closed" : "no closing tag");
        return;
    }

    SelfTestFinish(RIDELOG_SELFTEST_PASS, "%s: %d points, 2 heart rates, %u bytes, read back OK",
                   name, SELFTEST_POINTS, (unsigned)bytes);
}

void WriterTask(void *pv) {
    (void)pv;

    for (;;) {
        RideLogPoint_t point;

        // Waking on the flush interval rather than blocking forever means a
        // ride that ends mid-interval still gets its last points committed.
        if (xQueueReceive(s_queue, &point, pdMS_TO_TICKS(FLUSH_INTERVAL_MS)) == pdTRUE) {
            if (point.selftest) {
                RunSelfTest();
                continue;
            }

            if (!s_failed && !s_recording) {
                if (!OpenFile(point, "Pegasus ride")) {
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

// The last heart rate, and when it arrived.
//
// Taken from the bus rather than passed in, because the two publishers are
// independent: the strap reports on its own schedule and the receiver on its
// own, and neither waits for the other. The reading is attached to whichever
// trackpoint is queued next.
volatile uint8_t s_last_bpm = 0;
volatile uint32_t s_last_bpm_ms = 0;

// How old a reading may be and still be written against a fix.
//
// A strap that has dropped out must not have its last reading stamped on the
// rest of the ride: that is a fabricated heart rate, and it looks exactly like
// a real one to anything reading the file afterwards. Ten seconds is several
// beats of tolerance for a link that stutters, and far short of a dropout.
constexpr uint32_t BPM_MAX_AGE_MS = 10000;

void OnHeartRatePublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)user_arg;

    if (data == nullptr || size != sizeof(HeartRate_t)) {
        return;
    }
    const HeartRate_t *hr = (const HeartRate_t *)data;
    s_last_bpm = hr->bpm;
    s_last_bpm_ms = millis();
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

    // A strap that has dropped out must not stamp its last reading onto every
    // later point: past BPM_MAX_AGE_MS the sample is simply absent.
    point.bpm = ((now - s_last_bpm_ms) < BPM_MAX_AGE_MS) ? s_last_bpm : 0;
    point.selftest = false;

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
Account s_hr_account("RideLog/HR", OnHeartRatePublished);

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
    // 6KB, not 4KB: the self-test nests a 512-byte header buffer, a 320-byte
    // line buffer, a GPX parser and a read buffer inside the same task, under
    // whatever the SD driver uses.
    xTaskCreatePinnedToCore(WriterTask, "ridelog", 6144, nullptr, 1, nullptr, 0);

    DataCenter_Subscribe(TOPIC_GPS_INFO, &s_gps_account);
    DataCenter_Subscribe(TOPIC_HEART_RATE, &s_hr_account);
}

bool RideLog_SelfTestStart() {
    // No queue means no card was present at boot, and hot-plug is not
    // supported anywhere in this firmware.
    if (s_queue == nullptr) {
        SelfTestFinish(RIDELOG_SELFTEST_FAIL, "no SD card was mounted at boot");
        return false;
    }
    // The test drives the recorder's own file handle and counters, so running
    // it over a live ride would close that ride's file and lose the rest of it.
    if (s_recording) {
        SelfTestFinish(RIDELOG_SELFTEST_FAIL, "a ride is being recorded -- not while it is");
        return false;
    }
    if (s_selftest == RIDELOG_SELFTEST_RUNNING) {
        return false;
    }

    RideLogPoint_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.selftest = true;
    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        SelfTestFinish(RIDELOG_SELFTEST_FAIL, "the writer is busy -- try again");
        return false;
    }

    // Set here rather than in the writer task so the panel says something the
    // moment the button is released, even if the card takes a second.
    SelfTestFinish(RIDELOG_SELFTEST_RUNNING, "writing to the card...");
    return true;
}

RideLogSelfTest_t RideLog_SelfTestState() {
    return s_selftest;
}

const char *RideLog_SelfTestMessage() {
    return s_selftest_msg;
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

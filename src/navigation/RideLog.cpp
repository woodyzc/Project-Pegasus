#include "RideLog.h"

#include <Arduino.h>
#include <stdarg.h>
#include <Preferences.h>
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

// ---- Ending a ride nobody ended ----
//
// The safety net under the rider's own "finish ride". Recording only ever
// starts by hand now, but nothing makes them stop it, and a device left
// recording holds deep sleep off for as long as it lasts.
//
// This long with no movement and the ride is over: the file is closed and
// recording is DISARMED, so moving again does not quietly start a new one.
// That is the whole reason it closes the hole -- a device in a bag stays
// silent through the drive home and the next morning's train whether or not
// deep sleep is available, which matters because deep sleep ships switched
// off and its wake source has never been proven.
//
// An hour, not the fifteen minutes this started at. The number only has to
// cover "the rider forgot", because "the rider is done" is a button now, and
// the cost of being wrong is asymmetric: too short silently stops recording
// on a rider who is still out, and the twenty extra minutes of running cost
// about 17mAh. Long lunches, punctures and waiting for company all fit under
// an hour; almost nothing that is genuinely still a ride does not.
//
// The residual risk is a stop longer than this -- the rider rides on and the
// rest is not written. That is what the dashboard's REC indicator and the
// "not recording" nudge exist to catch (Page_Dashboard).
constexpr uint32_t AUTO_END_AFTER_MS = 60u * 60u * 1000u;

// ---- Carrying the armed flag across a restart ----
//
// Without this a reboot mid-ride silently stopped recording, and the only way
// to resume -- pressing "new ride" -- reset the odometer with it. Trip is
// persisted in NVS precisely so a restart does not lose the distance, and
// throwing it away was the one path back. So the flag is persisted too, and
// the ride simply continues.
//
// The track does NOT continue into the same file: appending would mean reading
// the GPX back and stripping its footer, on a card, at boot. Two files for one
// ride is an annoyance a laptop fixes in a minute; a corrupted ride is not.
constexpr char NVS_NAMESPACE[] = "pegasus";
constexpr char KEY_ARMED[] = "rl_armed";

// How long RideLog_Shutdown() waits for the writer task to close the file.
// The task is blocked on the queue the command arrives through, so this is
// almost never spent; it covers a write already in flight to a slow card.
constexpr uint32_t SHUTDOWN_CLOSE_MS = 1000;

void PersistArmed(bool armed) {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, false)) {
        prefs.putUChar(KEY_ARMED, armed ? 1 : 0);
        prefs.end();
    }
}

constexpr float AUTO_END_MOVING_MPS = 1.0f;

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

    // Not a trackpoint at all but a request to the writer task. Commands
    // travel through the same queue as points so that pressing a button wakes
    // the task immediately -- it otherwise sits in a ten-second receive, and a
    // plain flag would leave the panel looking frozen for that long. Sending
    // them in-band also means a command cannot overtake points already queued
    // ahead of it, which matters for ending a ride: the last few breadcrumbs
    // belong in the file being closed, not the one after it.
    uint8_t command; // RideLogCommand_t
} RideLogPoint_t;

typedef enum {
    RIDELOG_CMD_NONE = 0, // an ordinary trackpoint
    RIDELOG_CMD_NEW_RIDE,
    RIDELOG_CMD_FINISH,
    RIDELOG_CMD_SHUTDOWN,
} RideLogCommand_t;

QueueHandle_t s_queue = nullptr;
File s_file;
// Read from the UI core via RideLog_IsRecording(), written by the writer task.
volatile bool s_recording = false;

// The last time the rider was actually moving. Written from the GPS task in
// OnGpsPublished, read by the writer task -- volatile for the same reason
// s_last_bpm is, and a 32-bit aligned load is atomic on this core.
//
// A lost fix advances this timer rather than freezing it, and that is the
// case that matters most: OnGpsPublished returns early when fix_valid is
// false, so a device in a bag stops updating this and the ride ends on its
// own. That is exactly the scenario this whole mechanism exists for.
volatile uint32_t s_last_motion_ms = 0;

// ---- Recording is a deliberate act ----
//
// False at boot, and nothing but the rider's own "start ride" sets it true.
// Before this, the first valid fix opened a file, which meant the device
// recorded the drive to the trailhead, the walk from the car, and -- because
// nothing ever stopped it -- the train the following morning, all as rides.
//
// Owned by the writer task exactly like s_recording. The UI never writes it:
// RideLog_StartNewRide() and RideLog_FinishRide() post commands into the same
// queue the points travel through, so a press and a trackpoint already in
// flight resolve in the order they were issued. Writing it directly from the
// LVGL thread would break the one ordering guarantee this file has.
//
// volatile for the same reason s_recording is: the dashboard reads it through
// RideLog_IsArmed() once a second inside a loop that does not otherwise touch
// this translation unit, and a compiler is entitled to hoist that read out
// and never look again.
volatile bool s_armed = false;

// True once a file has been opened this boot, and never cleared afterwards.
//
// Distinguishes "the rider rode and finished" from "nothing has happened since
// power-on", which look identical from s_armed alone and mean opposite things
// to PowerManager: the first is a rider saying they are done, the second is a
// device that has been told nothing and should not vanish on someone who has
// not started yet.
volatile bool s_has_recorded = false;

// ---- Closing the file before the chip restarts ----
//
// The file belongs to the writer task, so the shutdown handler must not touch
// it directly -- that is the same race that let NimBLE be deinitialised under
// its own supervisor. The command goes through the queue instead, which also
// buys the ordering for free: every point already queued is written before the
// close, rather than being lost to it.
volatile bool s_shutdown_done = false;

// How many rides have been closed by the timeout since boot. On the panel,
// because an auto-end is otherwise silent -- the rider would find out at the
// end of the day that their ride is in two files, with nothing saying why.
uint32_t s_auto_end_count = 0;
// Set once the card has refused us, so the ride is not spent retrying.
bool s_failed = false;
char s_name[48] = "";
volatile uint32_t s_points = 0;

// Files thrown away for holding fewer than MIN_KEPT_POINTS. On the panel,
// because a silent delete and a failed write look identical from outside, and
// only one of them is fine.
uint32_t s_discarded_count = 0;

// Where the track points end and the footer begins. Every append seeks here
// first, overwriting the previous footer.
uint32_t s_body_end = 0;
uint32_t s_last_flush_ms = 0;

// Filter state. Written by the GPS publish callback, which is a single task,
// so it needs no lock for its own use.
//
// The one exception is CloseRide() on the writer task, which clears s_has_last
// so the first fix after a ride boundary is always recorded rather than being
// dropped for being close to the last point of the previous ride. That is a
// genuine cross-task write, and it is safe only because of what it costs when
// it races: a bool store is atomic on this core, and if the GPS callback sets
// it back to true immediately afterwards the worst outcome is that the new
// file's first point waits for the ordinary five metres. Nothing is corrupted
// and no point is written to the wrong file, because the command and the
// points share one queue and therefore one order.
double s_last_lat = 0.0;
double s_last_lon = 0.0;
volatile bool s_has_last = false;
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

// A track needs two points. One is a place, not a journey -- there is nothing
// to draw, no distance, no duration, and nothing any tool will plot.
//
// Real files, not hypothetical ones: a session spent pressing "new ride" to
// test the buttons left fourteen of these on the card, each a valid GPX
// containing a single position, and the rider then has to tell them apart from
// the ride they actually rode. Kept at 2 rather than something larger because
// the question being answered is "is this a track at all", not "was this ride
// worth keeping" -- a rider who starts, rolls ten metres and stops has a short
// ride, and that is theirs to delete.
constexpr uint32_t MIN_KEPT_POINTS = 2;

void CloseRide() {
    // Captured before the close: both are cleared below, and the decision to
    // delete needs them.
    const bool too_short = s_file && s_points < MIN_KEPT_POINTS;
    char doomed[sizeof(s_name)];
    doomed[0] = '\0';
    if (too_short) {
        strncpy(doomed, s_name, sizeof(doomed) - 1);
        doomed[sizeof(doomed) - 1] = '\0';
    }

    if (s_file) {
        s_file.flush();
        s_file.close();
    }

    // After the close, never before: removing a file that is still open is
    // undefined on FAT and the handle would go on referring to a directory
    // entry that no longer exists.
    if (doomed[0] != '\0') {
        SD_MMC.remove(doomed);
        s_discarded_count++;
    }

    s_recording = false;
    s_name[0] = '\0';
    s_points = 0;
    s_body_end = 0;
    s_has_last = false;
}

// The rider says the ride starts here. The odometer and the averages are reset
// alongside this by the caller, since the three describe the same ride.
void StartNewRide() {
    CloseRide();

    // The only thing that ever arms recording. Nothing else -- not a fix, not
    // movement -- puts the device back into a state where it writes to the
    // card. A reboot is the exception, and only because the flag is restored
    // from NVS rather than set afresh.
    s_armed = true;
    PersistArmed(true);

    // The hour starts here, not at the first fix. Otherwise an armed device
    // that never sees a satellite has no clock running at all, and with the
    // flag now surviving restarts it would stay armed for ever -- holding deep
    // sleep off with it.
    s_last_motion_ms = millis();

    // A deliberate gesture is the one moment a card that refused us earlier is
    // worth another attempt. Without this, one failed open early in the day
    // would leave the rider unable to record anything again until they power
    // cycle, and nothing on the panel would explain why.
    s_failed = false;
}

// The rider says the ride is over. Closes the file and, unlike every other
// path that closes one, leaves recording disarmed: movement will not restart
// it, so the drive home and the next morning's commute are not written.
//
// Deliberately does NOT clear s_failed. A card that refused the last open is
// still refusing; the gesture that deserves a retry is starting a ride, not
// ending one.
void FinishRide() {
    CloseRide();
    s_armed = false;
    PersistArmed(false);
}

// Reads the file back through GpxParse -- the same parser that loads routes
// off this card. Counting points here rather than trusting the byte count is
// the whole value of the exercise: it is what catches a card that accepts
// every write and returns something else.
void WriterTask(void *pv) {
    (void)pv;

    for (;;) {
        RideLogPoint_t point;

        // Waking on the flush interval rather than blocking forever means a
        // ride that ends mid-interval still gets its last points committed.
        if (xQueueReceive(s_queue, &point, pdMS_TO_TICKS(FLUSH_INTERVAL_MS)) == pdTRUE) {
            if (point.command == RIDELOG_CMD_NEW_RIDE) {
                StartNewRide();
                continue;
            }
            if (point.command == RIDELOG_CMD_FINISH) {
                FinishRide();
                continue;
            }
            if (point.command == RIDELOG_CMD_SHUTDOWN) {
                // Close, but do NOT disarm. A restart is not the rider saying
                // they are done, and the armed flag is what carries the ride
                // across it.
                CloseRide();
                s_shutdown_done = true;
                for (;;) {
                    vTaskDelay(portMAX_DELAY);
                }
            }

            if (!s_failed && s_armed && !s_recording) {
                if (!OpenFile(point, "Pegasus ride")) {
                    // Giving up for this power-on rather than retrying. A card
                    // that would not take the file is a steady state, not a
                    // transient, and retrying per point would spend the ride
                    // re-attempting the same failed open.
                    s_failed = true;
                    continue;
                }
                s_recording = true;
                s_has_recorded = true;
                // Start the idle clock at the open, not at boot: a device that
                // sat indoors for twenty minutes waiting for a fix must not
                // end its ride on the first point it writes.
                s_last_motion_ms = millis();
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

        // ---- The ride nobody ended ----
        // Checked here because this task already wakes every FLUSH_INTERVAL_MS
        // whether or not a point arrived, so the timeout needs no timer of its
        // own -- and the no-fix case, which is the one that matters, produces
        // no points at all and would never be noticed by a check on the
        // receive path.
        //
        // Unsigned subtraction, so the 49-day millis() wrap gives a small
        // number rather than instantly ending the ride.
        // s_armed, not s_recording. A device armed in a car park that never
        // gets a fix opens no file, so a check on s_recording never fired --
        // survivable while a reboot cleared the flag, and eternal now that one
        // does not.
        if (s_armed && (millis() - s_last_motion_ms) >= AUTO_END_AFTER_MS) {
            // The same close the rider's own "new ride" gesture uses. It
            // flushes, closes, and clears the name and point count as well --
            // which matters, because the settings page reads those, and a
            // finished ride still showing a filename and a rising point count
            // is worse than no line at all.
            //
            // The footer is already written after every point, so what it
            // closes is a complete document.
            // The same close the rider's own "finish ride" takes, disarm and
            // all. Leaving it armed was the earlier design and it did not
            // work: movement simply opened a new file, so a forgotten device
            // produced a clean ride followed by a string of junk ones instead
            // of one long junk ride. Closing without disarming splits the
            // problem up; it does not solve it.
            FinishRide();
            s_auto_end_count++;

            // s_failed is deliberately NOT set: the card is fine, the rider
            // simply stopped. Pressing "start ride" must still work.
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

    // Before the spacing filter below, not after. That filter drops points the
    // rider has not travelled far enough to justify, which is a question about
    // the file -- whereas this is a question about whether they are riding at
    // all, and a slow crawl that never clears MIN_POINT_SPACING_M is still
    // riding.
    if (gps->speed >= AUTO_END_MOVING_MPS) {
        s_last_motion_ms = now;
    }
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
    point.command = RIDELOG_CMD_NONE;

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

    // Restored before the task starts, so the first point after a reboot is
    // written rather than dropped. s_has_recorded stays false on purpose: the
    // previous boot's file is closed and finished, and what matters here is
    // only whether a new one should be opened.
    {
        Preferences prefs;
        if (prefs.begin(NVS_NAMESPACE, false)) {
            s_armed = prefs.getUChar(KEY_ARMED, 0) != 0;
            prefs.end();
        }
    }
    // Whether it was restored or not, the hour starts now. A device that was
    // armed when the power went and then sat on a bench must still time out.
    s_last_motion_ms = millis();

    // Close the file before the chip restarts. Deep sleep already flushed and
    // unmounted through PowerManager; a restart did neither, so up to a
    // flush interval of track was lost every time -- and CLAUDE.md section 7
    // asks for exactly this.
    esp_register_shutdown_handler(RideLog_Shutdown);

    // Core 0 with the other background work, at a priority below the GPS
    // reader: a slow card must never delay parsing the fixes themselves.
    // Back to 4KB now the SD self-test is gone. It ran at 4KB for most of this
    // project's life with buffers 128 bytes smaller than today's; the 6KB was
    // only ever for the parser and read buffer the self-test nested in here.
    xTaskCreatePinnedToCore(WriterTask, "ridelog", 4096, nullptr, 1, nullptr, 0);

    DataCenter_Subscribe(TOPIC_GPS_INFO, &s_gps_account);
    DataCenter_Subscribe(TOPIC_HEART_RATE, &s_hr_account);
}

bool RideLog_StartNewRide() {
    if (s_queue == nullptr) {
        return false;
    }

    RideLogPoint_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.command = RIDELOG_CMD_NEW_RIDE;
    // Zero timeout, like every other send: the rule everywhere in this file is
    // that nothing blocks a caller on the card, and the LVGL thread least of
    // all. A full queue means the writer is badly behind, which the caller
    // reports rather than waits out.
    return xQueueSend(s_queue, &cmd, 0) == pdTRUE;
}

void RideLog_Shutdown() {
    if (s_queue == nullptr) {
        return;
    }

    RideLogPoint_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.command = RIDELOG_CMD_SHUTDOWN;
    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        // A full queue means the writer is far behind. Nothing useful is left
        // to try: blocking here would delay the restart for a card that is
        // already not keeping up.
        return;
    }

    // Bounded, like the BLE park. The writer sits blocked on this very queue,
    // so it normally answers within a tick; the wait exists for the case where
    // it is mid-write to a slow card.
    const uint32_t deadline = millis() + SHUTDOWN_CLOSE_MS;
    while (!s_shutdown_done && (int32_t)(millis() - deadline) < 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

bool RideLog_FinishRide() {
    if (s_queue == nullptr) {
        return false;
    }

    RideLogPoint_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.command = RIDELOG_CMD_FINISH;
    // Through the queue for the same reason the command above is: a trackpoint
    // published a millisecond before the press must land in the file before it
    // closes, not after.
    return xQueueSend(s_queue, &cmd, 0) == pdTRUE;
}

bool RideLog_IsArmed() {
    return s_armed;
}

bool RideLog_HasRecorded() {
    return s_has_recorded;
}

uint32_t RideLog_AutoEndCount() {
    return s_auto_end_count;
}

uint32_t RideLog_DiscardedCount() {
    return s_discarded_count;
}

bool RideLog_IsRecording() {
    return s_recording;
}

const char *RideLog_NotRecordingReason() {
    if (s_recording || !s_armed) {
        return nullptr; // writing, or not asked to
    }

    // Checked before s_failed, and it is the more useful answer even though
    // the failure flag will be set too once a fix has arrived: the card is
    // mounted exactly once, in setup(), so a card pushed in after the board
    // came up is invisible for the whole power-on no matter how healthy it
    // is. Nothing remounts it. That is the likely story behind almost every
    // occurrence of this state, and unlike a genuinely bad card it is fixed
    // in five seconds by the rider.
    if (!GpxTrack_CardMounted()) {
        return "no card was mounted at start-up - restart with it inserted";
    }
    if (s_failed) {
        return "the card refused the ride file";
    }
    return nullptr; // armed, card fine, genuinely waiting for a first fix
}

const char *RideLog_FileName() {
    return s_name;
}

uint32_t RideLog_PointCount() {
    return s_points;
}

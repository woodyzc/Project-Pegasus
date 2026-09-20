#include "BLE_HR_Client.h"

#include "../navigation/BLE_TBT_Receiver.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_system.h>

#include "../system/DataCenter.h"
#include "BLE_CSC_Client.h"
#include "BleHrParse.h"
#include "BleRadioGate.h"

namespace {

// Standard GATT assigned numbers.
const NimBLEUUID kHrService((uint16_t)0x180D);      // Heart Rate Service
const NimBLEUUID kHrMeasurement((uint16_t)0x2A37);  // Heart Rate Measurement

constexpr uint32_t kDiscoveryScanMs = 15000;
constexpr uint32_t kBackoffStartMs = 1000;   // 1s, then 2s, 4s, 8s ...
constexpr uint32_t kBackoffMaxMs = 32000;    // ... capped, so a strap left at home
                                             // doesn't retry forever at 1Hz
constexpr uint32_t kSuperviseIdleMs = 1000;

// Gap between discovery scans while no peer is known. Deliberately short and
// NOT the exponential backoff used for reconnects: scanning is the only way to
// find a peer, and a peer that is not advertising yet -- a watch that has not
// noticed the head unit rebooted, say -- starts advertising at a moment we
// cannot predict. Backing off to 32s meant being deaf for most of the window
// in which it became findable.
constexpr uint32_t kRescanGapMs = 2000;

// Scan duty cycle: listen 30ms out of every 100ms.
//
// One radio serves both roles here. In turn-by-turn mode this client scans
// for a heart-rate peer while BLE_TBT_Receiver advertises a GATT server for
// the phone, and discovery retries with only a 2s gap -- so with no watch
// broadcasting the scan runs almost continuously. At NimBLE's default, where
// the window fills the interval, that leaves little radio time for
// advertising and the phone struggles to find or hold the head unit.
//
// 30% still finds a peer quickly: advertisers repeat every few tens of
// milliseconds to a second, so a 15s scan gets many chances at each.
constexpr uint16_t kScanIntervalMs = 100;
constexpr uint16_t kScanWindowMs = 30;

// How long to wait for a clean disconnect at shutdown. Long enough to cover
// several connection intervals at the slow end of what a watch negotiates.
constexpr uint32_t kShutdownDisconnectMs = 1500;

// How long the shutdown waits for the supervisor task to park itself before
// tearing the stack down anyway.
//
// The task normally sits in vTaskDelay and parks within a tick, so this is
// almost never spent. It is sized for the case it exists to survive: the task
// inside a connect attempt, which can hold it for kConnectTimeoutMs. Waiting
// the full 5s would make the Restart button feel broken, so this stops short
// and proceeds -- which is no worse than the behaviour it replaces, where the
// stack was torn down under the task every single time.
constexpr uint32_t kShutdownParkMs = 2500;

// Direct connects to the stored address to tolerate before throwing the
// address away and scanning again. A peer that has simply wandered out of
// range comes back at the same address, so a rescan on the first failure
// would be wasteful; one that re-advertises under a new address never comes
// back at all without this.
constexpr int kFailuresBeforeRescan = 3;

// Silence after which the link is *reported* as not delivering data. Purely a
// display threshold -- it no longer drops the link, see BleHrTask().
constexpr uint32_t kDataStaleMs = 5000;

// The first measurement after subscribing gets longer before the status line
// complains, since a peer can take a few seconds to produce its first reading.
constexpr uint32_t kFirstNotifyGraceMs = 15000;

// A connect to an address that is no longer advertising has to time out before
// the attempt returns. NimBLE's default is long enough that the retry loop
// spends most of its life blocked inside a single doomed attempt.
constexpr uint32_t kConnectTimeoutMs = 5000;

constexpr uint32_t kTaskStackSize = 4096;
constexpr UBaseType_t kTaskPriority = 3;
constexpr BaseType_t kTaskCore = 0; // Core 0: Background Data Core (CLAUDE.md §4)

NimBLEClient *s_client = nullptr;
NimBLEAddress s_peer_address;
bool s_have_peer = false;
volatile bool s_connected = false;

// When the last 0x2A37 notification arrived. Seeded at connect time so a peer
// that subscribes and then says nothing is caught by the same timeout.
volatile uint32_t s_last_notify_ms = 0;

// Cleared at connect: false until the peer has actually delivered something,
// which is what selects the longer grace period above.
volatile bool s_had_notify = false;

// Gap between the last two notifications. Measured rather than assumed, so
// the timeout above can be judged against what the peer really does.
volatile uint32_t s_notify_interval_ms = 0;

// What the last discovery scan saw. The difference between "the watch is not
// advertising at all" and "it is advertising but we cannot connect" needs
// completely different fixes, and with serial unusable on this board
// (CLAUDE.md §8) the status row is the only place that distinction can show.
volatile int s_last_scan_devices = -1;
volatile int s_last_scan_hr_peers = 0;
volatile int s_failed_connects = 0;

// What the last shutdown managed, written to NVS so it survives the reboot it
// describes. Two attempts at fixing "restart while connected" have now failed,
// both resting on an unverified belief that the goodbye reached the watch.
// This records whether it did, so the next step is chosen from evidence.
//   0 = no record   1 = nothing was connected   2 = disconnected cleanly
//   3 = timed out waiting
constexpr char kNvsNamespace[] = "pegasus";
constexpr char kNvsShutdownCode[] = "ble_sd_code";
constexpr char kNvsShutdownMs[] = "ble_sd_ms";
// The park result has to be persisted for the same reason the shutdown result
// is, and it is not an optimisation: parking happens at shutdown, which is the
// instant before a reboot, so a value kept only in RAM is wiped by the very
// event it describes. Left as a boot-local static it could never read anything
// but "not run" -- which is exactly what it did, on the first board it was
// asked.
constexpr char kNvsParkOk[] = "ble_pk_ok";
constexpr char kNvsParkMs[] = "ble_pk_ms";
int s_last_shutdown_code = 0;
uint32_t s_last_shutdown_ms = 0;

void RecordShutdown(int code, uint32_t elapsed_ms) {
    Preferences prefs;
    if (!prefs.begin(kNvsNamespace, false)) {
        return;
    }
    prefs.putInt(kNvsShutdownCode, code);
    prefs.putUInt(kNvsShutdownMs, elapsed_ms);
    prefs.end();
}

// Written as soon as the park finishes rather than at the end of the shutdown,
// because the paths out of BLE_HR_Shutdown() are not all the same and this
// must survive every one of them.
void RecordPark(bool ok, uint32_t elapsed_ms) {
    Preferences prefs;
    if (!prefs.begin(kNvsNamespace, false)) {
        return;
    }
    // 1 or 2 rather than a bool, so "never written" is distinguishable from
    // "written, and it timed out".
    prefs.putUChar(kNvsParkOk, ok ? 2 : 1);
    prefs.putUInt(kNvsParkMs, elapsed_ms);
    prefs.end();
}

// Set when the controller reports the link actually down. This, not
// isConnected(), is the completion signal: NimBLEClient::disconnect() only
// calls ble_gap_terminate() and sets m_connStatus = DISCONNECTING, while
// isConnected() tests m_connStatus == CONNECTED -- so it reads false the
// instant disconnect() returns, before a single packet has gone out. Waiting
// on it measured local bookkeeping and reported "clean in 0ms" while the
// terminate never reached the watch.
volatile bool s_disconnect_event = false;

// ---- Stopping the supervisor before the stack goes ----
//
// BleHrTask runs forever and calls into NimBLE on every pass. BLE_HR_Shutdown
// used to call NimBLEDevice::deinit(true) straight into that, which deletes
// every client and server while the task still holds pointers to them -- and
// the window was not a narrow one, because the disconnect immediately above
// the deinit is exactly what wakes the task up to try reconnecting.
//
// Cooperative rather than vTaskDelete: the task may be inside NimBLE holding
// its mutex, and deleting it there would leave the stack locked forever.
volatile bool s_shutdown_requested = false;
volatile bool s_task_parked = false;

// How long the task took to park, and whether it did. On the settings page
// beside the disconnect result, because a park that times out means the stack
// was torn down under a live task after all -- the exact thing this exists to
// prevent, and otherwise completely invisible.
uint32_t s_last_park_ms = 0;
bool s_last_park_ok = false;
// 0 = never written, 1 = timed out, 2 = parked. Restored from NVS at init.
uint8_t s_last_park_code = 0;

class ClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient *client, int reason) override {
        (void)client;
        s_connected = false;
        s_disconnect_event = true;
        Serial.printf("[BLE_HR] disconnected (reason %d)\n", reason);
    }
};
ClientCallbacks s_client_callbacks;

// Scans for a peer advertising 0x180D and remembers its address. Only needed
// once: later reconnects dial s_peer_address directly.
bool DiscoverPeer() {
    Serial.println("[BLE_HR] scanning for a 0x180D peripheral...");
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    NimBLEScanResults results = scan->getResults(kDiscoveryScanMs, false);

    s_last_scan_devices = results.getCount();
    s_last_scan_hr_peers = 0;
    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice *device = results.getDevice(i);
        if (device != nullptr && device->isAdvertisingService(kHrService)) {
            s_last_scan_hr_peers++;
        }
    }

    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice *device = results.getDevice(i);
        if (device != nullptr && device->isAdvertisingService(kHrService)) {
            s_peer_address = device->getAddress();
            s_have_peer = true;
            Serial.printf("[BLE_HR] found %s '%s' rssi %d\n", s_peer_address.toString().c_str(),
                          device->getName().c_str(), device->getRSSI());
            return true;
        }
    }
    Serial.println("[BLE_HR] no 0x180D peripheral found");
    return false;
}

// Connects to the remembered peer and subscribes to 0x2A37. No scan involved,
// which is what keeps a reconnect off the controller's scan path.
bool ConnectAndSubscribe() {
    if (!s_have_peer) {
        return false;
    }

    // Take the advertisement down for the duration of the attempt.
    //
    // Connecting asks the controller to stop a scan and initiate a link in
    // quick succession. Doing that while it is also advertising is what makes
    // an HCI command miss its ack deadline, and NimBLE answers a missed ack by
    // resetting its host -- whereupon its own uncancelled timer fires during
    // the re-sync and aborts the chip. See BLE_TBT_Receiver.h.
    //
    // The window is the connect timeout at worst (5s), and it closes on every
    // path out of this function.
    //
    // The gate is the same rule extended to the second client. Cadence has its
    // own supervisor on its own backoff, and two connects landing together is
    // the identical load with no advertisement involved -- see BleRadioGate.h.
    BleRadioGateHold gate;
    BLE_TBT_PauseAdvertising();
    struct ResumeAdvertising {
        ~ResumeAdvertising() { BLE_TBT_ResumeAdvertising(); }
    } resume_on_exit;

    if (s_client == nullptr) {
        s_client = NimBLEDevice::createClient();
        if (s_client == nullptr) {
            Serial.println("[BLE_HR] createClient failed");
            return false;
        }
        s_client->setClientCallbacks(&s_client_callbacks, false);
        s_client->setConnectTimeout(kConnectTimeoutMs);
    }

    // Dial the stored address rather than an advertised-device object, so no
    // scan results are required.
    if (!s_client->connect(s_peer_address)) {
        return false;
    }

    NimBLERemoteService *service = s_client->getService(kHrService);
    NimBLERemoteCharacteristic *measurement =
        (service != nullptr) ? service->getCharacteristic(kHrMeasurement) : nullptr;

    if (measurement == nullptr || !measurement->canNotify() ||
        !measurement->subscribe(true, OnNotifyCallback)) {
        Serial.println("[BLE_HR] 0x2A37 subscribe failed");
        s_client->disconnect();
        return false;
    }

    s_connected = true;
    s_last_notify_ms = millis();
    s_had_notify = false;
    Serial.printf("[BLE_HR] connected to %s, subscribed to 0x2A37\n",
                  s_peer_address.toString().c_str());
    return true;
}

// Tears the client down so the next attempt builds a fresh one.
//
// Reusing a client across a failed reconnect is how this gets stuck: the
// stack keeps per-connection state, and once an attempt to a peer that is no
// longer there has failed, further connect() calls on the same object can
// keep failing even after the peer returns.
void ResetClient() {
    if (s_client == nullptr) {
        return;
    }
    if (s_client->isConnected()) {
        s_client->disconnect();
    }
    NimBLEDevice::deleteClient(s_client);
    s_client = nullptr;
}

// Core 0 supervisor: keeps the link up with exponential backoff after the
// initial connect. The notifications themselves arrive on NimBLE's own host
// task, not here.
//
// Note this never starts a scan: initial discovery is done synchronously in
// BLE_HR_Start(), and every reconnect from here dials the stored address.
void BleHrTask(void *pvParameters) {
    (void)pvParameters;

    uint32_t backoff_ms = kBackoffStartMs;

    for (;;) {
        // Checked before anything touches NimBLE, so that once the flag is up
        // this task never enters the stack again. Parking forever rather than
        // deleting itself: the caller is about to restart or deep sleep, and a
        // task suspended in its own loop holds no NimBLE mutex.
        if (s_shutdown_requested) {
            s_task_parked = true;
            for (;;) {
                vTaskDelay(portMAX_DELAY);
            }
        }

        if (s_connected) {
            // A connected link is never dropped for going quiet.
            //
            // It was, briefly, and that was a regression: a watch app pauses
            // transmission when it is backgrounded or the screen dims, while
            // the link itself stays perfectly good and resumes on its own when
            // the watch wakes. Tearing it down turned a pause into a permanent
            // failure -- doubly so with a peer that does not re-advertise
            // after a disconnect, where there is then no way back at all.
            //
            // A link that is genuinely dead is not our problem to detect: the
            // controller's own supervision timeout reports that as a real
            // disconnect, which onDisconnect() handles. Silence alone is not
            // evidence of a dead link. The UI still says "connected, no data"
            // and the dashboard still clears the reading, so nothing pretends
            // a stale number is live.
            backoff_ms = kBackoffStartMs; // healthy link resets the backoff
            s_failed_connects = 0;
            vTaskDelay(pdMS_TO_TICKS(kSuperviseIdleMs));
            continue;
        }

        if (!s_have_peer) {
            // Keep looking. The boot-time scan is 15 seconds, and requiring
            // the peer to be broadcasting inside
            // that window meant a watch woken a moment late would never be
            // found however long it broadcast afterwards.
            bool found;
            {
                BleRadioGateHold gate;
                found = DiscoverPeer();
            }
            if (found && ConnectAndSubscribe()) {
                backoff_ms = kBackoffStartMs;
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(kRescanGapMs));
            continue;
        }

        if (ConnectAndSubscribe()) {
            s_failed_connects = 0;
            continue;
        }

        // A fresh client for every attempt: see ResetClient().
        ResetClient();
        s_failed_connects++;

        // The stored address can simply stop existing. A Galaxy Watch, like
        // most phones and watches, advertises under a resolvable private
        // address that rotates, so after a dropped link it often reappears as
        // a different address -- and dialling the old one then fails forever,
        // however long the peer keeps broadcasting. Observed exactly that on
        // the bench: the only recovery was stopping and restarting the
        // broadcast, which is not something a rider can do mid-ride.
        //
        // Dropping the address sends the loop back through discovery above,
        // which is also what tells us whether the peer is advertising at all.
        if (s_failed_connects >= kFailuresBeforeRescan) {
            Serial.println("[BLE_HR] stored address is not answering, rediscovering");
            s_have_peer = false;
            s_failed_connects = 0;
            backoff_ms = kBackoffStartMs;
            continue;
        }

        Serial.printf("[BLE_HR] connect failed, retrying in %lums\n", (unsigned long)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = (backoff_ms >= kBackoffMaxMs) ? kBackoffMaxMs : (backoff_ms * 2);
    }
}

} // namespace

void OnNotifyCallback(NimBLERemoteCharacteristic *characteristic, uint8_t *data, size_t length,
                      bool is_notify) {
    (void)characteristic;
    (void)is_notify;

    // Flags/width/length handling lives in BleHrParse.c so it can be covered
    // by the host tests (test/host/test_ble_hr_parse.c).
    uint16_t bpm16;
    if (!BLE_HR_ParseMeasurement(data, length, &bpm16)) {
        return;
    }

    // Stamped here, on a measurement that actually parsed: the supervisor
    // uses it to tell a live link from a silent one.
    {
        const uint32_t now = millis();
        if (s_had_notify) {
            s_notify_interval_ms = now - s_last_notify_ms;
        }
        s_last_notify_ms = now;
        s_had_notify = true;
    }

    HeartRate_t hr;
    hr.bpm = (uint8_t)bpm16;
    // The strap's own battery level lives in the separate Battery Service
    // (0x180F / 0x2A19), not in this characteristic, so it stays "unknown"
    // per DataCenter.h's convention. Reading 0x180F on connect would be a
    // small, self-contained follow-up.
    hr.battery = 0xFF;
    DataCenter_Publish(TOPIC_HEART_RATE, &hr);
}

void BLE_HR_Init() {
    // Read (and clear) what the previous shutdown managed, before anything
    // else can overwrite it.
    {
        Preferences prefs;
        if (prefs.begin(kNvsNamespace, false)) {
            s_last_shutdown_code = prefs.getInt(kNvsShutdownCode, 0);
            s_last_shutdown_ms = prefs.getUInt(kNvsShutdownMs, 0);
            s_last_park_code = prefs.getUChar(kNvsParkOk, 0);
            s_last_park_ms = prefs.getUInt(kNvsParkMs, 0);
            prefs.putInt(kNvsShutdownCode, 0);
            prefs.putUChar(kNvsParkOk, 0);
            prefs.end();
        }
    }

    NimBLEDevice::init("pegasus");

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(kScanIntervalMs);
    scan->setWindow(kScanWindowMs);
}

void BLE_HR_Start() {
    // Discovery runs synchronously, blocking for up to kDiscoveryScanMs. See
    // the header for why the UI task is started before this is called.
    if (DiscoverPeer()) {
        ConnectAndSubscribe();
    }

    // Disconnect cleanly on any esp_restart(), so the peer learns the link is
    // gone instead of holding it open and refusing to advertise.
    esp_register_shutdown_handler(BLE_HR_Shutdown);

    xTaskCreatePinnedToCore(BleHrTask, "BLE_HR", kTaskStackSize, nullptr, kTaskPriority, nullptr,
                            kTaskCore);
}

namespace {

// Asks BleHrTask to stop entering NimBLE, and waits a bounded time for it to
// say it has. Cuts a discovery scan short on the way, since that is the one
// thing that would otherwise hold the task for fifteen seconds.
void ParkSupervisor() {
    s_shutdown_requested = true;

    // A scan in progress blocks the task inside getResults() for the rest of
    // kDiscoveryScanMs. Stopping it is safe when none is running.
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan != nullptr) {
        scan->stop();
    }

    const uint32_t started = millis();
    while (!s_task_parked && (millis() - started) < kShutdownParkMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_last_park_ok = s_task_parked;
    s_last_park_ms = millis() - started;
    RecordPark(s_last_park_ok, s_last_park_ms);
}

// Deletes the stack and forgets every pointer into it.
//
// The nulling is not tidiness. deinit(true) destroys the client and the
// server, and both were left dangling -- s_client because only ResetClient()
// ever cleared it, s_server because nothing ever did. Anything that ran
// afterwards and checked them for null found a non-null corpse.
void ReleaseStack() {
    NimBLEDevice::deinit(true);
    s_client = nullptr;
    s_connected = false;
    s_have_peer = false;
    BLE_TBT_NoteStackReleased();
    vTaskDelay(pdMS_TO_TICKS(50));
}

} // namespace

void BLE_HR_Shutdown() {
    // Say goodbye before the chip restarts.
    //
    // A reboot otherwise leaves the peer believing the link is still up, and a
    // peripheral that thinks it is connected stops advertising and refuses new
    // connections. The head unit then comes back up and scans for something
    // that is deliberately not there, until the peer's own supervision timeout
    // finally expires -- which is what "restart while connected, then it can
    // never find the watch again" actually was.
    //
    // Registered as an ESP-IDF shutdown handler, so it covers esp_restart()
    // however it is reached, including the settings page's Restart button. It
    // cannot cover a power cut or the reset esptool asserts when flashing;
    // nothing running on the chip can.
    //
    // ⚠️ It is also not sufficient on its own, and the peer decides that.
    // Observed 2026-09-15 with a Galaxy Watch 8: a restart left the watch
    // unfindable, and the only cure was switching broadcasting off on the
    // watch and restarting again -- which is not something a rider can do at
    // the roadside, and not something a strap even offers.
    //
    // Expect this to be watch-only. A strap's supervision timeout is seconds,
    // so it works out the link is dead and starts advertising again by itself;
    // a watch is a whole operating system with a connection manager and
    // app-level state, and can sit on a phantom link far longer. Untested with
    // a strap -- see BLE_HR_Client.h for how to test it when one arrives.
    //
    // Before changing any timing here, read "Last disconnect on restart" on
    // the settings page: it is written to NVS below precisely so this question
    // can be answered rather than guessed at. "clean in NNNms" means the
    // goodbye reached the peer and the fault is the peer's; "TIMED OUT" means
    // kShutdownDisconnectMs is too short and this end is at fault.
    // ---- Stop BOTH supervisors before anything else ----
    // First, and before the disconnect below rather than after it, because the
    // disconnect is what would otherwise send the task straight into a
    // reconnect attempt on a stack that is about to be deleted.
    //
    // The cadence supervisor is parked here rather than by its own module
    // because this function owns the teardown: ReleaseStack() below runs
    // NimBLEDevice::deinit(true), which destroys every client on the stack.
    // A cadence task still holding a pointer to one is the exact bug §8
    // records for the heart-rate task, one sensor along.
    BLE_CSC_Park();
    ParkSupervisor();

    if (s_client == nullptr || !s_client->isConnected()) {
        RecordShutdown(1, 0);
        // Still tear the stack down, and still forget the pointers. The old
        // code returned here, which was survivable before a restart but leaves
        // the deep-sleep path inconsistent with the connected one for no
        // reason -- PrepareForSleep() comes through here too.
        ReleaseStack();
        return;
    }

    const uint32_t started = millis();
    s_disconnect_event = false;
    s_client->disconnect();

    // Wait for it to complete rather than guessing at a delay. disconnect()
    // only queues the request: the link-layer terminate cannot go out until
    // the next connection event, and a watch negotiates long connection
    // intervals to save power. A fixed 50ms was under that interval, so the
    // chip reset before the goodbye ever reached the air -- which left the
    // peer holding the link exactly as if nothing had been sent.
    // Wait for the controller's disconnect event, which only arrives once the
    // link really is down. A watch negotiates a long connection interval, so
    // this legitimately takes hundreds of milliseconds.
    const uint32_t deadline = millis() + kShutdownDisconnectMs;
    while (!s_disconnect_event && (int32_t)(millis() - deadline) < 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    RecordShutdown(s_disconnect_event ? 2 : 3, millis() - started);

    // Only now tear the stack down. Doing this before the event was the
    // second half of the bug: it stopped the controller while the terminate
    // was still queued, so the watch never learned the link was gone and
    // therefore never resumed advertising.
    ReleaseStack();
}

bool BLE_HR_IsConnected() {
    return s_connected;
}

const char *BLE_HR_LastShutdownText() {
    static char text[40];
    switch (s_last_shutdown_code) {
        case 1:  return "nothing connected";
        case 2:  snprintf(text, sizeof(text), "clean in %ums", (unsigned)s_last_shutdown_ms);
                 return text;
        case 3:  snprintf(text, sizeof(text), "TIMED OUT after %ums", (unsigned)s_last_shutdown_ms);
                 return text;
        default: return "not run";
    }
}

const char *BLE_HR_LastParkText() {
    static char text[32];
    if (s_last_park_code == 0) {
        return "not run";
    }
    snprintf(text, sizeof(text), "%s in %ums", s_last_park_code == 2 ? "parked" : "TIMED OUT",
             (unsigned)s_last_park_ms);
    return text;
}

const char *BLE_HR_StatusText() {
    if (s_connected) {
        // "connected" has to mean "heart rate is arriving". A link that is up
        // but silent is not a working source, and reporting it as connected
        // sends someone looking for the fault in the wrong place.
        const uint32_t allowed_ms = s_had_notify ? kDataStaleMs : kFirstNotifyGraceMs;
        if ((millis() - s_last_notify_ms) > allowed_ms) {
            return "connected, no data";
        }
        if (!s_had_notify) {
            return "connected, waiting";
        }
        // The measured interval, because the whole staleness scheme depends on
        // it and this is the only way to see what it actually is.
        static char text[32];
        snprintf(text, sizeof(text), "connected (%.1fs)", s_notify_interval_ms / 1000.0);
        return text;
    }
    static char text[48];

    if (s_have_peer) {
        snprintf(text, sizeof(text), "reconnecting (%d fails)", s_failed_connects);
        return text;
    }
    if (s_last_scan_devices < 0) {
        return "searching (first scan)";
    }
    // "0 HR" means the watch is not advertising the heart-rate service at all,
    // which is a watch-side problem; a non-zero count with no connection is
    // ours.
    snprintf(text, sizeof(text), "searching (%d seen, %d HR)", s_last_scan_devices,
             s_last_scan_hr_peers);
    return text;
}


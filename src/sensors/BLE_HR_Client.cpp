#include "BLE_HR_Client.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_system.h>

#include "../system/DataCenter.h"
#include "BleHrParse.h"

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

// How long to wait for a clean disconnect at shutdown. Long enough to cover
// several connection intervals at the slow end of what a watch negotiates.
constexpr uint32_t kShutdownDisconnectMs = 1500;

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

// Set once BLE_HR_StartCoexistScan() has retuned the scan for ANT+. While it
// is false nothing else owns the scan windows, so re-running discovery is
// safe -- which is the difference between "the peer must already be
// broadcasting when the board boots" and "start it whenever you like".
bool s_coexist_scan_active = false;

bool (*s_primary_active)() = nullptr;

// Set when the controller reports the link actually down. This, not
// isConnected(), is the completion signal: NimBLEClient::disconnect() only
// calls ble_gap_terminate() and sets m_connStatus = DISCONNECTING, while
// isConnected() tests m_connStatus == CONNECTED -- so it reads false the
// instant disconnect() returns, before a single packet has gone out. Waiting
// on it measured local bookkeeping and reported "clean in 0ms" while the
// terminate never reached the watch.
volatile bool s_disconnect_event = false;

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
// once: later reconnects dial s_peer_address directly (see the header's note
// on ANT+ coexistence consuming the scan).
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
// so this still works while ANT+ owns the scan windows.
bool ConnectAndSubscribe() {
    if (!s_have_peer) {
        return false;
    }

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
// BLE_HR_Start() before ANT can claim the scan windows, and every reconnect
// from here dials the stored address instead.
void BleHrTask(void *pvParameters) {
    (void)pvParameters;

    uint32_t backoff_ms = kBackoffStartMs;

    for (;;) {
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
            if (s_coexist_scan_active) {
                // ANT+ is riding the scan windows; starting an active scan
                // would pull them out from under it. Idle rather than spin --
                // a reboot is what recovers this.
                vTaskDelay(pdMS_TO_TICKS(kBackoffMaxMs));
                continue;
            }

            // Nothing else owns the scan, so keep looking. The boot-time scan
            // is 15 seconds, and requiring the peer to be broadcasting inside
            // that window meant a watch woken a moment late would never be
            // found however long it broadcast afterwards.
            if (DiscoverPeer() && ConnectAndSubscribe()) {
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
        if (s_failed_connects >= kFailuresBeforeRescan && !s_coexist_scan_active) {
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

    // Stamped here, on a measurement that actually parsed, and before the
    // arbitration hook below: the link is demonstrably alive either way, and
    // letting ANT+ suppress this would have the supervisor tear down a
    // perfectly good BLE link after 20 seconds of deferring to the primary.
    {
        const uint32_t now = millis();
        if (s_had_notify) {
            s_notify_interval_ms = now - s_last_notify_ms;
        }
        s_last_notify_ms = now;
        s_had_notify = true;
    }

    // ANT+ is the primary source (CLAUDE.md §3). While it is tracking, stay
    // connected but leave the topic alone -- see BLE_HR_SetPrimaryActiveHook.
    if (s_primary_active != nullptr && s_primary_active()) {
        return;
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
            prefs.putInt(kNvsShutdownCode, 0);
            prefs.end();
        }
    }

    NimBLEDevice::init("pegasus");

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
}

void BLE_HR_Start() {
    // Discovery runs synchronously (blocking for up to kDiscoveryScanMs), the
    // way deps/esp32-ant's verified coexist example sequences it. It has to
    // finish before BLE_HR_StartCoexistScan()/SoftANT_Start() touch the scan,
    // otherwise the perpetual passive scan would be reconfigured underneath an
    // in-flight active scan.
    if (DiscoverPeer()) {
        ConnectAndSubscribe();
    }

    // Disconnect cleanly on any esp_restart(), so the peer learns the link is
    // gone instead of holding it open and refusing to advertise.
    esp_register_shutdown_handler(BLE_HR_Shutdown);

    xTaskCreatePinnedToCore(BleHrTask, "BLE_HR", kTaskStackSize, nullptr, kTaskPriority, nullptr,
                            kTaskCore);
}

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
    if (s_client == nullptr || !s_client->isConnected()) {
        RecordShutdown(1, 0);
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
    NimBLEDevice::deinit(true);
    vTaskDelay(pdMS_TO_TICKS(50));
}

void BLE_HR_StartCoexistScan() {
    // Passive, perpetual: deps/esp32-ant's coexist mode retunes these scan
    // windows to receive ANT frames, so one must be running the whole time ANT
    // is open.
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(false);
    scan->start(0, false, true);
    s_coexist_scan_active = true;
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
    if (s_coexist_scan_active) {
        return "no peer (rescan needs restart)";
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

void BLE_HR_SetPrimaryActiveHook(bool (*is_primary_active)()) {
    s_primary_active = is_primary_active;
}

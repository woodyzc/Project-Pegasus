#include "BLE_CSC_Client.h"

#include "../navigation/BLE_TBT_Receiver.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../system/DataCenter.h"
#include "BleCscParse.h"
#include "BleRadioGate.h"

namespace {

// Standard GATT assigned numbers.
const NimBLEUUID kCscService((uint16_t)0x1816);     // Cycling Speed and Cadence
const NimBLEUUID kCscMeasurement((uint16_t)0x2A5B); // CSC Measurement

constexpr uint32_t kDiscoveryScanMs = 10000;
constexpr uint32_t kBackoffStartMs = 1000;
constexpr uint32_t kBackoffMaxMs = 32000;
constexpr uint32_t kRescanGapMs = 3000;
constexpr uint32_t kConnectTimeoutMs = 5000;
constexpr uint32_t kShutdownParkMs = 2500;
constexpr uint32_t kShutdownDisconnectMs = 800;
constexpr int kFailuresBeforeRescan = 3;

// How often the supervisor wakes while the link is up.
//
// Not idle bookkeeping: this is what drives CadenceTracker_Tick, which is the
// only thing that gets a coasting rider to zero when the sensor has stopped
// notifying. A second is well inside the tracker's three-second idle window.
constexpr uint32_t kSuperviseIdleMs = 1000;

// Same 30% duty cycle as the heart-rate scan, and for the same reason: the
// phone's advertisement shares this radio, and a scan that fills its interval
// starves it. See BLE_HR_Client.cpp.
constexpr uint16_t kScanIntervalMs = 100;
constexpr uint16_t kScanWindowMs = 30;

constexpr uint32_t kTaskStackSize = 4096;
constexpr UBaseType_t kTaskPriority = 3;
constexpr BaseType_t kTaskCore = 0; // Core 0: Background Data Core (CLAUDE.md §4)

NimBLEClient *s_client = nullptr;
NimBLEAddress s_peer_address;
bool s_have_peer = false;
volatile bool s_connected = false;
volatile bool s_had_notify = false;
int s_failed_connects = 0;

volatile bool s_shutdown_requested = false;
volatile bool s_task_parked = false;
volatile bool s_disconnect_event = false;

// The tracker is written by NimBLE's host task (from the notify callback) and
// by the supervisor task (from the idle tick), so it needs a lock of its own.
// Short, uncontended, and nothing inside it blocks.
CadenceTracker_t s_tracker;
SemaphoreHandle_t s_tracker_mutex = nullptr;

char s_status[96] = "not started";

void PublishRpm(uint16_t rpm) {
    Cadence_t out;
    out.rpm = rpm;
    DataCenter_Publish(TOPIC_CADENCE, &out);
}

class ClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient *client, int reason) override {
        (void)client;
        (void)reason;
        s_connected = false;
        s_disconnect_event = true;
        // The counters of a sensor that comes back have no relationship to the
        // ones before it -- it may have been power-cycled, or simply kept
        // counting out of range. Forgetting here rather than on reconnect
        // means there is no window in which a stale baseline could be used.
        if (s_tracker_mutex != nullptr &&
            xSemaphoreTake(s_tracker_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            CadenceTracker_Reset(&s_tracker);
            xSemaphoreGive(s_tracker_mutex);
        }
    }
};
ClientCallbacks s_client_callbacks;

// ⚠️ Passing false for deleteCallbacks is not optional. NimBLEClient's
// setClientCallbacks defaults it to true, and the destructor then calls delete
// on this file-scope static -- free() on a pointer in no heap, which asserts
// inside heap_caps_free with a message that never mentions BLE. The same trap
// as BLE_TBT_Receiver's server callbacks; see CLAUDE.md §8.

void OnNotify(NimBLERemoteCharacteristic *characteristic, uint8_t *data, size_t length,
              bool is_notify) {
    (void)characteristic;
    (void)is_notify;

    s_had_notify = true;

    CscMeasurement_t m;
    if (!Csc_ParseMeasurement(data, length, &m)) {
        // A combined speed-and-cadence sensor may legitimately send a
        // wheel-only packet. Not an error, just nothing to say.
        return;
    }

    if (s_tracker_mutex == nullptr) {
        return;
    }
    uint16_t rpm = 0;
    bool changed = false;
    if (xSemaphoreTake(s_tracker_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        changed = CadenceTracker_Update(&s_tracker, &m, millis(), &rpm);
        xSemaphoreGive(s_tracker_mutex);
    }
    // Published outside the lock. DataCenter wakes subscribers synchronously
    // on the publisher's task, and holding a mutex across other people's
    // callbacks is how a short lock becomes a long one.
    if (changed) {
        PublishRpm(rpm);
    }
}

bool DiscoverPeer() {
    Serial.println("[BLE_CSC] scanning for a 0x1816 peripheral...");
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    NimBLEScanResults results = scan->getResults(kDiscoveryScanMs, false);

    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice *device = results.getDevice(i);
        if (device != nullptr && device->isAdvertisingService(kCscService)) {
            s_peer_address = device->getAddress();
            s_have_peer = true;
            Serial.printf("[BLE_CSC] found %s '%s' rssi %d\n", s_peer_address.toString().c_str(),
                          device->getName().c_str(), device->getRSSI());
            return true;
        }
    }

    // What WAS advertising, named on the serial log.
    //
    // A sensor that turns out to speak Cycling Power (0x1818) instead of
    // 0x1816 is otherwise indistinguishable from one with a flat battery:
    // both are simply never found. Printing the service UUIDs of everything
    // nearby is what makes that answerable without guessing.
    Serial.printf("[BLE_CSC] no 0x1816 peripheral among %d advertisers\n", results.getCount());
    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice *device = results.getDevice(i);
        if (device == nullptr || device->getServiceUUIDCount() == 0) {
            continue;
        }
        Serial.printf("[BLE_CSC]   '%s'", device->getName().c_str());
        for (size_t u = 0; u < device->getServiceUUIDCount(); u++) {
            Serial.printf(" %s", device->getServiceUUID(u).toString().c_str());
        }
        Serial.println();
    }
    return false;
}

bool ConnectAndSubscribe() {
    if (!s_have_peer) {
        return false;
    }

    // One connect at a time across both sensor clients, and no advertisement
    // during it. See BleRadioGate.h for what happens without either.
    BleRadioGateHold gate;
    BLE_TBT_PauseAdvertising();
    struct ResumeAdvertising {
        ~ResumeAdvertising() { BLE_TBT_ResumeAdvertising(); }
    } resume_on_exit;

    if (s_client == nullptr) {
        s_client = NimBLEDevice::createClient();
        if (s_client == nullptr) {
            Serial.println("[BLE_CSC] createClient failed");
            return false;
        }
        s_client->setClientCallbacks(&s_client_callbacks, false);
        s_client->setConnectTimeout(kConnectTimeoutMs);
    }

    if (!s_client->connect(s_peer_address)) {
        return false;
    }

    NimBLERemoteService *service = s_client->getService(kCscService);
    NimBLERemoteCharacteristic *measurement =
        (service != nullptr) ? service->getCharacteristic(kCscMeasurement) : nullptr;

    if (measurement == nullptr || !measurement->canNotify() ||
        !measurement->subscribe(true, OnNotify)) {
        Serial.println("[BLE_CSC] 0x2A5B subscribe failed");
        s_client->disconnect();
        return false;
    }

    if (s_tracker_mutex != nullptr &&
        xSemaphoreTake(s_tracker_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        CadenceTracker_Reset(&s_tracker);
        xSemaphoreGive(s_tracker_mutex);
    }
    s_connected = true;
    s_had_notify = false;
    Serial.printf("[BLE_CSC] connected to %s, subscribed to 0x2A5B\n",
                  s_peer_address.toString().c_str());
    return true;
}

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

void BleCscTask(void *pv) {
    (void)pv;
    uint32_t backoff_ms = kBackoffStartMs;

    for (;;) {
        if (s_shutdown_requested) {
            s_task_parked = true;
            for (;;) {
                vTaskDelay(portMAX_DELAY);
            }
        }

        if (s_connected) {
            backoff_ms = kBackoffStartMs;
            s_failed_connects = 0;

            // One pass does two jobs, under one lock.
            //
            // The tick is what gets a coasting rider to zero when the sensor
            // has stopped notifying rather than repeating itself -- sensors
            // differ on which they do, and the panel must not hold the last
            // rpm either way.
            //
            // The publish that follows is unconditional, not just on change.
            // The dashboard blanks the cell after five seconds of silence, and
            // a rider holding a steady cadence produces no changes at all --
            // so without a heartbeat here the most stable reading on the panel
            // would be the one that kept disappearing. Silence on this bus
            // means "sensor gone", and that has to stay true.
            //
            // Cheap: one two-byte publish a second, to one subscriber that
            // sets a flag.
            if (s_tracker_mutex != nullptr && s_had_notify) {
                uint16_t rpm = 0;
                if (xSemaphoreTake(s_tracker_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    uint16_t ticked = 0;
                    CadenceTracker_Tick(&s_tracker, millis(), &ticked);
                    rpm = s_tracker.rpm;
                    xSemaphoreGive(s_tracker_mutex);
                    // Outside the lock: DataCenter runs subscriber callbacks
                    // synchronously on this task, and holding a mutex across
                    // someone else's code is how a short lock becomes a long
                    // one.
                    PublishRpm(rpm);
                }
            }

            vTaskDelay(pdMS_TO_TICKS(kSuperviseIdleMs));
            continue;
        }

        if (!s_have_peer) {
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

        ResetClient();
        s_failed_connects++;
        if (s_failed_connects >= kFailuresBeforeRescan) {
            Serial.println("[BLE_CSC] stored address is not answering, rediscovering");
            s_have_peer = false;
            s_failed_connects = 0;
            backoff_ms = kBackoffStartMs;
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = (backoff_ms >= kBackoffMaxMs) ? kBackoffMaxMs : (backoff_ms * 2);
    }
}

} // namespace

void BLE_CSC_Init(void) {
    // Scan parameters only. Starting a scan here would close the GATT table
    // before BLE_TBT_Start() registers its service, which panics the chip --
    // see main.cpp.
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan != nullptr) {
        scan->setInterval(kScanIntervalMs);
        scan->setWindow(kScanWindowMs);
    }
    if (s_tracker_mutex == nullptr) {
        s_tracker_mutex = xSemaphoreCreateMutex();
    }
    CadenceTracker_Reset(&s_tracker);
}

void BLE_CSC_Start(void) {
    // No synchronous discovery here, unlike BLE_HR_Start().
    //
    // That one scans before returning because its peer is the rider's watch
    // and the dashboard should have a heart rate as early as possible. Doing
    // the same here would add another ten seconds to a setup() that already
    // blocks for fifteen, and would do it while holding the gate the
    // heart-rate supervisor is waiting on. The task's own first pass finds the
    // sensor a moment later, and nothing is waiting on it.
    xTaskCreatePinnedToCore(BleCscTask, "BLE_CSC", kTaskStackSize, nullptr, kTaskPriority,
                            nullptr, kTaskCore);
}

void BLE_CSC_Park(void) {
    s_shutdown_requested = true;

    // A discovery scan would otherwise hold the task for the rest of
    // kDiscoveryScanMs. Safe when none is running.
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan != nullptr) {
        scan->stop();
    }

    const uint32_t started = millis();
    while (!s_task_parked && (millis() - started) < kShutdownParkMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Say goodbye if there is anyone to say it to. Best-effort and short: a
    // cadence sensor's supervision timeout is seconds, so unlike a watch it
    // recovers on its own if this does not land.
    if (s_client != nullptr && s_client->isConnected()) {
        s_disconnect_event = false;
        s_client->disconnect();
        const uint32_t deadline = millis() + kShutdownDisconnectMs;
        while (!s_disconnect_event && (int32_t)(millis() - deadline) < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // Deliberately NOT deleting the client or deinitialising anything.
    // BLE_HR_Shutdown() owns the teardown and runs deinit(true) straight
    // after this, which destroys every client there is. Doing it here too
    // would be a double free.
    s_client = nullptr;
    s_connected = false;
    s_have_peer = false;
}

bool BLE_CSC_IsConnected(void) {
    return s_connected;
}

const char *BLE_CSC_StatusText(void) {
    if (!s_have_peer && !s_connected) {
        snprintf(s_status, sizeof(s_status), "Cadence: no sensor found");
    } else if (!s_connected) {
        snprintf(s_status, sizeof(s_status), "Cadence: found, connecting");
    } else if (!s_had_notify) {
        snprintf(s_status, sizeof(s_status), "Cadence: connected, no data yet");
    } else {
        uint16_t rpm = 0;
        if (s_tracker_mutex != nullptr &&
            xSemaphoreTake(s_tracker_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            rpm = s_tracker.rpm;
            xSemaphoreGive(s_tracker_mutex);
        }
        snprintf(s_status, sizeof(s_status), "Cadence: connected, %u rpm", (unsigned)rpm);
    }
    return s_status;
}

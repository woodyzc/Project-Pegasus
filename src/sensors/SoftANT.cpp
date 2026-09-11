#include "SoftANT.h"

#include <Arduino.h>

#include "../system/DataCenter.h"
#include <Preferences.h>

#include "ant_node.h"

namespace {

ant_node_t s_node;
bool s_started = false;
bool s_coexist = false;

// millis() of the last HRM page we published. 0 = nothing published yet.
volatile uint32_t s_last_page_ms = 0;
// Mirrors what we last published, so the supervisor only republishes on change.
volatile uint8_t s_last_bpm = 0;

// An HRM master broadcasts at ANTPLUS_PERIOD_HRM (~4.06Hz, a page every
// ~246ms). Five seconds of silence means the strap is off/out of range, not a
// couple of dropped slots.
constexpr uint32_t kStaleTimeoutMs = 5000;
constexpr uint32_t kSupervisorPeriodMs = 1000;

constexpr uint32_t kTaskStackSize = 4096;
constexpr UBaseType_t kTaskPriority = 3;
constexpr BaseType_t kTaskCore = 0; // Core 0: Background Data Core (CLAUDE.md §4)

const char *s_status = "not started";

// Bring-up trace to NVS, readable with esptool over USB. Same reason as the
// BLE and SD traces: this board has no usable serial, and ANT+ has never run
// on it, so the first attempt needs to leave evidence behind.
void NoteAntStep(const char *key, uint8_t value) {
    Preferences prefs;
    if (prefs.begin("pegasus", false)) {
        prefs.putUChar(key, value);
        prefs.end();
    }
}

char s_status_buf[48];
volatile uint32_t s_page_count = 0;
volatile uint16_t s_device_num = 0;
volatile uint8_t s_last_event = 0;

// Every ANT page the node delivered, before the device-type filter below. The
// difference between this and s_page_count is the whole diagnosis when nothing
// shows up: traffic heard but not from an HRM, versus nothing heard at all.
volatile uint32_t s_raw_pages = 0;

void OnAntEvent(ant_node_t *node, uint8_t channel, uint8_t event, void *user) {
    (void)node;
    (void)channel;
    (void)user;
    s_last_event = event;
}

void PublishHeartRate(uint8_t bpm) {
    HeartRate_t hr;
    hr.bpm = bpm;
    // ANT+ HRM data pages 0-4 carry no battery level (it's page 7 in the
    // newer HRM spec, which antplus_hrm_decode doesn't surface), so this
    // stays "unknown" per DataCenter.h's convention.
    hr.battery = 0xFF;
    DataCenter_Publish(TOPIC_HEART_RATE, &hr);
    s_last_bpm = bpm;
}

// Runs on the library's ANT task, which holds the node lock and sits at
// priority configMAX_PRIORITIES-2 to keep the ANT TDMA grid. It must not
// block. DataCenter_Publish() is safe to call here: it takes a recursive
// mutex only briefly, and its one current subscriber (Page_Dashboard) does
// nothing but set a volatile flag. Anything added later that blocks in a
// DataCenter callback would stall ANT reception -- see the warning in
// DataCenter.h.
void OnAntData(ant_node_t *node, const ant_node_rx_t *rx, const uint8_t page[8], void *user) {
    (void)node;
    (void)user;

    s_raw_pages++;

    // Device Type 0x78 (ANTPLUS_DEVTYPE_HRM == 120) -- heart-rate straps only.
    if (rx->device_type != ANTPLUS_DEVTYPE_HRM) {
        return;
    }

    uint8_t bpm = ParseBPM(const_cast<uint8_t *>(page));
    if (bpm == 0) {
        return; // not an HRM data page, or the strap reported an invalid rate
    }

    s_last_page_ms = millis();
    s_page_count++;
    PublishHeartRate(bpm);
}

void OnAntPaired(ant_node_t *node, uint8_t channel, const ant_node_device_t *dev, bool remembered,
                 void *user) {
    (void)node;
    (void)channel;
    (void)user;
    Serial.printf("[SoftANT] HRM %u %s\n", dev->device_num,
                  remembered ? "reconnected (from NVS)" : "paired");
    s_device_num = dev->device_num;
    snprintf(s_status_buf, sizeof(s_status_buf), "tracking #%u%s", dev->device_num,
             remembered ? " (saved)" : "");
    s_status = s_status_buf;
}

} // namespace

uint8_t ParseBPM(uint8_t *payload) {
    if (payload == nullptr) {
        return 0;
    }
    antplus_hrm_data_t hr;
    if (!antplus_hrm_decode(payload, &hr)) {
        return 0;
    }
    return hr.computed_heart_rate; // 0 = invalid, per antplus_hrm_data_t
}

bool SoftANT_IsTracking() {
    return s_started && ant_node_is_tracking(&s_node, 0);
}

void SoftANT_Task(void *pvParameters) {
    (void)pvParameters;

    ant_node_config_t cfg = {};
    cfg.on_data = OnAntData;
    cfg.on_event = OnAntEvent;
    cfg.on_paired = OnAntPaired;
    // Remember the paired strap in NVS so the head unit reconnects to its own
    // strap on the next boot instead of re-searching (and instead of latching
    // onto a passing rider's). nvs_flash_init() is done by the Arduino core.
    cfg.store = &ant_node_store_nvs;
    cfg.coexist = s_coexist;
    // The library's radio task stays on its own Core 1 default -- the one
    // deliberate exception to CLAUDE.md §4's "Core 0 = Background Data Core"
    // (documented there too). This supervisor still runs on Core 0.
    //
    // Reasoning, from reading the library rather than guessing:
    //   * Core 0 is where the BT controller lives, and this task runs at
    //     configMAX_PRIORITIES-2. Putting it there means a max-priority task
    //     contending with the very controller it hooks -- which is exactly
    //     why the library defaults it to Core 1.
    //   * The "but it will starve LVGL on Core 1" worry does not hold for a
    //     receiver. ant_node's loop is tick -> wait_until(deadline), and in RX
    //     mode wait_until() takes the ant_espphy_wait_rx() path, which blocks
    //     in ulTaskNotifyTake() and yields the core. It only busy-waits
    //     (esp_rom_delay_us) for sub-millisecond TX deadlines, and we are a
    //     slave that never transmits. So it wakes on a frame (~4Hz for HRM)
    //     plus radio events, does a short ant_mac_tick(), and blocks again.
    //   * Core 1 is also the configuration the library actually verified live
    //     on an S3 tracking a real strap; Core 0 is unverified.
    //
    // Net: the timing-critical radio task gets the safe, proven core, and §4's
    // real intent -- our own data handling on Core 0 -- is served by this
    // supervisor and by DataCenter publishing.
    cfg.task_core = ANT_NODE_CORE_1;
    // proximity_rssi stays 0 (accept any strap) for first bring-up. -70 is
    // "on my bike" per the library's docs -- worth setting once pairing is
    // known to work, so a passing rider's strap can't be picked up.
    cfg.proximity_rssi = 0;

    ant_espphy_status_t status = ant_node_start(&s_node, &cfg);
    Serial.printf("[SoftANT] ant_node_start: %s (coexist=%d)\n", ant_espphy_status_str(status),
                  (int)s_coexist);

    if (status != ANT_ESPPHY_OK) {
        // Nothing is running -- most likely a BLE host owns the controller
        // (ANT_ESPPHY_ERR_BT) and we were started without coexist mode.
        Serial.println("[SoftANT] radio unavailable, ANT+ disabled");
        // ANT_ESPPHY_ERR_BT here means a BLE host still owns the controller,
        // which is the failure worth naming: it is a sequencing mistake in
        // main.cpp, not a missing strap.
        snprintf(s_status_buf, sizeof(s_status_buf), "radio unavailable (%s)",
                 ant_espphy_status_str(status));
        s_status = s_status_buf;
        NoteAntStep("ant_start", 0);
        s_started = false;
        vTaskDelete(nullptr);
        return;
    }
    s_started = true;
    NoteAntStep("ant_start", 1);
    s_status = "searching for strap";

    // Channel 0, any HRM (device_num 0 => the strap remembered in NVS if there
    // is one, else the first one heard, which then becomes the remembered one).
    uint8_t rc = ant_node_open_antplus_slave(&s_node, 0, ANTPLUS_DEVTYPE_HRM, 0, ANTPLUS_PERIOD_HRM);
    NoteAntStep("ant_chan", rc == 0 ? 1 : 0);
    if (rc != 0) {
        Serial.printf("[SoftANT] open HRM channel failed, ANT response %u\n", rc);
        snprintf(s_status_buf, sizeof(s_status_buf), "channel open failed (%u)", rc);
        s_status = s_status_buf;
    }

    // Supervisory loop. The actual page capture happens on the library's task
    // (see SoftANT.h); this only watches for the strap disappearing so the
    // dashboard can drop back to "--" rather than holding a stale BPM forever.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kSupervisorPeriodMs));

        uint32_t last = s_last_page_ms;
        bool stale = (last == 0) || ((millis() - last) > kStaleTimeoutMs);
        if (stale && s_last_bpm != 0) {
            PublishHeartRate(0); // 0 bpm = no live strap
        }
    }
}

const char *SoftANT_StatusText() {
    return s_status;
}

uint32_t SoftANT_PageCount() {
    return s_page_count;
}

uint16_t SoftANT_DeviceNumber() {
    return s_device_num;
}

uint32_t SoftANT_Ticks() {
    return s_started ? s_node.ticks : 0;
}

uint32_t SoftANT_RawPages() {
    return s_raw_pages;
}

uint8_t SoftANT_LastEvent() {
    return s_last_event;
}

void SoftANT_Start(bool coexist_with_ble) {
    s_coexist = coexist_with_ble;
    xTaskCreatePinnedToCore(SoftANT_Task, "SoftANT", kTaskStackSize, nullptr, kTaskPriority, nullptr,
                            kTaskCore);
}

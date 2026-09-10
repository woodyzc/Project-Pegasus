#include "SoftANT.h"

#include <Arduino.h>

#include "../system/DataCenter.h"
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

    // Device Type 0x78 (ANTPLUS_DEVTYPE_HRM == 120) -- heart-rate straps only.
    if (rx->device_type != ANTPLUS_DEVTYPE_HRM) {
        return;
    }

    uint8_t bpm = ParseBPM(const_cast<uint8_t *>(page));
    if (bpm == 0) {
        return; // not an HRM data page, or the strap reported an invalid rate
    }

    s_last_page_ms = millis();
    PublishHeartRate(bpm);
}

void OnAntPaired(ant_node_t *node, uint8_t channel, const ant_node_device_t *dev, bool remembered,
                 void *user) {
    (void)node;
    (void)channel;
    (void)user;
    Serial.printf("[SoftANT] HRM %u %s\n", dev->device_num,
                  remembered ? "reconnected (from NVS)" : "paired");
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
    cfg.on_paired = OnAntPaired;
    // Remember the paired strap in NVS so the head unit reconnects to its own
    // strap on the next boot instead of re-searching (and instead of latching
    // onto a passing rider's). nvs_flash_init() is done by the Arduino core.
    cfg.store = &ant_node_store_nvs;
    cfg.coexist = s_coexist;
    // Pin the library's own receive task to Core 0 alongside this supervisor,
    // per CLAUDE.md §4 (Core 0 = Background Data Core, Core 1 = UI).
    //
    // Worth knowing: the library defaults this task to Core 1 precisely
    // BECAUSE the BT controller lives on Core 0, and this task runs at
    // configMAX_PRIORITIES-2. So the spec's layout puts a max-priority task on
    // the same core as the controller it's hooking. The alternative
    // (ANT_NODE_CORE_1) avoids that but preempts the LVGL render loop instead,
    // which is why the spec puts it here. Untested on hardware either way --
    // if ANT frames are dropped under load, try ANT_NODE_CORE_1 and check
    // whether UI jank is the better trade.
    cfg.task_core = ANT_NODE_CORE_0;
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
        s_started = false;
        vTaskDelete(nullptr);
        return;
    }
    s_started = true;

    // Channel 0, any HRM (device_num 0 => the strap remembered in NVS if there
    // is one, else the first one heard, which then becomes the remembered one).
    uint8_t rc = ant_node_open_antplus_slave(&s_node, 0, ANTPLUS_DEVTYPE_HRM, 0, ANTPLUS_PERIOD_HRM);
    if (rc != 0) {
        Serial.printf("[SoftANT] open HRM channel failed, ANT response %u\n", rc);
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

void SoftANT_Start(bool coexist_with_ble) {
    s_coexist = coexist_with_ble;
    xTaskCreatePinnedToCore(SoftANT_Task, "SoftANT", kTaskStackSize, nullptr, kTaskPriority, nullptr,
                            kTaskCore);
}

#include <Arduino.h>
#include <lvgl.h>

#include "hal/Battery.h"
#include "hal/Display.h"
#include "hal/Touch.h"
#include "navigation/BLE_TBT_Receiver.h"
#include "navigation/GpxTrack.h"
#include "sensors/BLE_HR_Client.h"
#include "sensors/GPS_Reader.h"
#include "sensors/SoftANT.h"
#include "system/DataCenter.h"
#include "system/LvglTask.h"
#include "system/PageManager/PageManager.h"
#include "system/Settings.h"
#include "ui/Page_Dashboard.h"
#include "ui/Page_Settings.h"

static lv_indev_drv_t s_indev_drv;

// Page stack per CLAUDE.md §4 ("X-TRACK PageManager life cycle management").
// File-scope so they outlive setup(): PageManager keeps raw pointers to the
// registered pages for the life of the program.
static PageManager s_page_manager;
static PageDashboard s_page_dashboard;
static PageSettings s_page_settings;

void setup() {
    Serial.begin(115200);

    lv_init();
    DataCenter_Init();

    Display_Init();
    Touch_Init();
    Battery_Init();
    GPS_Init();

    // After Display_Init(): applies the persisted backlight level to the panel.
    Settings_Init();

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = Touch_Read;
    lv_indev_drv_register(&s_indev_drv);

    // setup()/loop() run on Core 1 (arduino-esp32's default loopTask
    // pinning), so this satisfies the pages' "Core 1 only" precondition.
    s_page_manager.Register(&s_page_dashboard, PAGE_NAME_DASHBOARD);
    s_page_manager.Register(&s_page_settings, PAGE_NAME_SETTINGS);
    s_page_manager.SetGlobalLoadAnimType(PageManager::LOAD_ANIM_OVER_LEFT, 300);
    s_page_manager.Push(PAGE_NAME_DASHBOARD);

    LvglTask_Start(); // Core 1: lv_timer_handler() loop (CLAUDE.md §4)

    // Core 0 power monitoring. Started before the radios because it is cheap
    // and independent -- if a radio mode stalls below, the battery reading is
    // already publishing.
    Battery_StartMonitor();

    // Core 0 GNSS reader. Independent of the heart-rate radios below, so it
    // starts first: a stall in radio bring-up should not cost the position
    // fix as well.
    GPS_StartReader();

    // Heart-rate radios come after LvglTask_Start() on purpose: BLE_HR_Start()
    // does its discovery synchronously and blocks for up to ~15s (see
    // BLE_HR_Client.h), so starting the UI task first means the dashboard is
    // already drawn and refreshing while the scan runs, instead of the screen
    // sitting blank until a peer is found.
    //
    // Which radios come up is the user's choice, persisted in Settings. These
    // are init-time configurations rather than a runtime switch, so the
    // settings page saves the choice and asks for a restart.
    // Raised before the radios are touched and cleared once we're through, so
    // a mode that hangs here is caught on the next boot (see Settings.h).
    Settings_NoteRadioBringUpStart();

    switch (Settings_GetHrSource()) {
        case HR_SOURCE_ANT:
            // Exclusive use of the BLE controller as an ANT modem; no NimBLE
            // host is started at all in this mode.
            SoftANT_Start(false);
            break;

        case HR_SOURCE_BLE:
        default:
            BLE_HR_Init();
            BLE_HR_Start();
            // Turn-by-turn shares the NimBLE stack the HR client brings up.
            // Settings guarantees NAV_MODE_TBT implies this branch (the
            // exclusivity rule in Settings.h), but honour the mode explicitly
            // rather than assuming: with GPX selected there is no reason to
            // advertise a service nothing will write to.
            if (Settings_GetNavMode() == NAV_MODE_TBT) {
                BLE_TBT_Start();
            }
            break;
    }

    // Got through radio bring-up: clear the flag so the next boot honours the
    // user's choice instead of falling back to BLE.
    Settings_NoteRadioBringUpOk();

    // Offline breadcrumbs. Only in GPX mode: mounting a card and reading a
    // multi-megabyte file costs time and PSRAM that TBT mode has no use for.
    // A missing card is not an error -- it just means no trail.
    if (Settings_GetNavMode() == NAV_MODE_GPX) {
        if (GpxTrack_MountCard()) {
            GpxTrack_LoadFirstAvailable();
        }
    }

    // TODO(Phase 1 Task 1.3+): remaining Core 0 tasks publishing into
    // DataCenter (GPS_Info, Sensor/IMU) -- Page_Dashboard is already
    // subscribed and waiting for real data.
}

void loop() {
    // Intentionally empty: Core 1's UI work runs in lvgl_task(); Core 0
    // background tasks (GPS/ANT+/BLE/power) land here in later Phase 1 tasks.
    vTaskDelay(pdMS_TO_TICKS(1000));
}

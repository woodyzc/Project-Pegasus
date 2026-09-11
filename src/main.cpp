#include <Arduino.h>
#include <lvgl.h>

#include "hal/Battery.h"
#include "hal/Display.h"
#include "hal/Touch.h"
#include "navigation/BLE_TBT_Receiver.h"
#include "navigation/GpxTrack.h"
#include "navigation/RideLog.h"
#include "sensors/BLE_HR_Client.h"
#include "sensors/GPS_Reader.h"
#include "sensors/SoftANT.h"
#include "system/DataCenter.h"
#include "system/LvglTask.h"
#include "system/PageManager/PageManager.h"
#include "system/Settings.h"
#include "system/Trip.h"
#include "ui/Page_Dashboard.h"
#include "ui/Page_Map.h"
#include "ui/Page_Settings.h"

static lv_indev_drv_t s_indev_drv;

// Page stack per CLAUDE.md §4 ("X-TRACK PageManager life cycle management").
// File-scope so they outlive setup(): PageManager keeps raw pointers to the
// registered pages for the life of the program.
static PageManager s_page_manager;
static PageDashboard s_page_dashboard;
static PageSettings s_page_settings;
static PageMap s_page_map;

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

    // Every page's root object needs this, and without it nothing lays out.
    //
    // PageManager creates each root with a bare lv_obj_create() and applies
    // whatever style the application hands it -- upstream X-TRACK does exactly
    // the same and supplies this from App.cpp. Porting the manager without the
    // style left every root at LVGL's default object size, LV_DPI_DEF square:
    // 130x130 in the top-left of a 240x320 panel, with the rest of the screen
    // showing through in the default light theme. Absolutely-positioned cells
    // were clipped and anything aligned TOP_MID or TOP_RIGHT bunched into the
    // left corner, because they were aligning against a 130px-wide parent.
    //
    // Border, padding and radius are zeroed as well: the default theme gives a
    // plain lv_obj all three, and the pages position their children in screen
    // coordinates, which padding would silently shift.
    static lv_style_t root_style;
    lv_style_init(&root_style);
    lv_style_set_width(&root_style, LV_HOR_RES);
    lv_style_set_height(&root_style, LV_VER_RES);
    lv_style_set_bg_opa(&root_style, LV_OPA_COVER);
    lv_style_set_bg_color(&root_style, lv_color_hex(0x101820)); // Page_Dashboard's COLOR_BG
    lv_style_set_border_width(&root_style, 0);
    lv_style_set_pad_all(&root_style, 0);
    lv_style_set_radius(&root_style, 0);
    s_page_manager.SetRootDefaultStyle(&root_style);

    // The screen behind the pages, seen for a moment during a page transition
    // when one root has slid partway off. White by default, which against this
    // palette reads as a flash.
    lv_disp_set_bg_color(lv_disp_get_default(), lv_color_hex(0x101820));

    // setup()/loop() run on Core 1 (arduino-esp32's default loopTask
    // pinning), so this satisfies the pages' "Core 1 only" precondition.
    s_page_manager.Register(&s_page_dashboard, PAGE_NAME_DASHBOARD);
    s_page_manager.Register(&s_page_settings, PAGE_NAME_SETTINGS);
    s_page_manager.Register(&s_page_map, PAGE_NAME_MAP);

    // ---- The card must be up BEFORE the first page is pushed ----
    // The card is mounted whatever the navigation mode, because ride logging
    // writes to it in both. Loading a route is GPX-only: that is the part that
    // costs time and PSRAM, and TBT mode has no use for it. A missing card is
    // not an error -- it just means no trail and no log.
    //
    // This used to sit after the radios, near the end of setup(), and the
    // dashboard was left permanently reading "No SD card" on a board whose
    // card was fine. PageManager caches a page once loaded, so the dashboard
    // built its map against whatever was true at Push() and never looked
    // again -- while the ROUTE page, pushed by the rider much later, saw the
    // mounted card and drew the route correctly. Same card, two answers.
    //
    // Unlike the radios there is no reason to defer this: mounting is fast and
    // does not block for 15s the way BLE_HR_Start() does.
    if (GpxTrack_MountCard() && Settings_GetNavMode() == NAV_MODE_GPX) {
        GpxTrack_LoadFirstAvailable();
    }

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

            // ---- This order is load-bearing. Do not swap these two. ----
            // Turn-by-turn shares the NimBLE stack the HR client brings up,
            // and registering its GATT service has to happen while the GATT
            // table is still mutable. NimBLE's ble_gatts_mutable() refuses
            // once ANY of these is true: advertising is active, a scan is
            // active, a connection attempt is in flight, or a connection is
            // established. BLE_HR_Start() ends with all of the last three
            // possible -- it scans, connects, and leaves a supervisor task
            // holding the link up.
            //
            // Registering anyway does not return an error to us. It reaches
            // ble_svc_gap_init(), whose SYSINIT_PANIC_ASSERT(rc == 0) turns
            // BLE_HS_EBUSY into a panic, and the board reboots. That made it
            // look intermittent and hardware-ish, because it only happened
            // when the heart-rate peer was actually in range: with no watch
            // nearby the scan finds nothing, nothing connects, and the same
            // code registers fine.
            //
            // BLE_HR_Init() is safe to precede this -- it only configures the
            // scan parameters, it does not start scanning.
            //
            // Settings guarantees NAV_MODE_TBT implies this branch (the
            // exclusivity rule in Settings.h), but honour the mode explicitly
            // rather than assuming: with GPX selected there is no reason to
            // advertise a service nothing will write to.
            if (Settings_GetNavMode() == NAV_MODE_TBT) {
                BLE_TBT_Start();
            }

            BLE_HR_Start();

            // Advertising goes last, and the split from BLE_TBT_Start() is the
            // point. Registration had to come before any connection existed;
            // advertising has the opposite constraint and must not overlap the
            // discovery scan above.
            //
            // Running them together puts a scan, an advertisement and a
            // connection attempt on the controller at once. An HCI command that
            // misses its ack deadline under that load makes NimBLE reset its
            // host, and its own timer -- which the reset does not cancel --
            // then fires during the re-sync and hits assert(0) in
            // ble_hs_timer_exp. That is a library defect we cannot patch, so
            // the concurrency that provokes it is what has to go.
            if (Settings_GetNavMode() == NAV_MODE_TBT) {
                BLE_TBT_StartAdvertising();
            }
            break;
    }

    // Got through radio bring-up: clear the flag so the next boot honours the
    // user's choice instead of falling back to BLE.
    Settings_NoteRadioBringUpOk();

    // Both read GPS through DataCenter, so they are independent of which page
    // the rider happens to be looking at.
    Trip_Init();
    RideLog_Init();

    // TODO(Phase 1 Task 1.3+): remaining Core 0 tasks publishing into
    // DataCenter (GPS_Info, Sensor/IMU) -- Page_Dashboard is already
    // subscribed and waiting for real data.
}

void loop() {
    // Core 1's UI work runs in lvgl_task() and Core 0's sensors in their own
    // tasks, so this is only for work that must not sit in either: persisting
    // the trip odometer costs an NVS write of tens of milliseconds, which
    // would stall the GPS task if it ran in the publish callback.
    Trip_Service();
    vTaskDelay(pdMS_TO_TICKS(1000));
}

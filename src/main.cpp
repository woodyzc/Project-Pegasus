#include <Arduino.h>
#include <lvgl.h>

#include "hal/Display.h"
#include "hal/Touch.h"
#include "system/DataCenter.h"
#include "system/LvglTask.h"
#include "ui/Page_Dashboard.h"

static lv_indev_drv_t s_indev_drv;

void setup() {
    Serial.begin(115200);

    lv_init();
    DataCenter_Init();

    Display_Init();
    Touch_Init();

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = Touch_Read;
    lv_indev_drv_register(&s_indev_drv);

    // setup()/loop() run on Core 1 (arduino-esp32's default loopTask
    // pinning), so this satisfies Page_Dashboard's "Core 1 only" precondition.
    Page_Dashboard_Create(nullptr); // nullptr => lv_scr_act()

    LvglTask_Start(); // Core 1: lv_timer_handler() loop (CLAUDE.md §4)

    // TODO(Phase 1 Task 1.3+): Core 0 sensor/GPS/BLE tasks publishing into
    // DataCenter (GPS_Info, Sensor/HeartRate, Sensor/IMU) -- Page_Dashboard
    // is already subscribed and waiting for real data.
}

void loop() {
    // Intentionally empty: Core 1's UI work runs in lvgl_task(); Core 0
    // background tasks (GPS/ANT+/BLE/power) land here in later Phase 1 tasks.
    vTaskDelay(pdMS_TO_TICKS(1000));
}

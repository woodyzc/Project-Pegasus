#include <Arduino.h>
#include <lvgl.h>

#include "hal/Display.h"
#include "hal/Touch.h"
#include "system/LvglTask.h"

static lv_indev_drv_t s_indev_drv;

void setup() {
    Serial.begin(115200);

    lv_init();

    Display_Init();
    Touch_Init();

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = Touch_Read;
    lv_indev_drv_register(&s_indev_drv);

    LvglTask_Start(); // Core 1: lv_timer_handler() loop (CLAUDE.md §4)

    // TODO(Phase 1 Task 1.3+): Core 0 sensor/GPS/BLE tasks, PageManager/DataCenter wiring.
}

void loop() {
    // Intentionally empty: Core 1's UI work runs in lvgl_task(); Core 0
    // background tasks (GPS/ANT+/BLE/power) land here in later Phase 1 tasks.
    vTaskDelay(pdMS_TO_TICKS(1000));
}

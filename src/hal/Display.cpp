#include "Display.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>

// TFT_eSPI is configured entirely via the USER_SETUP_LOADED build_flags in
// platformio.ini (ILI9341_DRIVER, TFT_MOSI/SCLK/CS/DC/BL, ...) -- no
// User_Setup.h needed.
static TFT_eSPI tft = TFT_eSPI();

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_color_t *s_buf1 = nullptr;
static lv_color_t *s_buf2 = nullptr;

// LVGL -> TFT_eSPI flush callback: push one rendered chunk to the panel and
// tell LVGL we're done so it can hand back the buffer.
static void Display_Flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)color_p, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(drv);
}

static void Backlight_Init() {
    // TFT_BL is a plain GPIO on this board (no dedicated PWM controller),
    // driven on/off via ledc for future brightness control.
    ledcSetup(0, 5000, 8);
    ledcAttachPin(TFT_BL, 0);
    ledcWrite(0, 255); // full brightness at boot
}

void Display_Init() {
    tft.begin();
    tft.setRotation(0); // portrait, 240x320; revisit once UI layout is finalized
    tft.fillScreen(TFT_BLACK);

    Backlight_Init();

    // Double buffer sized for a full-frame flush (240*320*2 bytes each ==
    // ~150KB/buffer). Trivial against the board's 8MB PSRAM, and a full
    // frame buffer keeps the flush logic simple for a live dashboard UI.
    // Allocated with MALLOC_CAP_SPIRAM so LVGL's rendering does not compete
    // with the ~512KB of internal SRAM the rest of the firmware needs.
    const size_t buf_pixels = TFT_WIDTH * TFT_HEIGHT;
    s_buf1 = (lv_color_t *)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    s_buf2 = (lv_color_t *)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (s_buf1 == nullptr || s_buf2 == nullptr) {
        // Out of PSRAM -- nothing sensible to render without a frame buffer.
        Serial.println("[Display] FATAL: PSRAM allocation for LVGL draw buffers failed");
        while (true) {
            vTaskDelay(portMAX_DELAY);
        }
    }

    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, buf_pixels);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = TFT_WIDTH;
    s_disp_drv.ver_res = TFT_HEIGHT;
    s_disp_drv.flush_cb = Display_Flush;
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);
}

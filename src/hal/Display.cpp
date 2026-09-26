#include "Display.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>

// TFT_eSPI is configured entirely via the USER_SETUP_LOADED build_flags in
// platformio.ini (ST7789_DRIVER, TFT_MOSI/SCLK/CS/DC/RST/BL, ...) -- no
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

// ledc channel shared by Backlight_Init() and Display_SetBrightness().
static constexpr uint8_t BACKLIGHT_LEDC_CHANNEL = 0;
static uint8_t s_brightness_pct = 100;

static void Backlight_Init() {
    // TFT_BL is a plain GPIO on this board (no dedicated PWM controller),
    // driven on/off via ledc for future brightness control. Active high: the
    // vendor drives the same pin with a plain 0-100% duty and no inversion.
    ledcSetup(BACKLIGHT_LEDC_CHANNEL, 5000, 8);
    ledcAttachPin(TFT_BL, BACKLIGHT_LEDC_CHANNEL);
    ledcWrite(BACKLIGHT_LEDC_CHANNEL, 255); // full brightness at boot
}

void Display_SetBrightness(uint8_t percent) {
    if (percent > 100) {
        percent = 100;
    }
    s_brightness_pct = percent;
    // 8-bit ledc range; rounds so 100% lands exactly on 255.
    ledcWrite(BACKLIGHT_LEDC_CHANNEL, (uint32_t)((percent * 255 + 50) / 100));
}

uint8_t Display_GetBrightness() {
    return s_brightness_pct;
}

void Display_Init() {
    tft.begin();
    tft.setRotation(0); // portrait, 240x320; revisit once UI layout is finalized
    tft.fillScreen(TFT_BLACK);

    Backlight_Init();

    // ---- One partial buffer, in INTERNAL RAM ----
    //
    // This was two full-screen buffers in PSRAM (240*320*2 == 150KB each),
    // chosen because PSRAM is plentiful and a whole frame keeps the flush
    // simple. It is also why the UI is sluggish: every pixel is written into
    // PSRAM by the renderer and then read back out of PSRAM by the flush, and
    // PSRAM here is several times slower than internal SRAM and contends with
    // instruction fetches on the same bus. 300KB of PSRAM traffic per frame
    // is the cost, and it lands squarely on the frame rate.
    //
    // A partial buffer in internal RAM is the standard arrangement for LVGL
    // on an ESP32: 40 lines is 19KB, so the renderer and the SPI push both
    // work out of fast memory and the frame is sent in eight chunks instead
    // of one. MALLOC_CAP_DMA because that memory is what TFT_eSPI hands to
    // the SPI peripheral.
    //
    // Single, not double. The flush here is blocking (tft.pushColors returns
    // when the bytes are out), so a second buffer buys nothing -- there is no
    // DMA completion to overlap rendering against. Two would only halve the
    // lines per chunk for the same memory.
    const size_t buf_pixels = TFT_WIDTH * 40;
    s_buf1 = (lv_color_t *)heap_caps_malloc(buf_pixels * sizeof(lv_color_t),
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_buf1 == nullptr) {
        // Fall back to the old PSRAM full-frame arrangement rather than
        // refusing to boot: slow beats blank, and the panel is this board's
        // only way to tell anyone what went wrong (CLAUDE.md §8).
        const size_t full_pixels = TFT_WIDTH * TFT_HEIGHT;
        s_buf1 = (lv_color_t *)heap_caps_malloc(full_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
        if (s_buf1 == nullptr) {
            Serial.println("[Display] FATAL: draw buffer allocation failed");
            while (true) {
                vTaskDelay(portMAX_DELAY);
            }
        }
        lv_disp_draw_buf_init(&s_draw_buf, s_buf1, nullptr, full_pixels);
    } else {
        lv_disp_draw_buf_init(&s_draw_buf, s_buf1, nullptr, buf_pixels);
    }

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = TFT_WIDTH;
    s_disp_drv.ver_res = TFT_HEIGHT;
    s_disp_drv.flush_cb = Display_Flush;
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);
}

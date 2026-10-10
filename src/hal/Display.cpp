#include "Display.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <soc/spi_reg.h>

// Send each rendered strip by DMA while the next one renders, rather than
// pushing it from the CPU and waiting. -D PEGASUS_LCD_DMA=0 restores the
// blocking flush, which is the first thing to try if the panel ever shows
// garbage after a build with this on.
#ifndef PEGASUS_LCD_DMA
#define PEGASUS_LCD_DMA 1
#endif

// TFT_eSPI is configured entirely via the USER_SETUP_LOADED build_flags in
// platformio.ini (ST7789_DRIVER, TFT_MOSI/SCLK/CS/DC/RST/BL, ...) -- no
// User_Setup.h needed.
static TFT_eSPI tft = TFT_eSPI();

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_color_t *s_buf1 = nullptr;
static lv_color_t *s_buf2 = nullptr;

// Which flush is running, for the settings page: the DMA path falls back to
// the blocking one rather than refusing to boot, so the panel has to be able
// to say which it got.
static const char *s_mode_text = "not started";

// LVGL -> TFT_eSPI flush callback: push one rendered chunk to the panel and
// tell LVGL we're done so it can hand back the buffer.
static void Display_Flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    // swap=false: LVGL already renders in the panel's byte order
    // (LV_COLOR_16_SWAP 1 in lv_conf.h). Passing true here would make
    // TFT_eSPI byte-swap every pixel in software, which is what pinned the
    // CPU at 90% during any redraw. The two settings must agree.
    tft.pushColors((uint16_t *)color_p, w * h, false);
    tft.endWrite();

    lv_disp_flush_ready(drv);
}

#if PEGASUS_LCD_DMA
// ---- The DMA flush ----
//
// The blocking flush above keeps the CPU in pushColors until the last byte of
// a strip is on the wire: 19.2KB at 80MHz is 1.9ms a strip, so a full repaint
// of the ROUTE page's map is ~12.6ms of the CPU doing nothing but waiting --
// time the next strip could have been rendering in. With two buffers, LVGL
// renders one while the DMA sends the other.
//
// Returning before the transfer is done is safe because of the order: each
// call first waits for the PREVIOUS transfer, so by the time LVGL is handed a
// buffer to render into again, the transfer reading it has finished. The
// buffer being sent now is the other one.
//
// ⚠️ TFT_eSPI 2.5.43 has a bug on this exact configuration, worked around in
// the second line. After a DMA transfer, the SPI peripheral's DMA enable has
// to be cleared before the CPU writes to it directly again -- or the next
// setAddrWindow's command bytes are taken from the DMA engine instead of the
// FIFO, and the window lands somewhere else. TFT_eSPI does clear it, in
// dma_end_callback, but as SPI_DMA_CONF_REG(spi_host) -- and spi_host is the
// DRIVER's enum (SPI3_HOST == 2) while the register macro takes the
// PERIPHERAL number (GPSPI3 == 3). REG_SPI_BASE(2) is GPSPI2: it clears a
// peripheral nothing here uses, and leaves ours enabled. Its own direct path
// uses SPI_PORT, which is right, so that is what clears it here.
static_assert(SPI_PORT == 3, "USE_HSPI_PORT on the S3 should mean GPSPI3");

// Whether TFT_eSPI's initDMA() will succeed, asked without its consequences.
//
// initDMA() wraps both of its driver calls in ESP_ERROR_CHECK, so a failure
// there is an abort -- and on this board a reset releases the power latch, so
// a firmware that aborts in Display_Init is one that switches itself off on
// every boot and can only be reflashed by holding the power key through it.
// The same two calls, with the same arguments, are made here first and
// undone; only if both succeed is initDMA() let near them.
//
// `bus_touched` says whether the probe got as far as claiming the bus: freeing
// it resets the SCLK and MOSI pins, so a probe that fails after that point has
// left the blocking path without a panel to talk to until tft.begin() routes
// them again.
static bool DmaWillStart(bool *bus_touched) {
    *bus_touched = false;
    spi_bus_config_t bus = {};
    bus.mosi_io_num = TFT_MOSI;
    bus.miso_io_num = -1; // TFT_MISO: the panel is write-only here
    bus.sclk_io_num = TFT_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 65536;
    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return false;
    }
    *bus_touched = true;

    spi_device_interface_config_t dev = {};
    dev.mode = TFT_SPI_MODE;
    dev.clock_speed_hz = SPI_FREQUENCY;
    dev.spics_io_num = -1;
    dev.flags = SPI_DEVICE_NO_DUMMY;
    dev.queue_size = 1;
    spi_device_handle_t handle = nullptr;
    const bool ok = spi_bus_add_device(SPI3_HOST, &dev, &handle) == ESP_OK;
    if (ok) {
        spi_bus_remove_device(handle);
    }
    spi_bus_free(SPI3_HOST);
    return ok;
}

static void Display_FlushDma(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;

    tft.dmaWait();                                  // the previous strip is out
    WRITE_PERI_REG(SPI_DMA_CONF_REG(SPI_PORT), 0);  // see the note above
    // Sets the window with direct writes, queues this strip, returns at once.
    // The const overload: no clipping (LVGL areas are on screen) and no byte
    // swap (LV_COLOR_16_SWAP already rendered in the panel's order).
    tft.pushImageDMA(area->x1, area->y1, w, h, (const uint16_t *)color_p);
    lv_disp_flush_ready(drv);
}
#endif

// Chosen in Display_Init: the DMA flush when it could be set up, otherwise the
// blocking one.
static void (*s_disp_drv_flush)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *) = Display_Flush;

// Bumped as LVGL starts rendering each frame. See Display_FrameSeq().
static volatile uint32_t s_frame_seq = 0;

static void Display_RenderStart(lv_disp_drv_t *drv) {
    (void)drv;
    s_frame_seq++;
}

uint32_t Display_FrameSeq() {
    return s_frame_seq;
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

const char *Display_ModeText() {
    return s_mode_text;
}

void Display_Init() {
    tft.begin();
    tft.setRotation(0); // portrait, 240x320; revisit once UI layout is finalized
    tft.fillScreen(TFT_BLACK);

    Backlight_Init();

    // ---- Partial buffers, in INTERNAL RAM ----
    //
    // This was two full-screen buffers in PSRAM (240*320*2 == 150KB each),
    // chosen because PSRAM is plentiful and a whole frame keeps the flush
    // simple. It is also why the UI was sluggish: every pixel is written into
    // PSRAM by the renderer and then read back out of PSRAM by the flush, and
    // PSRAM here is several times slower than internal SRAM and contends with
    // instruction fetches on the same bus. 300KB of PSRAM traffic per frame
    // is the cost, and it lands squarely on the frame rate.
    //
    // A partial buffer in internal RAM is the standard arrangement for LVGL
    // on an ESP32: 40 lines is 19KB, so the renderer and the SPI push both
    // work out of fast memory and the frame is sent in eight chunks instead
    // of one. MALLOC_CAP_DMA because that memory is what the SPI peripheral
    // reads.
    //
    // TWO of them now, because the flush is DMA (see Display_FlushDma): one
    // renders while the other is sent. With the old blocking flush a second
    // buffer bought nothing, which is why there used to be one. It costs
    // 19.2KB more internal RAM -- the settings page read 87KB free before it
    // (2026-10-10) -- and if it is not there, this runs on one buffer and the
    // blocking flush exactly as before.
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
        s_mode_text = "blocking, full frame in PSRAM (no internal RAM)";
    } else {
        bool dma = false;
        const char *why_not = "DMA off in this build";
#if PEGASUS_LCD_DMA
        s_buf2 = (lv_color_t *)heap_caps_malloc(buf_pixels * sizeof(lv_color_t),
                                                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        bool bus_touched = false;
        if (s_buf2 != nullptr && DmaWillStart(&bus_touched)) {
            // ctrl_cs false: chip select stays TFT_eSPI's, held low for good
            // by the startWrite below. endWrite() would wait out every
            // transfer before releasing it, which is the overlap undone --
            // and nothing else shares this bus, so it never needs releasing.
            tft.initDMA();
            tft.startWrite();
            dma = true;
        } else {
            why_not = (s_buf2 == nullptr) ? "no RAM for a second buffer" : "DMA would not start";
            if (bus_touched) {
                // The probe freed the bus and with it the pins; put them back
                // for the blocking flush.
                tft.begin();
                tft.setRotation(0);
                tft.fillScreen(TFT_BLACK);
            }
            if (s_buf2 != nullptr) {
                heap_caps_free(s_buf2);
                s_buf2 = nullptr;
            }
        }
#endif
        lv_disp_draw_buf_init(&s_draw_buf, s_buf1, dma ? s_buf2 : nullptr, buf_pixels);
        static char mode[64];
        if (dma) {
            snprintf(mode, sizeof(mode), "DMA, 2 x 40 lines");
        } else {
            snprintf(mode, sizeof(mode), "blocking, 1 x 40 lines (%s)", why_not);
        }
        s_mode_text = mode;
#if PEGASUS_LCD_DMA
        if (dma) {
            s_disp_drv_flush = Display_FlushDma;
        }
#endif
    }

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = TFT_WIDTH;
    s_disp_drv.ver_res = TFT_HEIGHT;
    s_disp_drv.flush_cb = s_disp_drv_flush;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.render_start_cb = Display_RenderStart;
    lv_disp_drv_register(&s_disp_drv);
}

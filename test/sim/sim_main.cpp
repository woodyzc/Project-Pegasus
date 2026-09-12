// Renders the dashboard to PNG files, on a laptop, using the real page code.
//
// Every layout bug this project has had was invisible until the firmware was
// flashed and the panel photographed: a unit placed off the tile, a number
// with its leading digit clipped, a caption touching the time, an arrow that
// drew nothing. Each cost a flash cycle and a photograph to find and another
// to confirm. This compiles the actual Page_Dashboard.cpp against the actual
// lv_conf.h and the actual generated fonts, so the same questions can be
// answered in a second.
//
// It is a rendering simulator and nothing more. Behaviour lives in the host
// suites next door, which compile the real parsers and accumulators; the
// stubs here return fixed values on purpose.

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "../../src/navigation/TbtParse.h"
#include "../../src/system/TimeSource.h"
#include "../../src/ui/Page_Dashboard.h"
#include "sim_state.h"

#define SCREEN_W 240
#define SCREEN_H 320

static lv_color_t s_canvas[SCREEN_W * SCREEN_H];

static void FlushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colors) {
    for (int32_t y = area->y1; y <= area->y2; y++) {
        for (int32_t x = area->x1; x <= area->x2; x++) {
            s_canvas[y * SCREEN_W + x] = *colors++;
        }
    }
    lv_disp_flush_ready(drv);
}

// A PPM, not a PNG: the format is nine lines of code and needs no library,
// and every viewer on this machine opens one. Converted to PNG by the
// Makefile if a converter is available.
static void WritePpm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        // RGB565 back to bytes. lv_color_to32 would pull in the whole colour
        // module's conversion path for no benefit at this size.
        const uint16_t c = s_canvas[i].full;
        const uint8_t rgb[3] = {
            (uint8_t)(((c >> 11) & 0x1F) * 255 / 31),
            (uint8_t)(((c >> 5) & 0x3F) * 255 / 63),
            (uint8_t)((c & 0x1F) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

// Pushes the simulated state through one full LVGL cycle and saves the result.
static void Render(PageDashboard *page, const char *path) {
    (void)page;
    // Past a second of simulated time, not just a few refresh ticks.
    //
    // The page has two cadences: a 100ms timer that redraws what a publish
    // changed, and a one-second branch inside it that refreshes the clock and
    // the ride averages. An earlier version of this loop advanced 600ms and
    // rendered a screen where the trip, the averages and the unit label were
    // all one state behind -- which looked like four bugs in the page and was
    // one in the simulator.
    for (int i = 0; i < 16; i++) {
        g_sim.millis += 100;
        lv_timer_handler();
    }
    WritePpm(path);
}

static void SetTurn(uint8_t icon, uint32_t distance_m, const char *street, uint8_t exit_number,
                    uint8_t then_icon, uint32_t then_m, uint32_t remaining_m) {
    g_sim.have_tbt = true;
    memset(&g_sim.tbt, 0, sizeof(g_sim.tbt));
    g_sim.tbt.icon_id = icon;
    g_sim.tbt.distance_m = distance_m;
    snprintf(g_sim.tbt.street_name, sizeof(g_sim.tbt.street_name), "%s", street);
    g_sim.tbt.exit_number = exit_number;
    g_sim.tbt.then_icon_id = then_icon;
    g_sim.tbt.then_distance_m = then_m;
    g_sim.tbt.remaining_m = remaining_m;
}

int main(int argc, char **argv) {
    const char *out_dir = argc > 1 ? argv[1] : ".";
    char path[512];

    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf[SCREEN_W * 40];
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, SCREEN_W * 40);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = FlushCb;
    disp_drv.hor_res = SCREEN_W;
    disp_drv.ver_res = SCREEN_H;
    lv_disp_drv_register(&disp_drv);

    g_sim.battery.percent = 92;
    g_sim.battery.on_usb = false;
    g_sim.battery.millivolts = 3980;

    // A clock, so the header renders the way it does on a linked head unit
    // rather than showing the dashes of a device that has never seen a phone.
    // 2026-09-16T18:43:00Z, four hours behind, which is the panel in the last
    // photograph.
    TimeSource_SetFromPhone(1789584180u, -240, "EDT", g_sim.millis);

    PageDashboard page;
    page._root = lv_scr_act();
    page.onViewLoad();

    // The battery is drawn from a publish like everything else.
    Sim_Publish(TOPIC_BATTERY, nullptr, 0);

    // ---- Scene 1: powered on, nothing connected ----
    snprintf(path, sizeof(path), "%s/01-cold-boot.ppm", out_dir);
    Render(&page, path);

    // ---- Scene 2: the state on the bench, a turn from the phone ----
    // The exact case in the last photograph, so the simulator can be checked
    // against a picture of the real panel rather than trusted on its own.
    SetTurn(TBT_ICON_STRAIGHT, 155, "Richter Farm Road", 0, TBT_ICON_NONE, 0,
            TBT_DISTANCE_UNKNOWN);
    g_sim.speed_unit = SPEED_UNIT_MPH;
    Sim_Publish(TOPIC_NAV_TBT, nullptr, 0);
    snprintf(path, sizeof(path), "%s/02-turn-from-phone.ppm", out_dir);
    Render(&page, path);

    // ---- Scene 3: mid-ride, everything live ----
    g_sim.speed_unit = SPEED_UNIT_KMH;
    g_sim.trip_km = 42.18;
    g_sim.avg_kmh = 24.6f;
    g_sim.max_kmh = 51.3f;
    g_sim.avg_bpm = 142;
    g_sim.max_bpm = 176;
    g_sim.have_hr = true;
    g_sim.hr.bpm = 151;
    SetTurn(TBT_ICON_TURN_LEFT, 420, "Kensington Gardens Road", 0, TBT_ICON_TURN_RIGHT, 60,
            12400);
    Sim_Publish(TOPIC_NAV_TBT, nullptr, 0);
    Sim_Publish(TOPIC_HEART_RATE, nullptr, 0);
    snprintf(path, sizeof(path), "%s/03-mid-ride.ppm", out_dir);
    Render(&page, path);

    // ---- Scene 4: a roundabout, close enough to act on ----
    SetTurn(TBT_ICON_ROUNDABOUT, 25, "A413 Wendover Road", 3, TBT_ICON_STRAIGHT, 800, 11000);
    Sim_Publish(TOPIC_NAV_TBT, nullptr, 0);
    snprintf(path, sizeof(path), "%s/04-roundabout-imminent.ppm", out_dir);
    Render(&page, path);

    // ---- Scene 5: the worst strings anything has to hold ----
    // A long street name, a three-digit trip, and a distance in kilometres.
    g_sim.trip_km = 188.44;
    g_sim.avg_kmh = 31.8f;
    g_sim.max_kmh = 68.9f;
    SetTurn(TBT_ICON_SHARP_RIGHT, 1250, "Llanfairpwllgwyngyllgogery", 0, TBT_ICON_UTURN, 1500,
            98000);
    Sim_Publish(TOPIC_NAV_TBT, nullptr, 0);
    snprintf(path, sizeof(path), "%s/05-worst-case-strings.ppm", out_dir);
    Render(&page, path);

    return 0;
}

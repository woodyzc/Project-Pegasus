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
#include "../../src/ui/Overlay_RideSummary.h"
#include "../../src/ui/Page_Map.h"
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

// Renders every combination of maneuver, distance and unit, for the HTML
// viewer to scrub through.
//
// The viewer is a viewer and nothing more: it shows frames produced by this
// program, which is the real page code. A dashboard reimplemented in HTML
// would be a second layout that drifts from the firmware quietly, and would
// answer questions about itself rather than about the panel.
static void RenderGallery(PageDashboard *page, const char *out_dir) {
    // Chosen to cross every threshold the tile has: the imminent colour at
    // 30m, the switch from metres to kilometres at 1000, the switch from feet
    // to miles at 1000ft, and the countdown bar's 500m ceiling.
    static const uint32_t kDistances[] = {1500, 900, 600, 400, 250, 150, 90, 50, 25, 10};
    static const uint8_t kIcons[] = {
        TBT_ICON_STRAIGHT,     TBT_ICON_TURN_LEFT,  TBT_ICON_TURN_RIGHT,
        TBT_ICON_SLIGHT_LEFT,  TBT_ICON_SLIGHT_RIGHT, TBT_ICON_SHARP_LEFT,
        TBT_ICON_SHARP_RIGHT,  TBT_ICON_UTURN,      TBT_ICON_ROUNDABOUT,
        TBT_ICON_ARRIVE,       TBT_ICON_NONE,
    };
    static const SpeedUnit_t kUnits[] = {SPEED_UNIT_KMH, SPEED_UNIT_MPH};
    char path[512];

    // A ride underway, so the metric cells are not all dashes while the turn
    // is being examined.
    g_sim.trip_km = 42.18;
    g_sim.avg_kmh = 24.6f;
    g_sim.max_kmh = 51.3f;
    g_sim.avg_bpm = 142;
    g_sim.max_bpm = 176;
    g_sim.ascent_m = 4988.0f; // four digits: the widest this cell must take
    g_sim.have_hr = true;
    g_sim.hr.bpm = 151;

    for (size_t u = 0; u < sizeof(kUnits) / sizeof(kUnits[0]); u++) {
        g_sim.speed_unit = kUnits[u];
        for (size_t i = 0; i < sizeof(kIcons) / sizeof(kIcons[0]); i++) {
            for (size_t d = 0; d < sizeof(kDistances) / sizeof(kDistances[0]); d++) {
                // A roundabout is the only maneuver an exit number belongs to.
                const uint8_t exit_number = kIcons[i] == TBT_ICON_ROUNDABOUT ? 3 : 0;
                SetTurn(kIcons[i], kDistances[d], "Kensington Gardens Road", exit_number,
                        TBT_ICON_TURN_RIGHT, 60, 12400);
                Sim_Publish(TOPIC_NAV_TBT, nullptr, 0);
                Sim_Publish(TOPIC_HEART_RATE, nullptr, 0);
                snprintf(path, sizeof(path), "%s/f-%s-%u-%u.ppm", out_dir,
                         kUnits[u] == SPEED_UNIT_MPH ? "mph" : "kmh", (unsigned)kIcons[i],
                         (unsigned)kDistances[d]);
                Render(page, path);
            }
        }
    }
}

int main(int argc, char **argv) {
    const char *out_dir = argc > 1 ? argv[1] : ".";
    const bool gallery = argc > 2 && strcmp(argv[2], "--gallery") == 0;
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

    if (gallery) {
        RenderGallery(&page, out_dir);
        return 0;
    }

    if (argc > 2 && strcmp(argv[2], "--route") == 0) {
        // The route page and its file picker, which is a separate page object
        // with its own root. Rendered on its own because it replaces the whole
        // screen rather than sharing it.
        page.onViewUnload();
        lv_obj_clean(lv_scr_act());

        PageMap route;
        route._root = lv_scr_act();
        route.onViewLoad();

        g_sim.gpx_files = 6;

        // A fix a third of the way along the trail, so the ridden and unridden
        // halves are both on screen and the split can be seen to follow the
        // line rather than the screen.
        g_sim.have_gps = true;
        g_sim.gps.fix_valid = true;
        g_sim.gps.time_valid = false;
        g_sim.gps.lat = 38.9100;
        g_sim.gps.lon = -77.1360;
        g_sim.gps.heading = 55.0f;
        g_sim.gps.num_sv = 9;
        Sim_Publish(TOPIC_GPS_INFO, nullptr, 0);

        snprintf(path, sizeof(path), "%s/r1-route-page.ppm", out_dir);
        Render(&page, path);

        // The same fix, the same trail, with the map turned so the rider's
        // heading is at the top. Rendered beside the north-up frame because
        // the only way to see that roads and trail rotate together -- and by
        // the same angle -- is to look at the two.
        //
        // Speed matters here: the smoother refuses a heading below walking
        // pace, so a scene that forgot to set one would render north-up and
        // look like the feature was not wired in.
        g_sim.track_up = true;
        g_sim.gps.speed = 6.0f; // m/s, well clear of MAP_HEADING_MIN_MPS
        Sim_Publish(TOPIC_GPS_INFO, nullptr, 0);
        snprintf(path, sizeof(path), "%s/r4-route-track-up.ppm", out_dir);
        Render(&page, path);
        g_sim.track_up = false;
        Sim_Publish(TOPIC_GPS_INFO, nullptr, 0);

        // Open the picker by pressing the button the rider presses, rather
        // than by calling the handler: that exercises the hit target too.
        Page_Map_OpenRoutePickerForTest();
        snprintf(path, sizeof(path), "%s/r2-picker-six-files.ppm", out_dir);
        Render(&page, path);

        // A card with nothing on it, which is a different message and a
        // different layout.
        Page_Map_ClosePickerForTest();
        g_sim.gpx_files = 0;
        Page_Map_OpenRoutePickerForTest();
        snprintf(path, sizeof(path), "%s/r3-picker-empty.ppm", out_dir);
        Render(&page, path);

        return 0;
    }

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
    g_sim.ascent_m = 847.0f;
    g_sim.have_hr = true;
    g_sim.hr.bpm = 151;

    // A live speed, not dashes. The speed cell is the tightest on the panel --
    // a 32pt number and two 18pt ride figures sharing 150px -- and every scene
    // before this one left it showing "--", which is the one string that
    // always fits. fix_valid without time_valid keeps the clock on the phone's
    // reading rather than sending this through the GNSS zone lookup.
    g_sim.have_gps = true;
    g_sim.gps.fix_valid = true;
    g_sim.gps.time_valid = false;
    g_sim.gps.speed = 18.5f / 3.6f; // m/s, as the receiver reports it
    Sim_Publish(TOPIC_GPS_INFO, nullptr, 0);

    SetTurn(TBT_ICON_TURN_LEFT, 420, "Kensington Gardens Road", 0, TBT_ICON_TURN_RIGHT, 60,
            12400);
    Sim_Publish(TOPIC_NAV_TBT, nullptr, 0);
    Sim_Publish(TOPIC_HEART_RATE, nullptr, 0);
    // A steep climb, so the ELEVATION cell is rendered with its widest
    // plausible grade rather than the "--" this board will always show.
    g_sim.have_imu = true;
    g_sim.imu.pitch = 7.13f; // about +12.5%
    Sim_Publish(TOPIC_IMU_DATA, nullptr, 0);
    snprintf(path, sizeof(path), "%s/03-mid-ride.ppm", out_dir);
    Render(&page, path);

    // ---- Scene 4: a roundabout, close enough to act on ----
    SetTurn(TBT_ICON_ROUNDABOUT, 25, "A413 Wendover Road", 3, TBT_ICON_STRAIGHT, 800, 11000);
    Sim_Publish(TOPIC_NAV_TBT, nullptr, 0);
    snprintf(path, sizeof(path), "%s/04-roundabout-imminent.ppm", out_dir);
    Render(&page, path);

    // ---- Scene 5: the worst strings anything has to hold ----
    // A long street name, a three-digit trip, a distance in kilometres, and
    // the speed cell's real worst case: a three-digit live value sharing 150px
    // with two ride figures. A descent does not reach this, but a settling fix
    // does, and the cell must not draw one number through another when it
    // happens.
    g_sim.trip_km = 188.44;
    g_sim.avg_kmh = 104.2f;
    g_sim.max_kmh = 118.7f;
    g_sim.gps.speed = 105.3f / 3.6f;
    Sim_Publish(TOPIC_GPS_INFO, nullptr, 0);
    SetTurn(TBT_ICON_SHARP_RIGHT, 1250, "Llanfairpwllgwyngyllgogery", 0, TBT_ICON_UTURN, 1500,
            98000);
    Sim_Publish(TOPIC_NAV_TBT, nullptr, 0);
    snprintf(path, sizeof(path), "%s/05-worst-case-strings.ppm", out_dir);
    Render(&page, path);

    // ---- Scene 6: the second data page ----
    // Restored to a plausible mid-ride first, because scene 5 leaves the speed
    // cell at its worst case and this page is about what a rider reads at
    // rest. Every figure at its widest all the same: a duration past six
    // hours, four-digit altitude and descent.
    {
        g_sim.trip_km = 148.72;
        g_sim.avg_kmh = 23.1f;
        g_sim.max_kmh = 62.8f;
        g_sim.avg_bpm = 131;
        g_sim.moving_seconds = 6 * 3600 + 26 * 60 + 14;
        g_sim.gps.speed = 21.4f / 3.6f;
        g_sim.gps.num_sv = 11;
        // A grade, so the INCLINE cell renders a value rather than the "--"
        // this board shows for want of an IMU.
        g_sim.have_imu = true;
        g_sim.imu.pitch = -4.0f;
        g_sim.have_hr = true;
        g_sim.hr.bpm = 148;
        g_sim.battery.percent = 41;
        g_sim.ascent_m = 2140.0f;
        Sim_Publish(TOPIC_GPS_INFO, nullptr, 0);
        Sim_Publish(TOPIC_BATTERY, nullptr, 0);
        Sim_Publish(TOPIC_HEART_RATE, nullptr, 0);

        Sim_Publish(TOPIC_IMU_DATA, nullptr, 0);

        Page_Dashboard_ShowSecondPageForTest(true);
        snprintf(path, sizeof(path), "%s/07-second-page.ppm", out_dir);
        Render(&page, path);
        Page_Dashboard_ShowSecondPageForTest(false);
    }

    // ---- Scene 7: the report a ride ends with ----
    // Every figure at its widest: a three-digit distance, a duration past an
    // hour, a four-digit climb and a filename that fills the line. The whole
    // panel has to hold this without the Done button falling off the bottom.
    {
        g_sim.trip_km = 148.72;
        g_sim.avg_kmh = 23.1f;
        g_sim.max_kmh = 62.8f;
        g_sim.avg_bpm = 131;
        g_sim.max_bpm = 181;
        g_sim.ascent_m = 2140.0f;
        g_sim.moving_seconds = 6 * 3600 + 26 * 60 + 14;
        g_sim.log_points = 18422;
        g_sim.log_file = "/rides/2026-09-13_071204.gpx";

        RideSummary_t summary;
        RideSummary_Capture(&summary);
        Overlay_RideSummary_Show(&summary, true);
        snprintf(path, sizeof(path), "%s/08-ride-summary.ppm", out_dir);
        Render(&page, path);
    }

    return 0;
}

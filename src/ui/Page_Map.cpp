#include "Page_Map.h"

#include <string.h>


#include "../navigation/GpxTrack.h"
#include "../navigation/MapProject.h"
#include "../navigation/RoadMap.h"
#include "../system/DataCenter.h"
#include "../system/PageManager/PageManager.h"
#include "MapView.h"
#include "RoadView.h"

// The same breadcrumb map the dashboard shows inline, given the whole panel.
// Nearly twice the area, which is the difference between reading the trail at
// a junction and squinting at it.
//
// The drawing is MapView's; this file is only the page around it -- header,
// satellite count, scale and file name. The projection deliberately does not
// live here: it lived here once, in parallel with the dashboard's copy, and a
// fix to one would have missed the other.

namespace {

constexpr uint32_t COLOR_BG = 0x101820;
constexpr uint32_t COLOR_CAPTION = 0x93A4B8;
constexpr uint32_t COLOR_VALUE = 0xFFFFFF;
constexpr uint32_t COLOR_ACCENT = 0x61DAFB;
constexpr uint32_t COLOR_PANEL = 0x18232E;

constexpr lv_coord_t MAP_X = 0;
constexpr lv_coord_t MAP_Y = 36;
constexpr lv_coord_t MAP_W = 240;
constexpr lv_coord_t MAP_H = 262;


// Reports the road map: ways loaded, segments drawn of points held, and the
// the path it tried: "not found" on its own sent the first bring-up looking in
// the wrong place, when the answer was one directory away.

// Twice the dashboard's allowance, because this view has roughly twice the
// area to resolve. Its own arrays rather than the dashboard's: during a page
// transition both maps are on screen at once, so sharing would corrupt one
// mid-slide.
constexpr size_t MAX_POLY_POINTS = 512;
lv_point_t s_points[MAX_POLY_POINTS];
MapPoint_t s_projected[MAX_POLY_POINTS];

MapView_t s_view;
lv_obj_t *s_recenter_btn = nullptr;
lv_obj_t *s_status_label = nullptr;
lv_obj_t *s_scale_label = nullptr;
// The route picker, built on demand and destroyed on choosing. Held so a
// second press of the button cannot stack two of them.
lv_obj_t *s_picker = nullptr;
lv_timer_t *s_refresh_timer = nullptr;

volatile bool s_gps_dirty = false;

void OnGpsPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)data;
    (void)size;
    (void)user_arg;
    s_gps_dirty = true;
}

Account s_gps_account("Page_Map/GPS", OnGpsPublished);

void UpdateScale() {
    if (s_scale_label == nullptr) {
        return;
    }
    // A figure rather than a ruler: 240px of panel does not spare the room,
    // and a number is unambiguous.
    const double across = MapView_MetresAcross(&s_view);
    if (across >= 1000.0) {
        lv_label_set_text_fmt(s_scale_label, "%.1f km across", across / 1000.0);
    } else {
        lv_label_set_text_fmt(s_scale_label, "%d m across", (int)across);
    }
}

// Only offered once the view has been moved by hand. A control that is always
// there but usually does nothing trains people to ignore it.
void UpdateRecenterButton() {
    if (s_recenter_btn == nullptr) {
        return;
    }
    if (MapView_IsManual(&s_view)) {
        lv_obj_clear_flag(s_recenter_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_recenter_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

// Dragging the map.
//
// LV_EVENT_PRESSING fires repeatedly while a finger is down, and the input
// device carries the movement since the previous call -- so this consumes a
// delta rather than tracking a start point itself, which is what makes it
// behave correctly when a drag leaves the widget and comes back.
void OnMapPressing(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == nullptr) {
        return;
    }
    lv_point_t vect;
    lv_indev_get_vect(indev, &vect);
    if (vect.x == 0 && vect.y == 0) {
        return;
    }

    MapView_PanPixels(&s_view, vect.x, vect.y);
    RoadView_Refresh();
    UpdateRecenterButton();
}

void OnRecenterClicked(lv_event_t *e) {
    (void)e;
    MapView_Recenter(&s_view);
    RoadView_Refresh();
    UpdateScale();
    UpdateRecenterButton();
}


void OnZoomClicked(lv_event_t *e) {
    const int in = (int)(intptr_t)lv_event_get_user_data(e);
    if (in == 0) {
        MapView_ZoomIn(&s_view);
    } else {
        MapView_ZoomOut(&s_view);
    }
    // The roads are drawn from the view's scale, and LVGL has no idea this
    // changed it.
    RoadView_Refresh();
    UpdateScale();
    UpdateRecenterButton();
}


void RefreshTimerCallback(lv_timer_t *timer) {
    (void)timer;

    if (!s_gps_dirty) {
        return;
    }
    s_gps_dirty = false;

    GPS_Info_t gps;
    if (!DataCenter_Pull(TOPIC_GPS_INFO, &gps, sizeof(gps))) {
        return;
    }

    if (!gps.fix_valid) {
        // The trail stays drawn, framed on itself -- only the rider marker is
        // meaningless without a fix, and MapView hides it.
        MapView_SetPosition(&s_view, &gps);
        RoadView_Refresh();
        lv_label_set_text(s_status_label, "Waiting for fix");
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_CAPTION), 0);
        return;
    }

    MapView_SetPosition(&s_view, &gps);
    lv_label_set_text_fmt(s_status_label, "%d sats", (int)gps.num_sv);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_ACCENT), 0);
    UpdateScale();
}

void ClosePicker() {
    if (s_picker != nullptr) {
        lv_obj_del(s_picker);
        s_picker = nullptr;
    }
}

void OnPickerDismissed(lv_event_t *e) {
    (void)e;
    ClosePicker();
}

// Loads the route the rider tapped, then reframes the map on it.
void OnRouteChosen(lv_event_t *e) {
    const char *path = (const char *)lv_event_get_user_data(e);
    if (path == nullptr || path[0] == '\0') {
        return;
    }

    // Closed first. Loading walks the card and re-thins up to 20,000 points,
    // which is long enough that leaving the list under the rider's finger
    // would look like the tap had missed.
    ClosePicker();

    if (!GpxTrack_Load(path)) {
        if (s_status_label != nullptr) {
            lv_label_set_text(s_status_label, "No track points");
        }
        return;
    }

    // A new route is a new place, so the rider's own pan and zoom no longer
    // mean anything: MapView_Recenter drops the manual flag, and FitTrack then
    // frames the whole of what was just loaded rather than keeping a scale
    // chosen for the last one.
    // The saved camera points at wherever the last route was, which is not
    // where this one is. Dropped along with the rider's pan and zoom, so the
    // new route is framed on itself.
    MapView_ForgetCamera();

    // And the roads under it. A route in another county wants another extract,
    // and the card may well carry it. Silent when nothing covers the new
    // route: the old streets are wrong, but a blank map is not better, and the
    // trail is what the rider came to see.
    {
        double lat = 0.0;
        double lon = 0.0;
        if (GpxTrack_Center(&lat, &lon)) {
            RoadMap_LoadCovering(lat, lon);
        }
    }
    MapView_Recenter(&s_view);
    s_view.zoom_locked = false;
    MapView_FitTrack(&s_view);
    RoadView_Refresh();
    UpdateRecenterButton();
    UpdateScale();
}

// The list of .gpx files on the card, over the map.
void OnChooseRouteClicked(lv_event_t *e) {
    (void)e;
    if (s_picker != nullptr) {
        return;
    }

    const size_t count = GpxTrack_ScanFiles();

    // Full-screen and opaque, not a panel over the map. The filenames are long
    // and the panel is 240px wide; anything less than the whole screen would
    // spend half its width on a map nobody is looking at while choosing.
    s_picker = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_picker, 240, 320);
    lv_obj_center(s_picker);
    lv_obj_set_style_bg_color(s_picker, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(s_picker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_picker, 0, 0);
    lv_obj_set_style_radius(s_picker, 0, 0);
    lv_obj_set_style_pad_all(s_picker, 0, 0);

    lv_obj_t *title = lv_label_create(s_picker);
    lv_label_set_text(title, "CHOOSE ROUTE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    // Which road extract is under the route, because choosing a route can
    // change it and a rider who sees no streets should be able to tell a
    // missing extract from a broken one without leaving this screen.
    {
        lv_obj_t *map_name = lv_label_create(s_picker);
        const char *loaded = RoadMap_LoadedPath();
        const char *shown = loaded;
        // Leading directory trimmed: every one of them starts "/MAP/".
        for (const char *p = loaded; *p != '\0'; p++) {
            if (*p == '/') {
                shown = p + 1;
            }
        }
        lv_label_set_text_fmt(map_name, "roads: %s", shown[0] != '\0' ? shown : "none");
        lv_obj_set_style_text_font(map_name, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(map_name, lv_color_hex(COLOR_CAPTION), 0);
        lv_obj_set_width(map_name, 180);
        lv_label_set_long_mode(map_name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(map_name, LV_TEXT_ALIGN_CENTER, 0);
        // Below the close button, not beside it: centred at this width the
        // caption ran under the button's left edge.
        lv_obj_align(map_name, LV_ALIGN_TOP_MID, 0, 38);
    }

    lv_obj_t *close = lv_btn_create(s_picker);
    lv_obj_set_size(close, 40, 30);
    lv_obj_align(close, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_radius(close, 6, 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x1D2A36), 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x61DAFB), LV_STATE_PRESSED);
    lv_obj_add_event_cb(close, OnPickerDismissed, LV_EVENT_CLICKED, nullptr);
    {
        lv_obj_t *icon = lv_label_create(close);
        lv_label_set_text(icon, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_VALUE), 0);
        lv_obj_center(icon);
    }

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(s_picker);
        // Which of the two reasons, since they need different things from the
        // rider: one wants a card, the other wants a file put on it.
        lv_label_set_text(empty, GpxTrack_CardMounted() ? "No .gpx files on the card"
                                                        : GpxTrack_MountStatus());
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(COLOR_CAPTION), 0);
        lv_obj_set_width(empty, 200);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(empty);
        return;
    }

    lv_obj_t *list = lv_obj_create(s_picker);
    lv_obj_set_size(list, 228, 246);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);

    const char *loaded = GpxTrack_LoadedName();
    for (size_t i = 0; i < count; i++) {
        const char *path = GpxTrack_FilePath(i);

        lv_obj_t *row = lv_btn_create(list);
        // Grows to fit the name, with a floor of 46px because this is pressed
        // with a thumb and a list sized for a fingertip on a desk is not the
        // same list on a bike.
        //
        // Wrapping rather than truncating: these names are long and the part
        // that tells two routes apart is not reliably at either end. A row
        // that says "Custis_Washington..." is a row a rider cannot choose
        // from. Three lines is ugly and legible, which is the right way round.
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, 46, 0);
        lv_obj_set_style_pad_ver(row, 8, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1D2A36), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x61DAFB), LV_STATE_PRESSED);
        // The path is a pointer into GpxTrack's own scan buffer, which stays
        // valid until the next scan -- and the next scan cannot happen while
        // this list is open, because the button that triggers one is behind
        // it.
        lv_obj_add_event_cb(row, OnRouteChosen, LV_EVENT_CLICKED, (void *)path);

        lv_obj_t *label = lv_label_create(row);
        // Without the leading slash: every entry has one, so it is 6px of
        // width spent saying nothing on a panel this narrow.
        lv_label_set_text(label, path[0] == '/' ? path + 1 : path);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

        const bool current = loaded[0] != '\0' && strcmp(loaded, path) == 0;
        lv_obj_set_style_text_color(
            label, lv_color_hex(current ? 0x61DAFB : COLOR_VALUE), 0);
    }
}

void OnBackClicked(lv_event_t *e) {
    PageMap *self = (PageMap *)lv_event_get_user_data(e);
    if (self != nullptr && self->_Manager != nullptr) {
        self->_Manager->Pop();
    }
}

} // namespace

void PageMap::onViewLoad() {
    lv_obj_t *parent = _root;
    lv_obj_set_style_bg_color(parent, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Header ----
    lv_obj_t *back_btn = lv_btn_create(parent);
    lv_obj_set_size(back_btn, 36, 26);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_radius(back_btn, 6, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, OnBackClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(COLOR_VALUE), 0);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "ROUTE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_status_label = lv_label_create(parent);
    lv_label_set_text(s_status_label, "No fix");
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_RIGHT, -8, 12);

    // ---- The map ----
    MapView_Create(&s_view, parent, MAP_X, MAP_Y, MAP_W, MAP_H, s_points, s_projected,
                   MAX_POLY_POINTS);

    // Roads behind the trail, sharing this view's projection.
    RoadView_Attach(&s_view);


    // Drag to pan. The gesture goes on MapView's own container, which already
    // covers the map area exactly -- a separate transparent overlay would have
    // to be kept in step with it for no gain.
    lv_obj_add_flag(s_view.container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_view.container, OnMapPressing, LV_EVENT_PRESSING, nullptr);

    // ---- Zoom ----
    // Right edge, stacked, deliberately large. This is the one control on the
    // map and it is pressed with a thumb, possibly gloved, possibly moving.
    // Small round buttons would be the obvious design and the wrong one.
    {
        static const char *const LABEL[2] = {LV_SYMBOL_PLUS, LV_SYMBOL_MINUS};
        for (int i = 0; i < 2; i++) {
            lv_obj_t *btn = lv_btn_create(parent);
            lv_obj_set_size(btn, 44, 44);
            lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -6, MAP_Y + 16 + i * 52);
            lv_obj_set_style_radius(btn, 8, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            // Semi-transparent: it sits over the map, and a solid button
            // would punch a hole in exactly the thing being navigated.
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x101820), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x61DAFB), LV_STATE_PRESSED);
            lv_obj_add_event_cb(btn, OnZoomClicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            lv_obj_t *label = lv_label_create(btn);
            lv_label_set_text(label, LABEL[i]);
            lv_obj_set_style_text_color(label, lv_color_hex(COLOR_VALUE), 0);
            lv_obj_center(label);
        }
    }

    // Back to the rider. Hidden until the map has been moved by hand, since a
    // control that is always visible but usually inert teaches people to stop
    // seeing it.
    s_recenter_btn = lv_btn_create(parent);
    lv_obj_set_size(s_recenter_btn, 44, 44);
    lv_obj_align(s_recenter_btn, LV_ALIGN_TOP_RIGHT, -6, MAP_Y + 172);
    lv_obj_set_style_radius(s_recenter_btn, 8, 0);
    lv_obj_set_style_shadow_width(s_recenter_btn, 0, 0);
    lv_obj_set_style_bg_color(s_recenter_btn, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(s_recenter_btn, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(s_recenter_btn, lv_color_hex(0x61DAFB), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_recenter_btn, OnRecenterClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_recenter_btn, LV_OBJ_FLAG_HIDDEN);
    {
        lv_obj_t *icon = lv_label_create(s_recenter_btn);
        lv_label_set_text(icon, LV_SYMBOL_GPS);
        lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_VALUE), 0);
        lv_obj_center(icon);
    }

    // ---- Choose a route ----
    // Below the other two in the same column, same size, same treatment: it is
    // pressed with the same thumb and belongs to the same set. Always visible,
    // unlike recenter, because a rider who has not noticed it cannot discover
    // that the card holds a second route at all.
    {
        lv_obj_t *btn = lv_btn_create(parent);
        lv_obj_set_size(btn, 44, 44);
        lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -6, MAP_Y + 120);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x101820), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x61DAFB), LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, OnChooseRouteClicked, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, LV_SYMBOL_DIRECTORY);
        lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_VALUE), 0);
        lv_obj_center(icon);
    }

    // ---- Attribution ----
    // Required, not decorative. The road data is OpenStreetMap under ODbL,
    // which obliges anything built from it to credit the contributors where a
    // user can see it. Only shown when a road map is actually loaded, because
    // crediting OSM for a blank screen would be its own kind of wrong.
    if (RoadMap_IsLoaded()) {
        lv_obj_t *attrib = lv_label_create(parent);
        lv_label_set_text(attrib, "(c) OpenStreetMap contributors");
        lv_obj_set_style_text_font(attrib, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(attrib, lv_color_hex(COLOR_CAPTION), 0);
        lv_obj_set_style_text_opa(attrib, LV_OPA_60, 0);
        lv_obj_align(attrib, LV_ALIGN_BOTTOM_MID, 0, -16);
    }

    // ---- Footer ----
    s_scale_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_scale_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_scale_label, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(s_scale_label, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_label_set_text(s_scale_label, "");

    UpdateRecenterButton();

    if (GpxTrack_PointCount() > 0) {
        // Where the map was last looking, if anywhere. Only when nothing has
        // been saved does this frame the trail -- otherwise enlarging the
        // dashboard's map, going back and enlarging it again would throw the
        // rider's own framing away and refit, which reads as the map resetting
        // itself for no reason.
        if (!MapView_RestoreCamera(&s_view)) {
            // First look of the boot: the whole trail, rather than an
            // arbitrary zoom on a corner of it.
            MapView_FitTrack(&s_view);
        }
        RoadView_Refresh();
        UpdateScale();
    } else if (s_status_label != nullptr) {
        // The status corner rather than a line of its own: with no trail there
        // is no scale and no satellite count to show either, so the corner is
        // free and is already where this page says what it knows.
        lv_label_set_text(s_status_label,
                          GpxTrack_CardMounted() ? "No .gpx on card" : "No SD card");
    }

    DataCenter_Subscribe(TOPIC_GPS_INFO, &s_gps_account);
    s_refresh_timer = lv_timer_create(RefreshTimerCallback, 500, nullptr);
}

void Page_Map_OpenRoutePickerForTest() {
    OnChooseRouteClicked(nullptr);
}

void Page_Map_ClosePickerForTest() {
    ClosePicker();
}

void PageMap::onViewUnload() {
    // No camera save here any more: MapView_Redraw does it on every change, so
    // by the time this runs the camera is already whatever the rider left the
    // map looking at. Saving here as well was not merely redundant, it ran too
    // late -- the dashboard's will-appear fires before this.
    if (s_refresh_timer != nullptr) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = nullptr;
    }
    DataCenter_Unsubscribe(TOPIC_GPS_INFO, &s_gps_account);

    s_status_label = nullptr;
    s_scale_label = nullptr;
    s_recenter_btn = nullptr;
    // Not deleted: it is a child of the page's root, which LVGL is tearing
    // down around us. Only the pointer needs clearing.
    s_picker = nullptr;
}

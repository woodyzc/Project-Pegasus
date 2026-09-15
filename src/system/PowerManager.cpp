#include "PowerManager.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include "../hal/Display.h"
#include "../navigation/RideLog.h"
#include "../sensors/BLE_HR_Client.h"
#include "DataCenter.h"
#include "FileServer.h"
#include "Settings.h"

namespace {

// One minute to dim, three to go dark, five to sleep. The last of those is the
// figure in CLAUDE.md section 2; the two before it exist because going from a
// lit screen straight to nothing is alarming, and because most of the saving
// is in the first step anyway -- the backlight is the largest draw on the
// board by a wide margin.
constexpr uint32_t DIM_AFTER_MS = POWER_DIM_AFTER_MS;
constexpr uint32_t BLANK_AFTER_MS = POWER_BLANK_AFTER_MS;
constexpr uint32_t SLEEP_AFTER_MS = POWER_SLEEP_AFTER_MS;

// Movement counts as activity even though nobody is touching anything. Set
// above the receiver's own wander so a parked bike does not hold the screen
// on all afternoon -- the same problem TripAccum's floor exists to solve, and
// the same answer.
constexpr float MOVING_MPS = 1.0f;

// How long the "going to sleep" notice stays up. Long enough to read, and long
// enough to be interrupted by a touch, which is the point of it.
constexpr uint32_t SLEEP_NOTICE_MS = 2500;

// Four times a second. The thresholds are minutes apart so this could be far
// slower, except that it also decides how quickly the sleep notice clears
// after a touch, and a notice that lingers a whole second after being
// cancelled reads as the device ignoring you.
constexpr uint32_t TICK_MS = 250;

volatile uint32_t s_last_activity_ms = 0;
lv_obj_t *s_notice = nullptr;
uint32_t s_notice_since_ms = 0;
lv_timer_t *s_timer = nullptr;
PowerStage_t s_stage = POWER_STAGE_ACTIVE;
uint8_t s_active_percent = 100;
bool s_screen_off = false;

// Cached from the bus, because reading it in Service() would mean pulling a
// topic every second for a value that changes every few.
volatile bool s_on_usb = true; // assume plugged in until told otherwise

void OnGpsPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)user_arg;
    if (data == nullptr || size != sizeof(GPS_Info_t)) {
        return;
    }
    const GPS_Info_t *gps = (const GPS_Info_t *)data;
    if (gps->fix_valid && gps->speed >= MOVING_MPS) {
        s_last_activity_ms = millis();
    }
}

void OnBatteryPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)user_arg;
    if (data == nullptr || size != sizeof(Battery_t)) {
        return;
    }
    s_on_usb = ((const Battery_t *)data)->on_usb;
}

Account s_gps_account("Power/GPS", OnGpsPublished);
Account s_battery_account("Power/Battery", OnBatteryPublished);

IdlePolicy_t Policy() {
    IdlePolicy_t policy;
    policy.dim_after_ms = DIM_AFTER_MS;
    policy.blank_after_ms = BLANK_AFTER_MS;
    policy.sleep_after_ms = SLEEP_AFTER_MS;
    policy.sleep_enabled = Settings_GetSleepEnabled();
    return policy;
}

IdleInhibit_t Inhibit() {
    IdleInhibit_t inhibit;
    inhibit.on_usb = s_on_usb;
    inhibit.recording = RideLog_IsRecording();
    inhibit.transferring = FileServer_IsRunning();
    return inhibit;
}

// Everything that must be true before the power goes away. CLAUDE.md section 7
// asks for this explicitly, and the ride log's own footer scheme only
// guarantees a valid file at a flush boundary, not at an arbitrary instant.
void PrepareForSleep() {
    // The peer learns the link is gone and resumes advertising, which is what
    // makes the reconnect on wake work rather than hang. BLE_HR_Client's
    // shutdown notes explain why this matters on a watch.
    BLE_HR_Shutdown();
    SD_MMC.end();
}

// Put up before sleeping, not instead of it. A screen that simply stops is
// indistinguishable from a crash, and that would be the first thing a rider
// reported. It is also the last chance to change your mind: a touch while it
// is up counts as activity and cancels the whole thing.
void ShowNotice() {
    Display_SetBrightness(s_active_percent);
    s_notice = lv_label_create(lv_layer_top());
    lv_label_set_text(s_notice, "Sleeping.\nTouch to wake.");
    lv_obj_set_style_text_align(s_notice, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_notice, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_notice, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(s_notice, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_notice, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_notice, 16, 0);
    lv_obj_center(s_notice);
    s_notice_since_ms = millis();
}

void HideNotice() {
    if (s_notice != nullptr) {
        lv_obj_del(s_notice);
        s_notice = nullptr;
    }
}

void EnterSleep() {
    HideNotice();
    PrepareForSleep();
    Display_SetBrightness(0);

    // The touch controller is the only way back. Two pins matter:
    //
    //   INT is the wake source, active-low, so ext0 waits for a 0.
    //   RST must stay high, or the ESP32 releasing every non-RTC pin resets
    //   the CST328 on the way into sleep and nothing is left to interrupt us.
    //
    // Both are within the RTC-capable range on the S3 (GPIO0-21), which is
    // what makes either possible at all.
    gpio_hold_en((gpio_num_t)TOUCH_RST_PIN);
    gpio_deep_sleep_hold_en();
    esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_INT_PIN, 0);

    esp_deep_sleep_start();
    // Not reached: waking runs setup() from the top.
}

void ApplyStage(PowerStage_t stage) {
    if (stage == s_stage) {
        return;
    }
    s_stage = stage;
    Display_SetBrightness(IdlePolicy_Brightness(stage, s_active_percent));
    s_screen_off = (stage == POWER_STAGE_BLANK);
}

void Service(lv_timer_t *timer) {
    (void)timer;

    // While the rider is at full brightness the settings slider is the source
    // of truth; once dimmed it is not, because the backlight then holds a
    // value nobody chose.
    if (s_stage == POWER_STAGE_ACTIVE) {
        s_active_percent = Settings_GetBrightness();
    }

    const IdlePolicy_t policy = Policy();
    const IdleInhibit_t inhibit = Inhibit();
    // Unsigned subtraction, so the 49-day millis() wrap produces a small
    // number rather than an enormous one.
    const uint32_t idle_ms = millis() - s_last_activity_ms;
    const PowerStage_t want = IdlePolicy_Stage(&policy, idle_ms, &inhibit);

    if (want == POWER_STAGE_SLEEP) {
        // Two ticks, not one. Creating the notice and then blocking to display
        // it would mean calling lv_refr_now() from inside a timer LVGL is
        // already running, so instead the notice goes up on one pass and the
        // sleep happens on a later one -- with the ordinary render in between.
        if (s_notice == nullptr) {
            s_stage = POWER_STAGE_SLEEP;
            // Treated as a dark screen even though it is lit, so the touch
            // that cancels the sleep is swallowed. A label is not clickable,
            // so without this the press would fall through the notice and hit
            // whatever button sits under it on the page below -- which on this
            // firmware could be "Start new ride".
            s_screen_off = true;
            ShowNotice();
            return;
        }
        if ((millis() - s_notice_since_ms) >= SLEEP_NOTICE_MS) {
            EnterSleep();
        }
        return;
    }

    // Anything else means the sleep was cancelled, by a touch or by an
    // inhibitor appearing while the notice was up.
    HideNotice();
    ApplyStage(want);
}

} // namespace

void PowerManager_Init() {
    s_active_percent = Settings_GetBrightness();
    s_last_activity_ms = millis();
    s_stage = POWER_STAGE_ACTIVE;
    s_screen_off = false;

    DataCenter_Subscribe(TOPIC_GPS_INFO, &s_gps_account);
    DataCenter_Subscribe(TOPIC_BATTERY, &s_battery_account);

    s_timer = lv_timer_create(Service, TICK_MS, nullptr);
}

void PowerManager_NoteActivity() {
    s_last_activity_ms = millis();

    // Straight back to full, without waiting for the next Service(): a screen
    // that takes a second to brighten reads as a screen that missed the touch.
    if (s_stage != POWER_STAGE_ACTIVE) {
        // Re-read, because the rider may have changed it on the settings page
        // while the stage machine was holding a dimmed value.
        s_active_percent = Settings_GetBrightness();
        s_stage = POWER_STAGE_ACTIVE;
        s_screen_off = false;
        Display_SetBrightness(s_active_percent);
    }
}

bool PowerManager_ScreenIsOff() {
    return s_screen_off;
}

PowerStage_t PowerManager_Stage() {
    return s_stage;
}

uint32_t PowerManager_IdleMs() {
    return millis() - s_last_activity_ms;
}

const char *PowerManager_StageText() {
    switch (s_stage) {
        case POWER_STAGE_DIM:   return "dimmed";
        case POWER_STAGE_BLANK: return "screen off";
        case POWER_STAGE_SLEEP: return "sleeping";
        case POWER_STAGE_ACTIVE:
        default:                return "awake";
    }
}

const char *PowerManager_InhibitText() {
    if (FileServer_IsRunning()) {
        return "file transfer is running";
    }
    if (!Settings_GetSleepEnabled()) {
        return "deep sleep is switched off";
    }
    if (s_on_usb) {
        return "USB power is connected";
    }
    if (RideLog_IsRecording()) {
        return "a ride is being recorded";
    }
    return "";
}

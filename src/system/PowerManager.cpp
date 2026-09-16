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

// ---- CPU frequency, and why 80 is a floor rather than a starting point ----
//
// Blanking the screen used to save the backlight and nothing else: the S3 kept
// running both cores at 240MHz, servicing LVGL, polling touch over I2C and
// re-rendering, all against a panel nobody could see. This drops the clock
// while the screen is dark and puts it back the instant anything happens.
//
// ⚠️ 80MHz is the LOWEST value that is safe here, and the reason is not
// performance. On the ESP32-S3 the APB clock stays at 80MHz for any CPU
// frequency sourced from the PLL -- 240, 160, 80 -- so UART, LEDC and SPI are
// untouched by the switch. Below 80 the CPU is sourced from the crystal and
// APB follows it down, which would change the GNSS baud rate, the backlight
// PWM frequency and the panel's SPI clock at once. arduino-esp32 will not warn
// you: its S3 branch of calculateApb() returns a hardcoded APB_CLK_FREQ, so
// its own apb-change callbacks never fire and every peripheral goes on
// believing the old rate. Pick 40MHz here and the symptom is a GNSS that
// silently stops decoding, a long way from this line.
//
// WiFi is not a concern despite sharing the same sensitivity: the file server
// inhibits every stage including BLANK (see Inhibit()), so the clock is
// already back at full speed for the whole time the radio is up.
//
// BLE is the one thing here that is not proven. setCpuFrequencyMhz() is a raw
// switch that bypasses esp_pm entirely, so unlike the IDF power-management
// framework it takes no lock the BT controller can hold against it, and a
// heart-rate link stays up across a blank. The reasoning for why it should be
// fine: the controller keeps its own clock domain, APB does not move (above),
// and 80MHz is a speed whole ESP32 products run BLE at. The reasoning for why
// to watch it anyway: none of that is a measurement. If the strap drops out
// after the screen goes dark and recovers when it comes back, this is the
// cause -- raise CPU_MHZ_IDLE to 160, or gate the downclock on there being no
// BLE peer connected.
constexpr uint32_t CPU_MHZ_FULL = 240;
constexpr uint32_t CPU_MHZ_IDLE = 80;

// Only BLANK downclocks. DIM still shows a readable screen the rider may be
// watching, and SLEEP is two ticks from deep sleep with a notice up -- neither
// is worth the risk of a sluggish frame for a saving measured in seconds.
uint32_t CpuMhzForStage(PowerStage_t stage) {
    return (stage == POWER_STAGE_BLANK) ? CPU_MHZ_IDLE : CPU_MHZ_FULL;
}

uint32_t s_cpu_mhz = CPU_MHZ_FULL;

// How many times the clock has actually been dropped. The settings page shows
// it, and it is the only way to confirm any of this works: the downclock
// happens exactly when the screen is too dark to read, so a live reading can
// only ever say 240. A count that goes up after a few minutes of not touching
// the device says the switch fired AND that it came back -- because the page
// being legible at all means we are at full speed again.
uint32_t s_downclock_count = 0;

void SetCpuMhz(uint32_t mhz) {
    if (mhz == s_cpu_mhz) {
        return;
    }
    // setCpuFrequencyMhz() returns false and changes nothing on a value the
    // chip cannot produce, so a failed switch leaves us at the old clock --
    // slower than intended, never broken. Record what actually happened rather
    // than what was asked for.
    if (setCpuFrequencyMhz(mhz)) {
        s_cpu_mhz = mhz;
        if (mhz == CPU_MHZ_IDLE) {
            s_downclock_count++;
        }
    }
}

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
    //   the FT6336G on the way into sleep and nothing is left to interrupt us.
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
    SetCpuMhz(CpuMhzForStage(stage));
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
            // Set here rather than through ApplyStage(), and it has to be:
            // ApplyStage would take the brightness to zero on the way in --
            // IdlePolicy_Brightness(SLEEP) is 0 -- only for ShowNotice() to
            // put it straight back, and it would leave s_screen_off false,
            // which is the opposite of what the line below needs.
            s_stage = POWER_STAGE_SLEEP;

            // The clock, by hand, for the same reason. This is the stage that
            // renders a notice and then runs BLE_HR_Shutdown(), and it is
            // reached only from BLANK, which means it inherits 80MHz unless
            // something says otherwise. CpuMhzForStage() has said SLEEP should
            // be at full speed since the downclock was added; until now
            // nothing ever asked it, so the branch was dead and the notice was
            // always drawn at 80.
            SetCpuMhz(CpuMhzForStage(POWER_STAGE_SLEEP));

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
        // Clock first, everything else after. This path exists to feel
        // instant, and the work of waking -- re-reading a setting, pushing a
        // backlight level, then the frame LVGL draws next -- all runs faster
        // once the switch is done. It costs microseconds.
        SetCpuMhz(CPU_MHZ_FULL);

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

uint32_t PowerManager_CpuMhz() {
    return s_cpu_mhz;
}

uint32_t PowerManager_DownclockCount() {
    return s_downclock_count;
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

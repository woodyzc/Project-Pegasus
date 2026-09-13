#include "Touch.h"

#include <Arduino.h>
#include <Wire.h>

// FT6336G I2C address and register map. FT6336G is register-compatible
// with the wider FocalTech FT6x06 family (FT6206/FT6236/FT6336) -- this is
// the standard map used by e.g. Adafruit's FT6206 driver.
static constexpr uint8_t FT6336_I2C_ADDR = 0x38;
static constexpr uint8_t REG_TD_STATUS = 0x02; // low nibble = number of touch points (0-2)
static constexpr uint8_t REG_TOUCH1_XH = 0x03; // bits[3:0] = X high nibble
static constexpr uint8_t REG_TOUCH1_XL = 0x04;
static constexpr uint8_t REG_TOUCH1_YH = 0x05; // bits[3:0] = Y high nibble
static constexpr uint8_t REG_TOUCH1_YL = 0x06;

// Raw-panel-to-display orientation mapping. Measured on the Hosyond/ES3C28P
// panel against Display_Init()'s tft.setRotation(0): touching top-left reads
// (0,1), top-right (239,1), bottom-left (0,319). So the controller already
// reports display coordinates (240x320, origin top-left, X left-to-right,
// Y top-to-bottom) and needs no transform -- all three stay false.
// Revisit if setRotation() ever changes.
static constexpr bool TOUCH_SWAP_XY = false;
static constexpr bool TOUCH_INVERT_X = false;
static constexpr bool TOUCH_INVERT_Y = false;

// The raw range the panel actually produces, stretched onto the screen's.
//
// Identity by default -- 0..239 and 0..319 are the first and last pixels of a
// 240x320 display, and the corner readings above say the controller already
// reports very nearly that. The constants exist because "very nearly" is not
// the same as "exactly": if the digitizer's active area is inset from the
// glass, the corners never reach 0 or the last column, and a control in the
// corner becomes unreachable no matter how large its touch area is made.
//
// To correct it, find the extremes the panel actually reports at each corner
// and put them here; everything between is linear, so four numbers are the
// whole calibration. There was a touch test on the settings page for reading
// them off, removed once the panel was confirmed fine -- a diagnostic nobody
// needs is clutter on a screen this size.
//
// Note there is no pixel 240 or 320. A 240-wide display ends at 239, and a
// mapping that produced 240 would be pointing one column past the screen.
static constexpr uint16_t TOUCH_RAW_MIN_X = 0;
static constexpr uint16_t TOUCH_RAW_MAX_X = TFT_WIDTH - 1;
static constexpr uint16_t TOUCH_RAW_MIN_Y = 0;
static constexpr uint16_t TOUCH_RAW_MAX_Y = TFT_HEIGHT - 1;

// Maps one axis from the panel's range onto the screen's, and clamps.
//
// Clamping is not defensive decoration: a reading a few counts outside the
// calibrated range lands off-screen, and LVGL will happily deliver a press at
// a coordinate no widget occupies -- which is a tap that does nothing, the
// hardest kind of fault to see.
static uint16_t MapAxis(uint16_t raw, uint16_t raw_min, uint16_t raw_max, uint16_t span) {
    if (raw_max <= raw_min) {
        return 0;
    }
    if (raw <= raw_min) {
        return 0;
    }
    if (raw >= raw_max) {
        return (uint16_t)(span - 1);
    }
    const uint32_t scaled = (uint32_t)(raw - raw_min) * (uint32_t)(span - 1);
    return (uint16_t)(scaled / (uint32_t)(raw_max - raw_min));
}

static bool ReadReg(uint8_t reg, uint8_t *buf, uint8_t len) {
    Wire.beginTransmission(FT6336_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom((int)FT6336_I2C_ADDR, (int)len) != len) {
        return false;
    }
    for (uint8_t i = 0; i < len; i++) {
        buf[i] = Wire.read();
    }
    return true;
}

void Touch_Init() {
    // The FT6336G stays held in reset until RST is driven high, and answers
    // nothing on I2C until then. Pulse it low then high and give the
    // controller's own firmware time to come up before the first transfer.
    pinMode(TOUCH_RST_PIN, OUTPUT);
    digitalWrite(TOUCH_RST_PIN, LOW);
    delay(10);
    digitalWrite(TOUCH_RST_PIN, HIGH);
    delay(300);

    Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    Wire.setClock(400000);
    pinMode(TOUCH_INT_PIN, INPUT); // FT6336G INT is active-low, open-drain; not currently used to gate reads (see Touch_Read)
}

void Touch_Read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    uint8_t touch_count = 0;
    if (!ReadReg(REG_TD_STATUS, &touch_count, 1)) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    touch_count &= 0x0F;

    if (touch_count == 0) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    uint8_t coords[4];
    if (!ReadReg(REG_TOUCH1_XH, coords, 4)) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    uint16_t raw_x = ((coords[0] & 0x0F) << 8) | coords[1]; // XH, XL
    uint16_t raw_y = ((coords[2] & 0x0F) << 8) | coords[3]; // YH, YL

    uint16_t x = raw_x;
    uint16_t y = raw_y;
    if (TOUCH_SWAP_XY) {
        uint16_t t = x;
        x = y;
        y = t;
    }
    if (TOUCH_INVERT_X) {
        x = TFT_WIDTH - 1 - x;
    }
    if (TOUCH_INVERT_Y) {
        y = TFT_HEIGHT - 1 - y;
    }

    data->point.x = MapAxis(x, TOUCH_RAW_MIN_X, TOUCH_RAW_MAX_X, TFT_WIDTH);
    data->point.y = MapAxis(y, TOUCH_RAW_MIN_Y, TOUCH_RAW_MAX_Y, TFT_HEIGHT);
    data->state = LV_INDEV_STATE_PR;
}

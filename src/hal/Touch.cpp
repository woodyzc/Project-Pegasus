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

// Raw-panel-to-display orientation mapping. Display_Init() currently sets
// tft.setRotation(0); these flags are placeholders to flip during hardware
// bring-up if the reported touch point doesn't line up with what's drawn
// on screen -- there's no way to derive the correct values without testing
// against the physical panel, so don't take these as verified.
static constexpr bool TOUCH_SWAP_XY = false;
static constexpr bool TOUCH_INVERT_X = false;
static constexpr bool TOUCH_INVERT_Y = false;

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

    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PR;
}

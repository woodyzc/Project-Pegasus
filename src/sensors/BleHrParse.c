#include "BleHrParse.h"

bool BLE_HR_ParseMeasurement(const uint8_t *data, size_t length, uint16_t *out_bpm) {
    if (data == NULL || out_bpm == NULL || length < 2) {
        return false;
    }

    /* Bit 0 of the flags byte selects the heart-rate value format. */
    const bool value_is_16bit = (data[0] & 0x01) != 0;

    uint16_t bpm;
    if (value_is_16bit) {
        if (length < 3) {
            return false; /* flags claim uint16, packet too short to hold one */
        }
        bpm = (uint16_t)(data[1] | ((uint16_t)data[2] << 8));
    } else {
        bpm = data[1];
    }

    /* 0 = no reading. >255 cannot be a real heart rate and would wrap
     * HeartRate_t::bpm, so reject rather than publish something bogus. */
    if (bpm == 0 || bpm > 255) {
        return false;
    }

    *out_bpm = bpm;
    return true;
}

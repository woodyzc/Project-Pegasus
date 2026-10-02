#include "Imu.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

#include "../system/Stillness.h"
#include "SensorBus.h"

namespace {

constexpr uint8_t ADDR_LOW = 0x6B;  // SDO low: this board
constexpr uint8_t ADDR_HIGH = 0x6A;
constexpr uint8_t WHO_AM_I_VALUE = 0x05;

// ---- Registers, from Waveshare's Gyro_QMI8658 ----
constexpr uint8_t REG_WHO_AM_I = 0x00;
constexpr uint8_t REG_CTRL1 = 0x02; // interface; bit6 auto-increment, bit0 osc
constexpr uint8_t REG_CTRL2 = 0x03; // accel: scale bits 4..6, ODR bits 0..3
constexpr uint8_t REG_CTRL3 = 0x04; // gyro: same layout
constexpr uint8_t REG_CTRL6 = 0x07; // AttitudeEngine
constexpr uint8_t REG_CTRL7 = 0x08; // enables
constexpr uint8_t REG_AX_L = 0x35;  // 0x35..0x40: ax, ay, az, gx, gy, gz

// Accel +/-4g at 120Hz; gyro +/-256dps at 120Hz. Neither range is close to
// being reached by a bicycle -- they are chosen for resolution near rest,
// which is the only place this detector looks.
constexpr uint8_t CTRL2_VALUE = (0x01 << 4) | 0x06;
constexpr uint8_t CTRL3_VALUE = (0x04 << 4) | 0x06;
constexpr uint8_t CTRL7_ENABLE = 0x43; // accel + gyro, high-speed internal clock
constexpr uint8_t CTRL1_AUTOINC = 0x40;
constexpr uint8_t CTRL1_OSC_ON = 0x01; // cleared to run the 2MHz oscillator

constexpr float ACCEL_G_PER_LSB = 4.0f / 32768.0f;
constexpr float GYRO_DPS_PER_LSB = 256.0f / 32768.0f;

// 20Hz. Far below the part's 120Hz output rate, which is deliberate: the
// detector needs a representative sample rather than every sample, and the
// bus is shared with the barometer.
constexpr uint32_t SAMPLE_MS = 50;

uint8_t s_addr = 0;
uint8_t s_who = 0;
bool s_found = false;

Stillness_t s_still;
volatile float s_accel_g = 0.0f;
volatile float s_gyro_dps = 0.0f;
volatile bool s_is_still = false;

int16_t Le16(const uint8_t *p) { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

void ImuTask(void *pv) {
    (void)pv;
    for (;;) {
        uint8_t buf[12];
        if (SensorBus_Read(s_addr, REG_AX_L, buf, sizeof(buf))) {
            const float ax = (float)Le16(buf + 0) * ACCEL_G_PER_LSB;
            const float ay = (float)Le16(buf + 2) * ACCEL_G_PER_LSB;
            const float az = (float)Le16(buf + 4) * ACCEL_G_PER_LSB;
            const float gx = (float)Le16(buf + 6) * GYRO_DPS_PER_LSB;
            const float gy = (float)Le16(buf + 8) * GYRO_DPS_PER_LSB;
            const float gz = (float)Le16(buf + 10) * GYRO_DPS_PER_LSB;

            // Magnitudes, so no axis is privileged and the head unit can be
            // clamped to the bars at any angle. See Stillness.h.
            const float amag = sqrtf(ax * ax + ay * ay + az * az);
            const float gmag = sqrtf(gx * gx + gy * gy + gz * gz);

            s_accel_g = amag;
            s_gyro_dps = gmag;
            s_is_still = Stillness_Feed(&s_still, amag, gmag, millis());
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

} // namespace

void Imu_Init() {
    SensorBus_Begin();
    Stillness_Reset(&s_still);

    if (SensorBus_Probe(ADDR_LOW)) {
        s_addr = ADDR_LOW;
    } else if (SensorBus_Probe(ADDR_HIGH)) {
        s_addr = ADDR_HIGH;
    } else {
        s_addr = 0;
        return;
    }

    // Checked, not assumed: 0x6B is an ordinary address other parts can use,
    // so an ACK alone does not mean an IMU.
    if (!SensorBus_Read(s_addr, REG_WHO_AM_I, &s_who, 1)) {
        s_addr = 0;
        return;
    }
    if (s_who != WHO_AM_I_VALUE) {
        return; // something is there; keep the id to show on the panel
    }

    uint8_t ctrl1 = 0;
    if (!SensorBus_Read(s_addr, REG_CTRL1, &ctrl1, 1)) {
        return;
    }
    // Read-modify-write, as the vendor does: CTRL1 also carries interface bits
    // this driver has no business clearing.
    ctrl1 = (uint8_t)((ctrl1 & (uint8_t)~CTRL1_OSC_ON) | CTRL1_AUTOINC);
    if (!SensorBus_Write(s_addr, REG_CTRL1, ctrl1)) {
        return;
    }

    if (!SensorBus_Write(s_addr, REG_CTRL2, CTRL2_VALUE)) {
        return;
    }
    if (!SensorBus_Write(s_addr, REG_CTRL3, CTRL3_VALUE)) {
        return;
    }
    // Enables last, so the configuration above is in force when it starts.
    if (!SensorBus_Write(s_addr, REG_CTRL7, CTRL7_ENABLE)) {
        return;
    }
    // AttitudeEngine off. It computes a fused orientation this firmware does
    // not use, and it changes what the data registers mean.
    if (!SensorBus_Write(s_addr, REG_CTRL6, 0x00)) {
        return;
    }

    s_found = true;
}

bool Imu_Found() { return s_found; }
uint8_t Imu_Address() { return s_addr; }
uint8_t Imu_WhoAmI() { return s_who; }
bool Imu_IsStill() { return s_is_still; }
float Imu_AccelG() { return s_accel_g; }
float Imu_GyroDps() { return s_gyro_dps; }

void Imu_StartMonitor() {
    if (!s_found) {
        return;
    }
    xTaskCreatePinnedToCore(ImuTask, "imu", 3072, nullptr, 1, nullptr, 0);
}

#include <Arduino.h>
#include <cmath>
#include <cstdarg>
#include <TFT_eSPI.h>
#include <TinyGPSPlus.h>
#include <lvgl.h>

#ifndef GPS_RX_PIN
#define GPS_RX_PIN 18
#endif

#ifndef GPS_TX_PIN
#define GPS_TX_PIN 17
#endif

#ifndef GPS_BAUD
#define GPS_BAUD 9600
#endif

namespace {

constexpr uint16_t SCREEN_WIDTH = 240;
constexpr uint16_t SCREEN_HEIGHT = 320;
constexpr uint16_t DRAW_BUFFER_LINES = 20;
constexpr uint32_t UI_UPDATE_INTERVAL_MS = 250;

TFT_eSPI display;
HardwareSerial gpsSerial(1);
TinyGPSPlus gps;

lv_disp_draw_buf_t drawBuffer;
lv_color_t drawBufferPixels[SCREEN_WIDTH * DRAW_BUFFER_LINES];
lv_disp_drv_t displayDriver;

lv_obj_t *speedLabel;
lv_obj_t *tripLabel;
lv_obj_t *clockLabel;
lv_obj_t *inclineLabel;
lv_obj_t *heartRateLabel;
lv_obj_t *heartRateZoneLabel;
lv_obj_t *routeArrowLabel;
lv_obj_t *routeDirectionLabel;
double tripKilometres = 0.0;
bool hasPreviousPosition = false;
double previousLatitude = 0.0;
double previousLongitude = 0.0;
uint32_t lastUiUpdate = 0;
uint32_t lastLvTick = 0;
uint8_t routeIndex = 4;

void flushDisplay(lv_disp_drv_t *driver, const lv_area_t *area,
                  lv_color_t *colorMap) {
  const uint32_t width = static_cast<uint32_t>(area->x2 - area->x1 + 1);
  const uint32_t height = static_cast<uint32_t>(area->y2 - area->y1 + 1);

  display.startWrite();
  display.setAddrWindow(area->x1, area->y1, width, height);
  display.pushColors(reinterpret_cast<uint16_t *>(&colorMap->full),
                     width * height, true);
  display.endWrite();
  lv_disp_flush_ready(driver);
}

void setLabelText(lv_obj_t *label, const char *format, ...) {
  char text[32];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  lv_label_set_text(label, text);
}

void updateRouteDirection() {
  static const char *directionNames[] = {"LEFT", "RIGHT", "STRAIGHT",
                                         "SLIGHT LEFT", "SLIGHT RIGHT"};
  static const char *directionArrows[] = {"↰", "↱", "➤", "↖", "↗"};

  const uint8_t index = (millis() / 2000) % 5;
  routeIndex = index;
  lv_label_set_text(routeArrowLabel, directionArrows[index]);
  lv_label_set_text(routeDirectionLabel, directionNames[index]);
}

void updateHeartRateZone() {
  static constexpr uint8_t zone1Low = 90;
  static constexpr uint8_t zone3Low = 120;
  static constexpr uint8_t zone4Low = 150;

  uint32_t color = 0x7EF0A5;
  const char *zoneName = "Zone 1";

  if (heartRateLabel != nullptr) {
    const int bpm = atoi(lv_label_get_text(heartRateLabel));
    if (bpm >= zone4Low) {
      color = 0xFFD166;
      zoneName = "Zone 4";
    } else if (bpm >= zone3Low) {
      color = 0x7BC8FF;
      zoneName = "Zone 3";
    } else if (bpm >= zone1Low) {
      color = 0x7EF0A5;
      zoneName = "Zone 2";
    }
  }

  lv_label_set_text(heartRateZoneLabel, zoneName);
  lv_obj_set_style_bg_color(heartRateZoneLabel, lv_color_hex(color), 0);
  lv_obj_set_style_text_color(heartRateZoneLabel, lv_color_hex(0x081015), 0);
}

void createUi() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "PEGASUS");
  lv_obj_set_style_text_color(title, lv_color_hex(0x93A4B8), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

  speedLabel = lv_label_create(screen);
  lv_label_set_text(speedLabel, "--");
  lv_obj_set_style_text_color(speedLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(speedLabel, &lv_font_montserrat_48, 0);
  lv_obj_align(speedLabel, LV_ALIGN_TOP_MID, 0, 38);

  lv_obj_t *speedUnitLabel = lv_label_create(screen);
  lv_label_set_text(speedUnitLabel, "km/h");
  lv_obj_set_style_text_color(speedUnitLabel, lv_color_hex(0x61DAFB), 0);
  lv_obj_set_style_text_font(speedUnitLabel, &lv_font_montserrat_14, 0);
  lv_obj_align(speedUnitLabel, LV_ALIGN_TOP_MID, 0, 92);

  lv_obj_t *tripCaption = lv_label_create(screen);
  lv_label_set_text(tripCaption, "TRIP");
  lv_obj_set_style_text_color(tripCaption, lv_color_hex(0x93A4B8), 0);
  lv_obj_set_style_text_font(tripCaption, &lv_font_montserrat_12, 0);
  lv_obj_align(tripCaption, LV_ALIGN_TOP_LEFT, 18, 118);

  tripLabel = lv_label_create(screen);
  lv_label_set_text(tripLabel, "0.00 km");
  lv_obj_set_style_text_color(tripLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(tripLabel, &lv_font_montserrat_24, 0);
  lv_obj_align(tripLabel, LV_ALIGN_TOP_LEFT, 18, 138);

  lv_obj_t *clockCaption = lv_label_create(screen);
  lv_label_set_text(clockCaption, "BEIJING TIME");
  lv_obj_set_style_text_color(clockCaption, lv_color_hex(0x93A4B8), 0);
  lv_obj_set_style_text_font(clockCaption, &lv_font_montserrat_10, 0);
  lv_obj_align(clockCaption, LV_ALIGN_TOP_LEFT, 18, 196);

  clockLabel = lv_label_create(screen);
  lv_label_set_text(clockLabel, "--:--:--");
  lv_obj_set_style_text_color(clockLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(clockLabel, &lv_font_montserrat_18, 0);
  lv_obj_align(clockLabel, LV_ALIGN_TOP_LEFT, 18, 212);

  lv_obj_t *inclineCaption = lv_label_create(screen);
  lv_label_set_text(inclineCaption, "INCLINE");
  lv_obj_set_style_text_color(inclineCaption, lv_color_hex(0x93A4B8), 0);
  lv_obj_set_style_text_font(inclineCaption, &lv_font_montserrat_10, 0);
  lv_obj_align(inclineCaption, LV_ALIGN_TOP_MID, 0, 196);

  inclineLabel = lv_label_create(screen);
  lv_label_set_text(inclineLabel, "+0.0%");
  lv_obj_set_style_text_color(inclineLabel, lv_color_hex(0x61DAFB), 0);
  lv_obj_set_style_text_font(inclineLabel, &lv_font_montserrat_18, 0);
  lv_obj_align(inclineLabel, LV_ALIGN_TOP_MID, 0, 212);

  lv_obj_t *heartCaption = lv_label_create(screen);
  lv_label_set_text(heartCaption, "HEART RATE");
  lv_obj_set_style_text_color(heartCaption, lv_color_hex(0x93A4B8), 0);
  lv_obj_set_style_text_font(heartCaption, &lv_font_montserrat_10, 0);
  lv_obj_align(heartCaption, LV_ALIGN_TOP_RIGHT, -18, 196);

  heartRateLabel = lv_label_create(screen);
  lv_label_set_text(heartRateLabel, "120");
  lv_obj_set_style_text_color(heartRateLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(heartRateLabel, &lv_font_montserrat_18, 0);
  lv_obj_align(heartRateLabel, LV_ALIGN_TOP_RIGHT, -38, 212);

  lv_obj_t *heartUnit = lv_label_create(screen);
  lv_label_set_text(heartUnit, "bpm");
  lv_obj_set_style_text_color(heartUnit, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(heartUnit, &lv_font_montserrat_10, 0);
  lv_obj_align(heartUnit, LV_ALIGN_TOP_RIGHT, -12, 220);

  heartRateZoneLabel = lv_label_create(screen);
  lv_label_set_text(heartRateZoneLabel, "Zone 3");
  lv_obj_set_style_text_color(heartRateZoneLabel, lv_color_hex(0x081015), 0);
  lv_obj_set_style_bg_color(heartRateZoneLabel, lv_color_hex(0x7BC8FF), 0);
  lv_obj_set_style_bg_opa(heartRateZoneLabel, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(heartRateZoneLabel, 8, 0);
  lv_obj_set_style_pad_ver(heartRateZoneLabel, 2, 0);
  lv_obj_set_style_radius(heartRateZoneLabel, 8, 0);
  lv_obj_align(heartRateZoneLabel, LV_ALIGN_TOP_RIGHT, -18, 238);

  routeArrowLabel = lv_label_create(screen);
  lv_label_set_text(routeArrowLabel, "↗");
  lv_obj_set_style_text_color(routeArrowLabel, lv_color_hex(0x61DAFB), 0);
  lv_obj_set_style_text_font(routeArrowLabel, &lv_font_montserrat_28, 0);
  lv_obj_align(routeArrowLabel, LV_ALIGN_TOP_RIGHT, -18, 260);

  routeDirectionLabel = lv_label_create(screen);
  lv_label_set_text(routeDirectionLabel, "SLIGHT RIGHT");
  lv_obj_set_style_text_color(routeDirectionLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(routeDirectionLabel, &lv_font_montserrat_12, 0);
  lv_obj_align(routeDirectionLabel, LV_ALIGN_TOP_RIGHT, -18, 294);

  lv_obj_t *routeCaption = lv_label_create(screen);
  lv_label_set_text(routeCaption, "ROUTE");
  lv_obj_set_style_text_color(routeCaption, lv_color_hex(0x93A4B8), 0);
  lv_obj_set_style_text_font(routeCaption, &lv_font_montserrat_10, 0);
  lv_obj_align(routeCaption, LV_ALIGN_TOP_RIGHT, -18, 248);
}

void updateTrip() {
  if (!gps.location.isUpdated() || !gps.location.isValid()) {
    return;
  }

  const double latitude = gps.location.lat();
  const double longitude = gps.location.lng();
  if (hasPreviousPosition) {
    tripKilometres += TinyGPSPlus::distanceBetween(
                          previousLatitude, previousLongitude, latitude,
                          longitude) /
                      1000.0;
  }
  previousLatitude = latitude;
  previousLongitude = longitude;
  hasPreviousPosition = true;
}

void updateUi() {
  if (gps.speed.isValid()) {
    setLabelText(speedLabel, "%.1f", gps.speed.kmph());
  }
  setLabelText(tripLabel, "%.2f km", tripKilometres);

  if (gps.date.isValid() && gps.time.isValid()) {
    const uint8_t beijingHour = (gps.time.hour() + 8) % 24;
    setLabelText(clockLabel, "%02u:%02u:%02u", beijingHour, gps.time.minute(),
                 gps.time.second());
  }

  static float inclineValue = 2.5f;
  inclineValue = 2.5f + 3.5f * sin(millis() / 1800.0f);
  setLabelText(inclineLabel, "%+.1f%%", inclineValue);

  static int simulatedHeartRate = 120;
  simulatedHeartRate = 110 + (int)(22.0f * sin(millis() / 1200.0f)) +
                       (int)(3.0f * ((millis() / 700) % 5));
  setLabelText(heartRateLabel, "%d", simulatedHeartRate);
  updateHeartRateZone();
  updateRouteDirection();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  display.init();
  display.setRotation(0);
  display.fillScreen(TFT_BLACK);

  lv_init();
  lv_disp_draw_buf_init(&drawBuffer, drawBufferPixels, nullptr,
                        SCREEN_WIDTH * DRAW_BUFFER_LINES);
  lv_disp_drv_init(&displayDriver);
  displayDriver.hor_res = SCREEN_WIDTH;
  displayDriver.ver_res = SCREEN_HEIGHT;
  displayDriver.flush_cb = flushDisplay;
  displayDriver.draw_buf = &drawBuffer;
  lv_disp_drv_register(&displayDriver);
  createUi();
}

void loop() {
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  updateTrip();
  if (millis() - lastUiUpdate >= UI_UPDATE_INTERVAL_MS) {
    lastUiUpdate = millis();
    updateUi();
  }

  const uint32_t now = millis();
  lv_tick_inc(now - lastLvTick);
  lastLvTick = now;
  lv_timer_handler();
  delay(5);
}

# Renders the dashboard to images on this machine, using the real page code.
#
# Every layout bug in this project has been invisible until the firmware was
# flashed and the panel photographed. This compiles the actual
# src/ui/Page_Dashboard.cpp against the actual lv_conf.h and the actual
# generated fonts and arrows, so the same questions can be answered in a
# second instead of a flash cycle.
#
#   cd test/sim && make        # build, render, and convert to PNG
#   cd test/sim && make clean
#
# Not a behaviour test. The stubs return fixed values on purpose; behaviour
# lives in test/host, which compiles the real parsers and accumulators.

LVGL   := ../../.pio/libdeps/esp32s3/lvgl
REAL_CONF := ../../include/lv_conf.h
SRC    := ../../src
OUT    := out

CXXFLAGS := -std=c++17 -O1 -Wall -Wno-unused-parameter \
            -DLV_CONF_INCLUDE_SIMPLE -I. -I$(LVGL) -I$(LVGL)/src
CFLAGS   := -std=c11 -O1 -Wall -Wno-unused-parameter \
            -DLV_CONF_INCLUDE_SIMPLE -I. -I$(LVGL) -I$(LVGL)/src

# LVGL is built from source rather than linked: there is no prebuilt host
# library, and compiling it is what guarantees the simulator and the firmware
# are laying out against the same code.
LVGL_SRC := $(shell find $(LVGL)/src -name '*.c')

# The real page, the real icons, the real generated font, and the real modules
# it draws from. Anything not listed here is stubbed in stubs.cpp.
APP_CXX := $(SRC)/ui/Page_Dashboard.cpp $(SRC)/ui/TbtIcons.cpp
APP_C   := $(SRC)/ui/NumFont.c $(SRC)/system/HrZone.c $(SRC)/system/TimeZone.c \
           $(SRC)/system/TimeSource.c $(SRC)/navigation/TbtParse.c

SIM_CXX := sim_main.cpp stubs.cpp

.PHONY: all run png clean

all: run png

# The simulator's LVGL config is GENERATED from the firmware's, not copied.
#
# Only two settings differ, and both are about the host rather than the
# layout: the tick comes from the simulator instead of Arduino's millis(). Every
# LV_FONT_* switch is therefore identical by construction. A hand-maintained
# copy would drift the first time a font was enabled for the panel, and a
# simulator laying out with different fonts answers questions about a screen
# that does not exist.
lv_conf.h: $(REAL_CONF) Makefile
	@printf '/* GENERATED from %s by the Makefile -- do not edit. */\n' "$(REAL_CONF)" > $@
	@sed -e 's|#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"|#define LV_TICK_CUSTOM_INCLUDE "sim_tick.h"|' \
	     -e 's|#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())|#define LV_TICK_CUSTOM_SYS_TIME_EXPR (sim_millis())|' \
	     $(REAL_CONF) >> $@

# Compiled to objects rather than one command line: the C and C++ halves need
# different standards, and LVGL is nearly a thousand files that should not be
# rebuilt to re-render one scene.
LVGL_OBJ := $(patsubst $(LVGL)/%.c,$(OUT)/lvgl/%.o,$(LVGL_SRC))
APP_C_OBJ := $(patsubst $(SRC)/%.c,$(OUT)/app/%.o,$(APP_C))
APP_CXX_OBJ := $(patsubst $(SRC)/%.cpp,$(OUT)/app/%.o,$(APP_CXX))
SIM_OBJ := $(patsubst %.cpp,$(OUT)/simobj/%.o,$(SIM_CXX))

$(OUT)/lvgl/%.o: $(LVGL)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/app/%.o: $(SRC)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/app/%.o: $(SRC)/%.c lv_conf.hpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/simobj/%.o: %.cpp lv_conf.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/dashsim: $(LVGL_OBJ) $(APP_C_OBJ) $(APP_CXX_OBJ) $(SIM_OBJ)
	@mkdir -p $(OUT)
	$(CXX) -o $@ $(SIM_OBJ) $(APP_CXX_OBJ) $(APP_C_OBJ) $(LVGL_OBJ) -lm

run: $(OUT)/dashsim
	@mkdir -p $(OUT)
	./$(OUT)/dashsim $(OUT)

# sips ships with macOS, so no extra install for a viewable image.
#
# Also written at 3x, nearest-neighbour, because 240x320 is smaller than the
# panel is in the hand and judging whether two labels are too close together at
# that size is guesswork. The 1x files are the truth; the 3x ones are for
# looking at.
png:
	@for f in $(OUT)/*.ppm; do \
	    sips -s format png "$$f" --out "$${f%.ppm}.png" >/dev/null 2>&1 && rm -f "$$f"; \
	done
	@mkdir -p $(OUT)/3x
	@for f in $(OUT)/*.png; do \
	    sips -z 960 720 "$$f" --out "$(OUT)/3x/$$(basename $$f)" >/dev/null 2>&1; \
	done
	@ls $(OUT)/*.png 2>/dev/null || true

clean:
	rm -rf $(OUT) lv_conf.h

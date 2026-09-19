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

# -MMD -MP: every object also writes a .d listing the headers it included, and
# those are pulled in at the bottom. Without them a header change rebuilds
# nothing, and the link quietly mixes objects compiled against two different
# definitions of the same struct -- which is exactly what happened when
# MapView_t grew a field: MapView.o wrote past the end of a struct that
# Page_Map.o still thought was smaller, and the symptom was a garbage value
# rather than any kind of build error.
CXXFLAGS := -std=c++17 -O1 -Wall -Wno-unused-parameter -MMD -MP \
            -DLV_CONF_INCLUDE_SIMPLE -I. -I$(LVGL) -I$(LVGL)/src
CFLAGS   := -std=c11 -O1 -Wall -Wno-unused-parameter -MMD -MP \
            -DLV_CONF_INCLUDE_SIMPLE -I. -I$(LVGL) -I$(LVGL)/src

# LVGL is built from source rather than linked: there is no prebuilt host
# library, and compiling it is what guarantees the simulator and the firmware
# are laying out against the same code.
LVGL_SRC := $(shell find $(LVGL)/src -name '*.c')

# The real page, the real icons, the real generated font, and the real modules
# it draws from. Anything not listed here is stubbed in stubs.cpp.
APP_CXX := $(SRC)/ui/Page_Dashboard.cpp $(SRC)/ui/TbtIcons.cpp \
           $(SRC)/ui/Page_Map.cpp $(SRC)/ui/MapView.cpp \
           $(SRC)/ui/Overlay_RideSummary.cpp $(SRC)/ui/Overlay_Alert.cpp
APP_C   := $(SRC)/ui/NumFont.c $(SRC)/ui/NumFontBig.c $(SRC)/ui/NumFontAvg.c $(SRC)/ui/NumFontLive.c $(SRC)/system/HrZone.c $(SRC)/system/TimeZone.c \
           $(SRC)/system/RideSummary.c $(SRC)/navigation/MapHeading.c \
           $(SRC)/system/TimeSource.c $(SRC)/navigation/TbtParse.c \
           $(SRC)/navigation/MapProject.c $(SRC)/navigation/TrackBuffer.c \
           $(SRC)/system/AlertFrame.c $(SRC)/ui/CjkFont.c

SIM_CXX := sim_main.cpp stubs.cpp

.PHONY: all run png gallery fontproof clean

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

$(OUT)/app/%.o: $(SRC)/%.cpp lv_conf.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/simobj/%.o: %.cpp lv_conf.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/dashsim: $(LVGL_OBJ) $(APP_C_OBJ) $(APP_CXX_OBJ) $(SIM_OBJ)
	@mkdir -p $(OUT)
	$(CXX) -o $@ $(SIM_OBJ) $(APP_CXX_OBJ) $(APP_C_OBJ) $(LVGL_OBJ) -lm

# The CJK font, rendered through LVGL's real glyph blitter.
#
# gencjkfont.py packs those bitmaps by hand, and every way of getting that
# wrong still compiles and links. This is the cheapest place to find out --
# it needs no page, no stubs and no state, just the font and a screen.
$(OUT)/fontproof: $(LVGL_OBJ) $(OUT)/simobj/fontproof.o $(OUT)/app/ui/CjkFont.o
	@mkdir -p $(OUT)
	$(CXX) -o $@ $(OUT)/simobj/fontproof.o $(OUT)/app/ui/CjkFont.o $(LVGL_OBJ) -lm

fontproof: $(OUT)/fontproof
	@mkdir -p $(OUT)
	./$(OUT)/fontproof $(OUT)
	@sips -s format png -z 640 480 $(OUT)/cjk-font-proof.ppm \
	      --out $(OUT)/cjk-font-proof.png >/dev/null 2>&1 || true

run: $(OUT)/dashsim
	@mkdir -p $(OUT)
	./$(OUT)/dashsim $(OUT)
	@# The route page replaces the whole screen, so it renders in its own
	@# invocation. Included here rather than left to be remembered: the
	@# track-up frame was produced by hand once and would have rotted.
	./$(OUT)/dashsim $(OUT) --route
	@# The alert banner composites onto lv_layer_top(), so it needs the
	@# dashboard underneath it to show what it does and does not cover.
	./$(OUT)/dashsim $(OUT) --alert

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

# Every maneuver crossed with every distance and both unit settings, for the
# HTML viewer in test/sim/viewer. Frames only -- the viewer never lays anything
# out itself.
gallery: $(OUT)/dashsim
	@rm -rf $(OUT)/gallery && mkdir -p $(OUT)/gallery
	@./$(OUT)/dashsim $(OUT)/gallery --gallery >/dev/null
	@for f in $(OUT)/gallery/*.ppm; do \
	    sips -s format png "$$f" --out "$${f%.ppm}.png" >/dev/null 2>&1 && rm -f "$$f"; \
	done
	@echo "$$(ls $(OUT)/gallery/*.png | wc -l | tr -d ' ') frames in $(OUT)/gallery"

clean:
	rm -rf $(OUT) lv_conf.h

-include $(shell find $(OUT) -name '*.d' 2>/dev/null)

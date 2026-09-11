# Host-side tests: plain cc, no ESP32 toolchain, no hardware.
#
# Almost nothing in this firmware can be tested off-target, so the parts that
# can are deliberately kept dependency-free and covered here:
#   * test_parsebpm     - ANT+ HRM page decoding (src/sensors/SoftANT.cpp's
#                         ParseBPM logic, over deps/esp32-ant's decoder)
#   * test_ble_hr_parse - BLE 0x2A37 measurement parsing (compiles the real
#                         src/sensors/BleHrParse.c)
#   * test_tbt_parse    - turn-by-turn frame decoding (compiles the real
#                         src/navigation/TbtParse.c)
#   * test_ubx_parse    - u-blox NAV-PVT decoding (compiles the real
#                         src/sensors/UbxParse.c)
#   * test_timezone     - position to POSIX TZ, and the civil-to-epoch maths
#                         (compiles the real src/system/TimeZone.c)
#   * test_gpx_parse    - GPX track-point extraction (compiles the real
#                         src/navigation/GpxParse.c)
#   * test_track_buffer - breadcrumb thinning and bounds (compiles the real
#                         src/navigation/TrackBuffer.c)
#   * test_map_project  - breadcrumb projection geometry (compiles the real
#                         src/navigation/MapProject.c)
#   * test_gpx_write    - the GPX a ride is recorded into (compiles the real
#                         src/navigation/GpxWrite.c, and reads its output back
#                         through GpxParse.c)
#   * test_trip_accum   - which GPS steps count towards trip distance
#                         (compiles the real src/system/TripAccum.c)
#   * test_hr_zone      - heart-rate training zones by reserve, checked
#                         against the rider's own phone (compiles the real
#                         src/system/HrZone.c)
#
#   cd test/host && make          # build + run everything
#   cd test/host && make asan     # same under ASan/UBSan
#
# See also deps/esp32-ant/test/host (the library's own 100-test suite).

ANT     := ../../deps/esp32-ant/components/ant
SYSTEM  := ../../src/system
SENSORS := ../../src/sensors
NAV     := ../../src/navigation

CFLAGS ?= -std=c11 -Wall -Wextra
SUFFIX ?=

BINS := test_parsebpm$(SUFFIX) test_ble_hr_parse$(SUFFIX) test_tbt_parse$(SUFFIX) \
        test_ubx_parse$(SUFFIX) test_timezone$(SUFFIX) test_gpx_parse$(SUFFIX) \
        test_track_buffer$(SUFFIX) test_map_project$(SUFFIX) test_gpx_write$(SUFFIX) \
        test_trip_accum$(SUFFIX) test_hr_zone$(SUFFIX)

.PHONY: all run asan clean

all: run

run: $(BINS)
	@for t in $(BINS); do echo "== $$t =="; ./$$t || exit 1; echo; done

test_parsebpm$(SUFFIX): test_parsebpm.c $(ANT)/src/antplus_profiles.c
	$(CC) $(CFLAGS) -I$(ANT)/include -o $@ $^

test_ble_hr_parse$(SUFFIX): test_ble_hr_parse.c $(SENSORS)/BleHrParse.c
	$(CC) $(CFLAGS) -I$(SENSORS) -o $@ $^

test_tbt_parse$(SUFFIX): test_tbt_parse.c $(NAV)/TbtParse.c
	$(CC) $(CFLAGS) -I$(NAV) -o $@ $^

test_ubx_parse$(SUFFIX): test_ubx_parse.c $(SENSORS)/UbxParse.c
	$(CC) $(CFLAGS) -I$(SENSORS) -o $@ $^ -lm

test_timezone$(SUFFIX): test_timezone.c $(SYSTEM)/TimeZone.c
	$(CC) $(CFLAGS) -I$(SYSTEM) -o $@ $^

test_gpx_parse$(SUFFIX): test_gpx_parse.c $(NAV)/GpxParse.c
	$(CC) $(CFLAGS) -I$(NAV) -o $@ $^ -lm

test_track_buffer$(SUFFIX): test_track_buffer.c $(NAV)/TrackBuffer.c
	$(CC) $(CFLAGS) -I$(NAV) -o $@ $^ -lm

test_map_project$(SUFFIX): test_map_project.c $(NAV)/MapProject.c $(NAV)/TrackBuffer.c
	$(CC) $(CFLAGS) -I$(NAV) -o $@ $^ -lm

# Compiles GpxParse.c alongside the writer so the round-trip test reads back
# what it wrote using the same parser that loads route files from the card.
test_gpx_write$(SUFFIX): test_gpx_write.c $(NAV)/GpxWrite.c $(NAV)/GpxParse.c
	$(CC) $(CFLAGS) -I$(NAV) -o $@ $^ -lm

test_trip_accum$(SUFFIX): test_trip_accum.c $(SYSTEM)/TripAccum.c
	$(CC) $(CFLAGS) -I$(SYSTEM) -o $@ $^ -lm

test_hr_zone$(SUFFIX): test_hr_zone.c $(SYSTEM)/HrZone.c
	$(CC) $(CFLAGS) -I$(SYSTEM) -o $@ $^ -lm

# Both suites under the sanitizers.
asan:
	$(MAKE) CC="$(CC) -fsanitize=address,undefined" SUFFIX=_asan

clean:
	rm -f $(BINS) $(BINS:%=%_asan) test_*_asan

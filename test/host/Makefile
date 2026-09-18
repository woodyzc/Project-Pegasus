# Host-side tests: plain cc, no ESP32 toolchain, no hardware.
#
# Almost nothing in this firmware can be tested off-target, so the parts that
# can are deliberately kept dependency-free and covered here:
#   * test_ble_hr_parse - BLE 0x2A37 measurement parsing (compiles the real
#                         src/sensors/BleHrParse.c)
#   * test_gps_frame    - the position fix the phone sends, standing in for a
#                         GNSS module that has never been fitted (compiles the
#                         real src/sensors/GpsFrame.c)
#   * test_alert_frame  - the call/text/chat alert the phone pushes (compiles
#                         the real src/system/AlertFrame.c). The name in it is
#                         the only off-device string this firmware draws
#                         verbatim, so the length handling is the subject.
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
#   * test_clock_frame  - the clock the phone writes over BLE (compiles the
#                         real src/system/ClockFrame.c). Worth its own suite:
#                         a wrong clock is written to the card permanently, in
#                         ride filenames and in every trackpoint stamp.
#   * test_time_source  - which clock the head unit believes, and when it stops
#                         believing it (compiles the real
#                         src/system/TimeSource.c)
#   * test_map_heading  - which way is up on a track-up map (compiles the real
#                         src/navigation/MapHeading.c)
#   * test_ride_summary - what a ride amounted to, and the three shapes its
#                         duration takes (compiles the real
#                         src/system/RideSummary.c)
#   * test_ascent       - total climbing from noisy GNSS altitude (compiles the
#                         real src/system/Ascent.c)
#   * test_idle_policy  - when the screen dims, blanks and sleeps (compiles the
#                         real src/system/IdlePolicy.c)
#   * test_file_path    - which paths the WiFi file server may touch (compiles
#                         the real src/system/FilePath.c). The only host suite
#                         covering an adversarial input rather than a mistake.
#   * test_ride_stats   - ride averages and maxima: moving average, the stop
#                         that must not decay it, and the bad fix that must
#                         not own the max (compiles the real
#                         src/system/RideStatsCore.c)
#   * test_route_parse  - the BLE route-download protocol and the offline
#                         route-following geometry (compiles the real
#                         src/navigation/RouteParse.c and RouteFollow.c).
#                         Carries more weight than most: the offline half of
#                         turn-by-turn cannot be exercised on the bench,
#                         because no GPS module has ever been attached.
#
#   cd test/host && make          # build + run everything
#   cd test/host && make asan     # same under ASan/UBSan


SYSTEM  := ../../src/system
SENSORS := ../../src/sensors
NAV     := ../../src/navigation

CFLAGS ?= -std=c11 -Wall -Wextra
SUFFIX ?=

BINS := test_ble_hr_parse$(SUFFIX) test_tbt_parse$(SUFFIX) test_ubx_parse$(SUFFIX) \
        test_timezone$(SUFFIX) test_gpx_parse$(SUFFIX) test_track_buffer$(SUFFIX) \
        test_map_project$(SUFFIX) test_gpx_write$(SUFFIX) test_trip_accum$(SUFFIX) \
        test_hr_zone$(SUFFIX) test_route_parse$(SUFFIX) test_clock_frame$(SUFFIX) \
        test_time_source$(SUFFIX) test_ride_stats$(SUFFIX) test_file_path$(SUFFIX) \
        test_idle_policy$(SUFFIX) test_ascent$(SUFFIX) \
        test_ride_summary$(SUFFIX) test_map_heading$(SUFFIX) \
        test_gps_frame$(SUFFIX) test_alert_frame$(SUFFIX)

.PHONY: all run asan clean

all: run

run: $(BINS)
	@for t in $(BINS); do echo "== $$t =="; ./$$t || exit 1; echo; done

# Every binary is built from its sources in one step, so there are no object
# files for `cc -MMD` to hang dependencies off. Depend on all the headers
# instead: a header edit then rebuilds everything, which costs about a second
# for fifteen small binaries and removes the failure mode that cost real time
# three times over -- a changed constant in a .h, a `make` that reports PASS,
# and a suite that never recompiled against it.
HEADERS := $(wildcard $(SYSTEM)/*.h) $(wildcard $(SENSORS)/*.h) \
           $(wildcard $(NAV)/*.h)

$(BINS): $(HEADERS)

# The headers are prerequisites but not inputs to the compiler, so every recipe
# passes $(CSRC) rather than $^ -- clang refuses `-o` when handed a header.
CSRC = $(filter %.c,$^)

test_ble_hr_parse$(SUFFIX): test_ble_hr_parse.c $(SENSORS)/BleHrParse.c
	$(CC) $(CFLAGS) -I$(SENSORS) -o $@ $(CSRC)

test_gps_frame$(SUFFIX): test_gps_frame.c $(SENSORS)/GpsFrame.c
	$(CC) $(CFLAGS) -I$(SENSORS) -o $@ $(CSRC) -lm

# The name in this frame is printed straight onto the panel, so the suite is
# mostly about lengths: the declared one and the delivered one are separate
# numbers, and believing the first reads past the BLE buffer.
test_alert_frame$(SUFFIX): test_alert_frame.c $(SYSTEM)/AlertFrame.c
	$(CC) $(CFLAGS) -I$(SYSTEM) -o $@ $(CSRC)

test_tbt_parse$(SUFFIX): test_tbt_parse.c $(NAV)/TbtParse.c
	$(CC) $(CFLAGS) -I$(NAV) -o $@ $(CSRC)

test_ubx_parse$(SUFFIX): test_ubx_parse.c $(SENSORS)/UbxParse.c
	$(CC) $(CFLAGS) -I$(SENSORS) -o $@ $(CSRC) -lm

test_timezone$(SUFFIX): test_timezone.c $(SYSTEM)/TimeZone.c
	$(CC) $(CFLAGS) -I$(SYSTEM) -o $@ $(CSRC)

test_clock_frame$(SUFFIX): test_clock_frame.c $(SYSTEM)/ClockFrame.c
	$(CC) $(CFLAGS) -I$(SYSTEM) -o $@ $(CSRC)

test_time_source$(SUFFIX): test_time_source.c $(SYSTEM)/TimeSource.c
	$(CC) $(CFLAGS) -I$(SYSTEM) -o $@ $(CSRC)

test_ride_stats$(SUFFIX): test_ride_stats.c $(SYSTEM)/RideStatsCore.c
	$(CC) $(CFLAGS) -I$(SYSTEM) -o $@ $(CSRC) -lm

# Carries more weight than its size suggests: this is the only input in the
# firmware that arrives from outside the device and reaches the filesystem.
test_file_path$(SUFFIX): test_file_path.c $(SYSTEM)/FilePath.c
	$(CC) $(CFLAGS) -I$(SYSTEM) -o $@ $(CSRC)

# Thresholds here are minutes long, so the alternative to testing this is
# waiting -- and the failure modes are a device that dies at lunchtime or one
# that blanks while the rider is reading it.
test_idle_policy$(SUFFIX): test_idle_policy.c $(SYSTEM)/IdlePolicy.c
	$(CC) $(CFLAGS) -I$(SYSTEM) -o $@ $(CSRC)

# The headline test is that an hour of flat riding climbs nothing: a sum of
# positive altitude differences would report several hundred metres, and it
# would look plausible.
test_ascent$(SUFFIX): test_ascent.c $(SYSTEM)/Ascent.c
	$(CC) $(CFLAGS) -I$(SYSTEM) -o $@ $(CSRC) -lm

test_ride_summary$(SUFFIX): test_ride_summary.c $(SYSTEM)/RideSummary.c
	$(CC) $(CFLAGS) -I$(SYSTEM) -o $@ $(CSRC)

# The wrap at north is the case: a smoother written the obvious way takes 350
# towards 10 through 180, spinning the whole map a half-turn.
test_map_heading$(SUFFIX): test_map_heading.c $(NAV)/MapHeading.c
	$(CC) $(CFLAGS) -I$(NAV) -o $@ $(CSRC) -lm

test_gpx_parse$(SUFFIX): test_gpx_parse.c $(NAV)/GpxParse.c
	$(CC) $(CFLAGS) -I$(NAV) -o $@ $(CSRC) -lm

test_track_buffer$(SUFFIX): test_track_buffer.c $(NAV)/TrackBuffer.c
	$(CC) $(CFLAGS) -I$(NAV) -o $@ $(CSRC) -lm

test_map_project$(SUFFIX): test_map_project.c $(NAV)/MapProject.c $(NAV)/TrackBuffer.c
	$(CC) $(CFLAGS) -I$(NAV) -o $@ $(CSRC) -lm

# Compiles GpxParse.c alongside the writer so the round-trip test reads back
# what it wrote using the same parser that loads route files from the card.
test_gpx_write$(SUFFIX): test_gpx_write.c $(NAV)/GpxWrite.c $(NAV)/GpxParse.c
	$(CC) $(CFLAGS) -I$(NAV) -o $@ $(CSRC) -lm

test_trip_accum$(SUFFIX): test_trip_accum.c $(SYSTEM)/TripAccum.c
	$(CC) $(CFLAGS) -I$(SYSTEM) -o $@ $(CSRC) -lm

test_hr_zone$(SUFFIX): test_hr_zone.c $(SYSTEM)/HrZone.c
	$(CC) $(CFLAGS) -I$(SYSTEM) -o $@ $(CSRC) -lm

# The route download protocol and the route-following geometry together. They
# are tested as one because the geometry only ever runs on a blob the protocol
# assembled, and a test that built its own blob by hand would stop proving the
# two agree.
test_route_parse$(SUFFIX): test_route_parse.c $(NAV)/RouteParse.c $(NAV)/RouteFollow.c
	$(CC) $(CFLAGS) -I$(NAV) -o $@ $(CSRC) -lm

# Every suite under the sanitizers.
asan:
	$(MAKE) CC="$(CC) -fsanitize=address,undefined" SUFFIX=_asan

clean:
	rm -f $(BINS) $(BINS:%=%_asan) test_*_asan

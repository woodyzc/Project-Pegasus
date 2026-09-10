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
#
#   cd test/host && make          # build + run everything
#   cd test/host && make asan     # same under ASan/UBSan
#
# See also deps/esp32-ant/test/host (the library's own 100-test suite).

ANT     := ../../deps/esp32-ant/components/ant
SENSORS := ../../src/sensors
NAV     := ../../src/navigation

CFLAGS ?= -std=c11 -Wall -Wextra
SUFFIX ?=

BINS := test_parsebpm$(SUFFIX) test_ble_hr_parse$(SUFFIX) test_tbt_parse$(SUFFIX)

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

# Both suites under the sanitizers.
asan:
	$(MAKE) CC="$(CC) -fsanitize=address,undefined" SUFFIX=_asan

clean:
	rm -f test_parsebpm test_ble_hr_parse test_tbt_parse \
	      test_parsebpm_asan test_ble_hr_parse_asan test_tbt_parse_asan

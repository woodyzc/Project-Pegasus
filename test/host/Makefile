# Host-side tests: plain cc, no ESP32 toolchain, no hardware.
#
# Almost nothing in this firmware can be tested off-target, but the ANT+ page
# decoding in src/sensors/SoftANT.cpp is pure logic over the vendored
# deps/esp32-ant decoder, so it can be. Worth keeping runnable.
#
#   cd test/host && make
#
# See also deps/esp32-ant/test/host (the library's own 100-test suite), which
# runs the same way.

ANT := ../../deps/esp32-ant/components/ant
CFLAGS ?= -std=c11 -Wall -Wextra -I$(ANT)/include
BIN ?= test_parsebpm

.PHONY: all clean asan

all: $(BIN)
	./$(BIN)

$(BIN): test_parsebpm.c $(ANT)/src/antplus_profiles.c
	$(CC) $(CFLAGS) -o $@ $^

# Same suite under the sanitizers.
asan:
	$(MAKE) CC="$(CC) -fsanitize=address,undefined" BIN=test_parsebpm_asan

clean:
	rm -f test_parsebpm test_parsebpm_asan

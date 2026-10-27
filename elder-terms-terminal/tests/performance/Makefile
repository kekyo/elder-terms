.PHONY: all test clean

CC ?= gcc
CFLAGS ?= -O2 -g
CPPFLAGS ?=

BUILD_DIR ?= ../../../.build/latency-performance

all: $(BUILD_DIR)/latency-peer $(BUILD_DIR)/latency-input

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/latency-peer: latency-peer.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror $< -o $@

$(BUILD_DIR)/latency-input: latency-input.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror $< $(shell pkg-config --cflags --libs x11 xtst) -o $@

test: all
	node test-latency-helpers.mjs $(abspath $(BUILD_DIR)/latency-peer)

.PHONY: test-gui
test-gui: all
	xvfb-run -a --server-args='-screen 0 1600x900x24' dbus-run-session -- node test-latency-gui.mjs $(abspath $(BUILD_DIR)/latency-peer) $(abspath $(BUILD_DIR)/latency-input) $(abspath ../../../.build/elder-terms-terminal/boxrig-driver)

.PHONY: test-gui-full
test-gui-full: all
	xvfb-run -a --server-args='-screen 0 1600x900x24' dbus-run-session -- node test-latency-gui.mjs $(abspath $(BUILD_DIR)/latency-peer) $(abspath $(BUILD_DIR)/latency-input) $(abspath ../../../.build/elder-terms-terminal/boxrig-driver) full

clean:
	rm -f $(BUILD_DIR)/latency-peer $(BUILD_DIR)/latency-input

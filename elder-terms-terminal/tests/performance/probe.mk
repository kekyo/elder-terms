ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
BUILD := $(ROOT)/.build/latency
SDK := $(ROOT)/.build/boxrig/sdk
CC ?= gcc

.PHONY: all test test-annotations
all: $(BUILD)/latency-probe.so $(BUILD)/annotation-cost

$(BUILD):
	mkdir -p $@

$(BUILD)/latency-probe.so: $(ROOT)/elder-terms-terminal/tests/performance/latency-probe.c | $(BUILD)
	$(CC) -std=c99 -O2 -g -Wall -Wextra -Wpedantic -Werror -fPIC -shared \
	  -I$(SDK)/include $(shell pkg-config --cflags gtk+-3.0) $< -o $@ \
	  -L$(SDK)/lib -Wl,-rpath,$(SDK)/lib -lboxrig-gtk3 -lboxrig-ui -lboxrig-session -lboxrig \
	  $(shell pkg-config --libs gtk+-3.0) -ldl

test: all
	bash $(ROOT)/elder-terms-terminal/tests/performance/test-latency-probe.sh

$(BUILD)/annotation-cost: $(ROOT)/elder-terms-terminal/tests/performance/annotation-cost.c | $(BUILD)
	$(CC) -std=c99 -O2 -g -Wall -Wextra -Wpedantic -Werror -I$(SDK)/include $< -o $@ \
	  -L$(SDK)/lib -Wl,-rpath,$(SDK)/lib -lboxrig-ui -lboxrig

test-annotations: $(BUILD)/annotation-cost
	node $(ROOT)/elder-terms-terminal/tests/performance/test-annotation-cost.mjs $<

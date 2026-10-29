ROOT := $(abspath ../../..)
SMB2_PREFIX ?= $(ROOT)/.build/plan12-smb-sdk
BUILD_DIR ?= $(ROOT)/.build/smb-probe
SMB2_SOURCE := $(ROOT)/.build/plan12-libsmb2-fixed-source
SMB2_BUILD := $(ROOT)/.build/plan12-libsmb2-fixed-build
SMB2_REVISION := 727feaaa6d0f4fed1bae57745fbc2c5db9a30e64
PKG_CONFIG_PATH := $(SMB2_PREFIX)/lib/pkgconfig:$(PKG_CONFIG_PATH)
export PKG_CONFIG_PATH
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Werror -g -pthread -DCARDIO_WITH_GLIB=1 -DCARDIO_WITH_GIO=1 -I$(ROOT)/deps/cardio/include -isystem $(SMB2_PREFIX)/include
SMB_FLAGS = $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags --libs libsmb2 gio-2.0)
.PHONY: build deps test
build: $(BUILD_DIR)/smb-probe
deps:
	@test -d $(SMB2_SOURCE) || { git init $(SMB2_SOURCE) && git -C $(SMB2_SOURCE) fetch --depth 1 https://github.com/sahlberg/libsmb2.git $(SMB2_REVISION) && git -C $(SMB2_SOURCE) checkout --detach FETCH_HEAD; }
	@test "$$(git -C $(SMB2_SOURCE) rev-parse HEAD)" = "$(SMB2_REVISION)"
	cmake -S $(SMB2_SOURCE) -B $(SMB2_BUILD) -DCMAKE_INSTALL_PREFIX=$(SMB2_PREFIX) -DENABLE_LIBKRB5=OFF -DCMAKE_BUILD_TYPE=Release
	cmake --build $(SMB2_BUILD) --parallel 4
	cmake --install $(SMB2_BUILD)
test: build
	node run-samba.mts
$(BUILD_DIR)/smb-probe: smb-probe.cpp Makefile
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(SMB_FLAGS) -Wl,-rpath,$(SMB2_PREFIX)/lib

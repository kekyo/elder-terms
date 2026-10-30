ROOT := $(abspath ../../..)
SMB2_PREFIX ?= $(ROOT)/.build/plan12-smb-sdk
BUILD_DIR ?= $(ROOT)/.build/smb-probe
SMB2_SOURCE := $(ROOT)/.build/plan12-libsmb2-fixed-source
SMB2_BUILD := $(ROOT)/.build/plan12-libsmb2-fixed-build
SMB2_REVISION := 727feaaa6d0f4fed1bae57745fbc2c5db9a30e64
SANITIZE ?= 0
PKG_CONFIG_PATH := $(SMB2_PREFIX)/lib/pkgconfig:$(PKG_CONFIG_PATH)
export PKG_CONFIG_PATH
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Werror -g -pthread -DCARDIO_WITH_GLIB=1 -DCARDIO_WITH_GIO=1 -I$(ROOT)/deps/cardio/include -isystem $(SMB2_PREFIX)/include
ifeq ($(SANITIZE),1)
BUILD_DIR := $(BUILD_DIR)-asan
CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
endif
CLIENT_SOURCE := $(ROOT)/elder-terms-terminal/src/smb/smb-client.cpp
CLIENT_OBJECTS = $(BUILD_DIR)/smb-client.o $(BUILD_DIR)/smb-share-list.o
ENGINE_SOURCE := $(ROOT)/elder-terms-terminal/src/file-transfer/file-transfer-engine.cpp
SMB_FLAGS = $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags --libs libsmb2 gio-2.0)
.PHONY: build deps test test-client
build: $(BUILD_DIR)/smb-probe
deps:
	@test -d $(SMB2_SOURCE) || { git init $(SMB2_SOURCE) && git -C $(SMB2_SOURCE) fetch --depth 1 https://github.com/sahlberg/libsmb2.git $(SMB2_REVISION) && git -C $(SMB2_SOURCE) checkout --detach FETCH_HEAD; }
	@test "$$(git -C $(SMB2_SOURCE) rev-parse HEAD)" = "$(SMB2_REVISION)"
	cmake -S $(SMB2_SOURCE) -B $(SMB2_BUILD) -DCMAKE_INSTALL_PREFIX=$(SMB2_PREFIX) -DENABLE_LIBKRB5=OFF -DCMAKE_BUILD_TYPE=Release
	cmake --build $(SMB2_BUILD) --parallel 4
	cmake --install $(SMB2_BUILD)
test: build
	node run-samba.mts
test-client: $(BUILD_DIR)/smb-client-test $(BUILD_DIR)/smb-transfer-test $(BUILD_DIR)/smb-fault-test
	node run-client.mts $(BUILD_DIR)/smb-client-test $(BUILD_DIR)/smb-fault-test
	node run-client.mts $(BUILD_DIR)/smb-transfer-test
$(BUILD_DIR)/smb-probe: smb-probe.cpp Makefile
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(SMB_FLAGS) -Wl,-rpath,$(SMB2_PREFIX)/lib
$(BUILD_DIR)/smb-client.o: $(CLIENT_SOURCE) $(ROOT)/elder-terms-terminal/src/smb/smb-client.h Makefile
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(ROOT)/shared/include $(SMB_FLAGS) -c $< -o $@
$(BUILD_DIR)/file-transfer-engine.o: $(ENGINE_SOURCE) Makefile
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(ROOT)/shared/include $(SMB_FLAGS) -c $< -o $@
$(BUILD_DIR)/smb-share-list.o: $(ROOT)/elder-terms-terminal/src/smb/smb-share-list.cpp $(ROOT)/elder-terms-terminal/src/smb/smb-share-list.h Makefile
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(ROOT)/shared/include $(SMB_FLAGS) -c $< -o $@
$(BUILD_DIR)/smb-client-test: smb-client-test.cpp $(CLIENT_OBJECTS) Makefile
	$(CXX) $(CXXFLAGS) -I$(ROOT)/shared/include $< $(CLIENT_OBJECTS) -o $@ $(SMB_FLAGS) -Wl,-rpath,$(SMB2_PREFIX)/lib
$(BUILD_DIR)/smb-%-test: smb-%-test.cpp $(CLIENT_OBJECTS) $(BUILD_DIR)/file-transfer-engine.o Makefile
	$(CXX) $(CXXFLAGS) -I$(ROOT)/shared/include $< $(CLIENT_OBJECTS) $(BUILD_DIR)/file-transfer-engine.o -o $@ $(SMB_FLAGS) -Wl,-rpath,$(SMB2_PREFIX)/lib

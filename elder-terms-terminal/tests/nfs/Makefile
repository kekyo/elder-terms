ROOT := $(abspath ../../..)
BUILD_DIR ?= $(ROOT)/.build/nfs
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Werror -g -pthread -DCARDIO_WITH_GLIB=1 -DCARDIO_WITH_GIO=1 -I$(ROOT)/deps/cardio/include
SANITIZE ?= 0
ifeq ($(SANITIZE),1)
BUILD_DIR := $(BUILD_DIR)-asan
CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
endif
GLIB_FLAGS := $(shell pkg-config --cflags --libs gio-2.0)
NFS_FLAGS = $(shell pkg-config --cflags --libs libnfs)
SOURCE := $(ROOT)/elder-terms-terminal/src/nfs
.PHONY: build test deps
build: $(BUILD_DIR)/nfs-name-test

deps:
	pkg-config --atleast-version=5.0.2 libnfs
	pkg-config --modversion libnfs

test: build
	$(BUILD_DIR)/nfs-name-test
	ELDER_NFS_PROBE=$(BUILD_DIR)/nfs-probe node run-faults.mts
	ELDER_NFS_PROBE=$(BUILD_DIR)/nfs-probe node run-vm.mts

$(BUILD_DIR)/nfs-name-test: nfs-name-test.cpp $(SOURCE)/nfs-name.cpp $(SOURCE)/nfs-name.h Makefile
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) nfs-name-test.cpp $(SOURCE)/nfs-name.cpp -o $@ $(GLIB_FLAGS)

build: $(BUILD_DIR)/nfs-probe
$(BUILD_DIR)/nfs-probe: nfs-probe.cpp $(SOURCE)/nfs-rpc.cpp $(SOURCE)/nfs-rpc.h $(SOURCE)/nfs-api.h $(SOURCE)/nfs-name.cpp $(SOURCE)/nfs-name.h Makefile | deps
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) nfs-probe.cpp $(SOURCE)/nfs-rpc.cpp $(SOURCE)/nfs-name.cpp -o $@ $(GLIB_FLAGS) $(NFS_FLAGS)

build: $(BUILD_DIR)/nfs-client-test
$(BUILD_DIR)/nfs-client-test: nfs-client-test.cpp $(SOURCE)/nfs-client.cpp $(SOURCE)/nfs-client.h $(SOURCE)/nfs-rpc.cpp $(SOURCE)/nfs-rpc.h $(SOURCE)/nfs-name.cpp $(SOURCE)/nfs-name.h $(SOURCE)/nfs-api.h ../../src/file-transfer/file-transfer-engine.cpp ../../src/file-transfer/remote-file-client.h ../../src/file-transfer/activity-file-client.cpp Makefile | deps
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) nfs-client-test.cpp $(SOURCE)/nfs-client.cpp $(SOURCE)/nfs-rpc.cpp $(SOURCE)/nfs-name.cpp ../../src/file-transfer/file-transfer-engine.cpp ../../src/file-transfer/activity-file-client.cpp -o $@ $(GLIB_FLAGS) $(NFS_FLAGS)

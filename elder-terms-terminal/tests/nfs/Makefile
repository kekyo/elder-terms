ROOT := $(abspath ../../..)
BUILD_DIR ?= $(ROOT)/.build/nfs
NFS_SDK := $(ROOT)/.build/libnfs/sdk
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Werror -g -pthread -DCARDIO_WITH_GLIB=1 -DCARDIO_WITH_GIO=1 -I$(ROOT)/deps/cardio/include -isystem $(NFS_SDK)/include
SANITIZE ?= 0
ifeq ($(SANITIZE),1)
BUILD_DIR := $(BUILD_DIR)-asan
CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
endif
GLIB_FLAGS := $(shell pkg-config --cflags --libs gio-2.0)
NFS_FLAGS := -L$(NFS_SDK)/lib -lnfs -Wl,-rpath,$(NFS_SDK)/lib
SOURCE := $(ROOT)/elder-terms-terminal/src/nfs
.PHONY: build test deps
build: $(BUILD_DIR)/nfs-name-test

deps:
	# rpcgen's unused buffer variable is harmless; only this external build suppresses it.
	cmake -S $(ROOT)/deps/libnfs -B $(ROOT)/.build/libnfs/build -DCMAKE_INSTALL_PREFIX=$(NFS_SDK) -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-Wno-unused-but-set-variable -DENABLE_NFS4_2=OFF -DENABLE_TLS=OFF -DENABLE_KRB5=OFF -DENABLE_RDMA=OFF -DENABLE_MULTITHREADING=OFF
	cmake --build $(ROOT)/.build/libnfs/build --parallel 8
	cmake --install $(ROOT)/.build/libnfs/build

test: build
	$(BUILD_DIR)/nfs-name-test
	ELDER_NFS_PROBE=$(BUILD_DIR)/nfs-probe node run-faults.mts
	ELDER_NFS_PROBE=$(BUILD_DIR)/nfs-probe node run-vm.mts

$(BUILD_DIR)/nfs-name-test: nfs-name-test.cpp $(SOURCE)/nfs-name.cpp $(SOURCE)/nfs-name.h Makefile
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) nfs-name-test.cpp $(SOURCE)/nfs-name.cpp -o $@ $(GLIB_FLAGS)

build: $(BUILD_DIR)/nfs-probe
$(BUILD_DIR)/nfs-probe: nfs-probe.cpp $(SOURCE)/nfs-rpc.cpp $(SOURCE)/nfs-rpc.h $(SOURCE)/nfs-name.cpp $(SOURCE)/nfs-name.h Makefile
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) nfs-probe.cpp $(SOURCE)/nfs-rpc.cpp $(SOURCE)/nfs-name.cpp -o $@ $(GLIB_FLAGS) $(NFS_FLAGS)

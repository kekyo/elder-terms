# Point SDK at an unpacked Linux release of boxrig 0.16.0.
SDK ?= ../../../.build/dock-pilot-sdk
SDK_LIB := $(abspath $(SDK))/lib
PKGS := boxrig-gtk3 boxrig-posix boxrig-session
PC := PKG_CONFIG_PATH="$(SDK_LIB)/pkgconfig:$(PKG_CONFIG_PATH)" pkg-config
CFLAGS += -std=c99 -O2 -g -Wall -Wextra -Werror

build/dock-pilot: pilot.c session.c session.h tearoff.c tearoff.h
	$(PC) --exact-version=0.16.0 boxrig-gtk3
	mkdir -p $(@D)
	$(CC) $(CFLAGS) $$($(PC) --cflags $(PKGS)) pilot.c session.c tearoff.c -o $@ \
	  $$($(PC) --libs $(PKGS)) -Wl,-rpath,"$(SDK_LIB)"

build/dock-peer: peer.c
	mkdir -p $(@D)
	$(CC) $(CFLAGS) $$(pkg-config --cflags gtk+-3.0) $< -o $@ $$(pkg-config --libs gtk+-3.0)

.PHONY: test test-x11 test-wayland
test: test-x11 test-wayland

test-x11: build/dock-pilot build/dock-peer
	bash x11.sh

test-wayland: build/dock-pilot
	node wayland.mjs

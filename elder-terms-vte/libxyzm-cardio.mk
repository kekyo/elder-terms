include Makefile

# Preserve libxyzm's compiler defaults while allowing the parent Meson build
# to append build-type-specific flags.
LIBXYZM_EXTRA_CXXFLAGS ?=
override CXXFLAGS += $(LIBXYZM_EXTRA_CXXFLAGS)

# Keep every Cardio consumer on the same shared runtime and feature set. GIO's
# compiler flags are needed because enabling the GIO integration includes its
# public headers from cardio.h.
override CARDIO_CPPFLAGS += $(shell pkg-config --cflags gio-2.0)
override CARDIO_ASYNC_CPPFLAGS := \
	-DCARDIO_SHARED_LIB=1 \
	-DCARDIO_WITH_GLIB=1 \
	-DCARDIO_WITH_GIO=1 \
	-DCARDIO_WITH_LINUX_IO_URING=1

# libxyzm's async objects include the externally selected cardio header. Keep
# their ABI aligned when elder-terms updates its cardio submodule.
ELDER_TERMS_LIBXYZM_CARDIO_MAKEFILE := $(firstword $(MAKEFILE_LIST))
$(LIBXYZM_ASYNC_OBJECTS): \
	$(CARDIO_DIR)/include/cardio.h \
	$(ELDER_TERMS_LIBXYZM_CARDIO_MAKEFILE)

include Makefile

# Preserve libxyzm's compiler defaults while allowing the parent Meson build
# to append build-type-specific flags.
LIBXYZM_EXTRA_CXXFLAGS ?=
override CXXFLAGS += $(LIBXYZM_EXTRA_CXXFLAGS)

# libxyzm's async objects include the externally selected cardio header. Keep
# their ABI aligned when elder-terms updates its cardio submodule.
$(LIBXYZM_ASYNC_OBJECTS): $(CARDIO_DIR)/include/cardio.h

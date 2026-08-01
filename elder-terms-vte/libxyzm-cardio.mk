include Makefile

# libxyzm's async objects include the externally selected cardio header. Keep
# their ABI aligned when elder-terms updates its cardio submodule.
$(LIBXYZM_ASYNC_OBJECTS): $(CARDIO_DIR)/include/cardio.h

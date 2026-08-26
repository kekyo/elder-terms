#include "terminal-bell.h"

#include <utility>

namespace elder_terms {

struct TerminalBellState {
  TerminalBellCallbacks callbacks;
  std::optional<std::filesystem::path> sound_file;
};

TerminalBellState *create_terminal_bell(TerminalBellCallbacks callbacks) {
  return new TerminalBellState{
      .callbacks = std::move(callbacks),
      .sound_file = std::nullopt,
  };
}

void apply_terminal_bell_settings(TerminalBellState *state,
                                  TerminalBellSettings settings) {
  if (state == nullptr) {
    return;
  }
  if (state->callbacks.cancel_playback) {
    state->callbacks.cancel_playback();
  }
  state->sound_file = std::move(settings.sound_file);
  if (state->callbacks.set_audible) {
    state->callbacks.set_audible(!state->sound_file.has_value());
  }
}

void ring_terminal_bell(TerminalBellState *state) {
  if (state == nullptr || !state->sound_file.has_value()) {
    return;
  }
  if (state->callbacks.cancel_playback) {
    state->callbacks.cancel_playback();
  }

  const std::optional<std::string> error =
      state->callbacks.play_file
          ? state->callbacks.play_file(*state->sound_file)
          : std::optional<std::string>{"custom sound player is unavailable"};
  if (!error.has_value()) {
    return;
  }

  state->sound_file.reset();
  if (state->callbacks.set_audible) {
    state->callbacks.set_audible(true);
  }
  if (state->callbacks.warning) {
    state->callbacks.warning("Failed to play terminal bell sound: " + *error);
  }
}

void destroy_terminal_bell(TerminalBellState *state) {
  if (state == nullptr) {
    return;
  }
  if (state->callbacks.cancel_playback) {
    state->callbacks.cancel_playback();
  }
  delete state;
}

} // namespace elder_terms

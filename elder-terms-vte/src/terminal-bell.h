#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <elder-terms/settings/terminal-settings.h>

namespace elder_terms {

struct TerminalBellState;

/**
 * Integrates terminal BEL policy with a terminal widget and sound player.
 */
struct TerminalBellCallbacks {
  /**
   * Enables or disables the terminal widget's built-in audible bell.
   *
   * @param audible True to let the terminal widget produce its default beep.
   */
  std::function<void(bool audible)> set_audible;
  /** Stops the currently playing custom BEL, if any. */
  std::function<void()> cancel_playback;
  /**
   * Starts custom BEL playback.
   *
   * @param sound_file Absolute path selected for custom playback.
   * @returns No value when playback was accepted, or an error description.
   */
  std::function<std::optional<std::string>(
      const std::filesystem::path &sound_file)>
      play_file;
  /**
   * Reports a non-fatal playback failure.
   *
   * @param message Human-readable failure description.
   */
  std::function<void(std::string message)> warning;
};

/**
 * Creates terminal BEL policy state.
 *
 * @param callbacks Terminal widget, playback, and warning integrations.
 * @returns Newly allocated terminal BEL state.
 */
TerminalBellState *create_terminal_bell(TerminalBellCallbacks callbacks);

/**
 * Applies built-in or custom terminal BEL playback settings.
 *
 * @param state Terminal BEL state.
 * @param settings Effective BEL settings.
 */
void apply_terminal_bell_settings(TerminalBellState *state,
                                  TerminalBellSettings settings);

/**
 * Handles one BEL request emitted by VTE.
 *
 * @param state Terminal BEL state.
 */
void ring_terminal_bell(TerminalBellState *state);

/**
 * Stops playback and destroys terminal BEL policy state.
 *
 * @param state Terminal BEL state, or null.
 */
void destroy_terminal_bell(TerminalBellState *state);

} // namespace elder_terms

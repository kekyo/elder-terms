#include "terminal-bell.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace elder_terms_terminal_bell_test {

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct BellProbe {
  bool audible = false;
  std::optional<std::string> play_error;
  std::vector<std::string> events;
};

static elder_terms::TerminalBellCallbacks callbacks_for(BellProbe *probe) {
  return {
      .set_audible = [probe](bool audible) {
        probe->audible = audible;
        probe->events.push_back(audible ? "audible:on" : "audible:off");
      },
      .cancel_playback = [probe]() {
        probe->events.push_back("cancel");
      },
      .play_file = [probe](const std::filesystem::path &path) {
        probe->events.push_back("play:" + path.string());
        return probe->play_error;
      },
      .warning = [probe](std::string warning) {
        probe->events.push_back("warning:" + warning);
      },
  };
}

static void default_bell_leaves_playback_to_vte() {
  BellProbe probe;
  elder_terms::TerminalBellState *state =
      elder_terms::create_terminal_bell(callbacks_for(&probe));

  elder_terms::apply_terminal_bell_settings(
      state, elder_terms::TerminalBellSettings{.sound_file = std::nullopt});
  expect_true(probe.audible,
              "the default bell should enable VTE's audible bell");
  probe.events.clear();
  elder_terms::ring_terminal_bell(state);
  expect_true(probe.events.empty(),
              "the default bell should not invoke custom playback");

  elder_terms::destroy_terminal_bell(state);
}

static void custom_bell_replaces_and_limits_playback() {
  BellProbe probe;
  const std::filesystem::path path = "/tmp/elder-terms-bell.oga";
  elder_terms::TerminalBellState *state =
      elder_terms::create_terminal_bell(callbacks_for(&probe));

  elder_terms::apply_terminal_bell_settings(
      state, elder_terms::TerminalBellSettings{.sound_file = path});
  expect_true(!probe.audible,
              "a custom bell should disable VTE's audible bell");
  probe.events.clear();

  elder_terms::ring_terminal_bell(state);
  elder_terms::ring_terminal_bell(state);
  expect_true(
      probe.events ==
          std::vector<std::string>{"cancel", "play:" + path.string(),
                                   "cancel", "play:" + path.string()},
      "each custom bell should cancel the previous playback before starting");

  probe.events.clear();
  elder_terms::apply_terminal_bell_settings(
      state, elder_terms::TerminalBellSettings{.sound_file = std::nullopt});
  expect_true(probe.audible,
              "restoring the default bell should re-enable VTE playback");
  expect_true(probe.events ==
                  std::vector<std::string>{"cancel", "audible:on"},
              "restoring the default bell should stop custom playback");

  elder_terms::destroy_terminal_bell(state);
}

static void playback_failure_falls_back_once_and_can_retry_after_apply() {
  BellProbe probe;
  const std::filesystem::path path = "/tmp/elder-terms-bell.wav";
  probe.play_error = "audio service unavailable";
  elder_terms::TerminalBellState *state =
      elder_terms::create_terminal_bell(callbacks_for(&probe));
  elder_terms::apply_terminal_bell_settings(
      state, elder_terms::TerminalBellSettings{.sound_file = path});
  probe.events.clear();

  elder_terms::ring_terminal_bell(state);
  expect_true(probe.audible,
              "a custom playback failure should restore VTE's audible bell");
  expect_true(
      probe.events ==
          std::vector<std::string>{
              "cancel", "play:" + path.string(), "audible:on",
              "warning:Failed to play terminal bell sound: audio service "
              "unavailable"},
      "a custom playback failure should be reported and fall back");

  probe.events.clear();
  elder_terms::ring_terminal_bell(state);
  expect_true(probe.events.empty(),
              "the failed custom player should not be retried for every BEL");

  probe.play_error = std::nullopt;
  elder_terms::apply_terminal_bell_settings(
      state, elder_terms::TerminalBellSettings{.sound_file = path});
  probe.events.clear();
  elder_terms::ring_terminal_bell(state);
  expect_true(probe.events ==
                  std::vector<std::string>{"cancel",
                                           "play:" + path.string()},
              "applying settings again should allow custom playback to retry");

  elder_terms::destroy_terminal_bell(state);
}

} // namespace elder_terms_terminal_bell_test

int main() {
  elder_terms_terminal_bell_test::default_bell_leaves_playback_to_vte();
  elder_terms_terminal_bell_test::custom_bell_replaces_and_limits_playback();
  elder_terms_terminal_bell_test::
      playback_failure_falls_back_once_and_can_retry_after_apply();
  return 0;
}

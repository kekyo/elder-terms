// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-connection-phase.h"

#include <array>
#include <stdexcept>
#include <string>

namespace elder_terms {

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static void pending_connection_phases_keep_the_terminal_dimmed_without_a_disconnected_notice() {
  constexpr std::array phases{
      TerminalSessionConnectionPhase::connecting,
      TerminalSessionConnectionPhase::verifying_host,
      TerminalSessionConnectionPhase::authenticating,
      TerminalSessionConnectionPhase::opening_shell,
  };

  for (TerminalSessionConnectionPhase phase : phases) {
    const TerminalConnectionPresentation presentation =
        terminal_connection_presentation(phase);
    expect_true(!presentation.connection_active,
                "Pending connection phase must not activate CONN");
    expect_true(!presentation.terminal_interactive,
                "Pending connection phase must keep terminal read-only");
    expect_true(presentation.terminal_dim_visible,
                "Pending connection phase must keep terminal dimmed");
    expect_true(!presentation.disconnected_notice_visible,
                "Pending connection phase must hide Disconnected");
  }
}

static void connected_phase_enables_the_terminal() {
  const TerminalConnectionPresentation presentation =
      terminal_connection_presentation(
          TerminalSessionConnectionPhase::connected);

  expect_true(presentation.connection_active,
              "Connected phase must activate CONN");
  expect_true(presentation.terminal_interactive,
              "Connected phase must enable terminal input");
  expect_true(!presentation.terminal_dim_visible,
              "Connected phase must remove terminal dimming");
  expect_true(!presentation.disconnected_notice_visible,
              "Connected phase must hide Disconnected");
}

static void disconnected_phase_is_the_only_phase_that_shows_disconnected() {
  const TerminalConnectionPresentation presentation =
      terminal_connection_presentation(
          TerminalSessionConnectionPhase::disconnected);

  expect_true(!presentation.connection_active,
              "Disconnected phase must deactivate CONN");
  expect_true(!presentation.terminal_interactive,
              "Disconnected phase must keep terminal read-only");
  expect_true(presentation.terminal_dim_visible,
              "Disconnected phase must keep terminal dimmed");
  expect_true(presentation.disconnected_notice_visible,
              "Disconnected phase must show Disconnected");
}

static void reconnect_is_available_for_every_manual_disconnection() {
  for (const bool auto_close : {false, true}) {
    for (const auto phase : {TerminalSessionConnectionPhase::connecting,
                            TerminalSessionConnectionPhase::verifying_host,
                            TerminalSessionConnectionPhase::authenticating,
                            TerminalSessionConnectionPhase::opening_shell,
                            TerminalSessionConnectionPhase::connected,
                            TerminalSessionConnectionPhase::disconnected}) {
      for (const bool ready : {false, true}) {
        for (const bool closing : {false, true}) {
          const auto presentation = terminal_reconnect_presentation(
              auto_close, phase, ready, closing);
          const bool visible = !auto_close && !closing &&
              phase == TerminalSessionConnectionPhase::disconnected;
          expect_true(presentation.visible == visible, "reconnect visibility must honor closing policy");
          expect_true(presentation.sensitive == (visible && ready), "reconnect must wait for cleanup");
        }
      }
    }
  }
}

} // namespace elder_terms

int main() {
  try {
    elder_terms::
        pending_connection_phases_keep_the_terminal_dimmed_without_a_disconnected_notice();
    elder_terms::connected_phase_enables_the_terminal();
    elder_terms::
        disconnected_phase_is_the_only_phase_that_shows_disconnected();
    elder_terms::reconnect_is_available_for_every_manual_disconnection();
    return 0;
  } catch (...) {
    return 1;
  }
}

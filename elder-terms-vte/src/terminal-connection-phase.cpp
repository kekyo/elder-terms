#include "terminal-connection-phase.h"

namespace elder_terms {

TerminalConnectionPresentation terminal_connection_presentation(
    TerminalSessionConnectionPhase phase) {
  const bool connected = phase == TerminalSessionConnectionPhase::connected;
  return {
      .connection_active = connected,
      .terminal_interactive = connected,
      .terminal_dim_visible = !connected,
      .disconnected_notice_visible =
          phase == TerminalSessionConnectionPhase::disconnected,
  };
}

} // namespace elder_terms

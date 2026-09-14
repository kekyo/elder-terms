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

TerminalReconnectPresentation terminal_reconnect_presentation(
    TerminalConnectionKind kind, bool auto_close,
    TerminalSessionConnectionPhase phase, bool ready, bool closing) {
  const bool visible = !auto_close && !closing &&
      (kind == TerminalConnectionKind::ssh || kind == TerminalConnectionKind::telnet) &&
      phase == TerminalSessionConnectionPhase::disconnected;
  return {.visible = visible, .sensitive = visible && ready};
}

} // namespace elder_terms

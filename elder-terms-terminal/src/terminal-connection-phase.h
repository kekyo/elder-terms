// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

namespace elder_terms {

/**
 * Describes the lifecycle phase of a terminal backend connection.
 */
enum class TerminalSessionConnectionPhase {
  /** The backend is resolving or opening its transport. */
  connecting,
  /** The backend is validating or asking about the remote host key. */
  verifying_host,
  /** The backend is performing user authentication. */
  authenticating,
  /** The backend is opening the interactive terminal session. */
  opening_shell,
  /** The terminal session is ready for user input. */
  connected,
  /** The terminal session is not connected and is no longer progressing. */
  disconnected,
};

/**
 * Describes the main-window presentation implied by a connection phase.
 */
struct TerminalConnectionPresentation {
  /** True when the CONN indicator and connection-scoped services are active. */
  bool connection_active;
  /** True when the terminal may accept user input. */
  bool terminal_interactive;
  /** True when the terminal surface must remain dimmed. */
  bool terminal_dim_visible;
  /** True when the disconnected notice must be visible. */
  bool disconnected_notice_visible;
};

/**
 * Derives the terminal presentation for a connection lifecycle phase.
 *
 * @param phase Current backend connection phase.
 * @returns Presentation flags for the main terminal surface.
 */
TerminalConnectionPresentation terminal_connection_presentation(
    TerminalSessionConnectionPhase phase);

/** Presentation of the optional disconnected-session reconnect button. */
struct TerminalReconnectPresentation {
  /** True for disconnected terminals when automatic window closing is disabled. */
  bool visible;
  /** True when the runtime can accept a manual reconnect request. */
  bool sensitive;
};

/**
 * Derives reconnection controls without altering automatic closing policy.
 * @param auto_close Whether automatic closing is configured.
 * @param phase Current backend phase.
 * @param ready Whether the session manager can accept a reconnect request.
 * @param closing Whether the terminal window is closing.
 * @returns Visibility and sensitivity of the reconnect button.
 */
TerminalReconnectPresentation terminal_reconnect_presentation(
    bool auto_close,
    TerminalSessionConnectionPhase phase, bool ready, bool closing);

} // namespace elder_terms

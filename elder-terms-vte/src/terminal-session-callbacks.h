#pragma once

#include <functional>
#include <span>
#include <string>

#include <cardio.h>

#include "activity-indicator-id.h"
#include "terminal-connection-phase.h"
#include "terminal-sessions/ssh-session/ssh-user-prompt.h"
#include "terminal-transfer.h"

namespace elder_terms {

/**
 * Callback invoked when a terminal session ends without an explicit stop.
 */
using TerminalSessionEndedCallback = std::function<void()>;

/**
 * Callback invoked after successful backend transport or line activity.
 */
using TerminalSessionActivityCallback =
    std::function<void(ActivityIndicatorId indicator)>;

/**
 * Callback invoked when backend connection or line state changes.
 */
using TerminalSessionIndicatorStateCallback =
    std::function<void(ActivityIndicatorId indicator, bool active)>;

/**
 * Callback invoked when the backend connection lifecycle phase changes.
 */
using TerminalSessionConnectionPhaseCallback =
    std::function<void(TerminalSessionConnectionPhase phase)>;

/**
 * Callback invoked when a backend session fails.
 */
using TerminalSessionFailureCallback = std::function<void(std::string message)>;

/**
 * Callback invoked with normal received terminal output before and after text
 * conversion.
 */
using TerminalSessionOutputCallback = std::function<void(
    std::span<const unsigned char> raw_bytes,
    std::span<const unsigned char> cooked_bytes)>;

/**
 * Callback invoked when a backend detects a ZMODEM auto-start preamble.
 */
using TerminalSessionZmodemAutoStartCallback =
    std::function<void(TerminalTransferDirection direction)>;

/**
 * Callback invoked when SSH requires a host-key or authentication response.
 */
using TerminalSessionSshPromptCallback = std::function<
    cardio::promise<SshUserPromptResponse>(
        const SshUserPrompt &prompt, cardio::cancellation cancellation)>;

/**
 * Optional callbacks emitted by terminal session backends.
 */
struct TerminalSessionCallbacks {
  /** Called after a backend reaches a natural terminal-session end. */
  TerminalSessionEndedCallback ended;
  /** Called after bytes or serial modem-line activity are observed. */
  TerminalSessionActivityCallback activity;
  /** Called when a connection or serial modem-line state changes. */
  TerminalSessionIndicatorStateCallback indicator_state;
  /** Called when the backend connection lifecycle phase changes. */
  TerminalSessionConnectionPhaseCallback connection_phase;
  /** Called with the reason when the backend session fails. */
  TerminalSessionFailureCallback failure;
  /** Called with normal terminal output, excluding active file transfers. */
  TerminalSessionOutputCallback output;
  /** Called when a valid ZMODEM auto-start preamble is detected. */
  TerminalSessionZmodemAutoStartCallback zmodem_auto_start;
  /** Collects all SSH user responses through the terminal overlay. */
  TerminalSessionSshPromptCallback ssh_prompt;
};

} // namespace elder_terms

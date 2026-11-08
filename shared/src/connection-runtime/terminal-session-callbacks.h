// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <functional>

#include <cardio.h>

#include "activity-indicator-id.h"
#include "terminal-connection-phase.h"
#include "ssh/ssh-user-prompt.h"

namespace elder_terms {

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
 * Callback invoked when SSH requires a host-key or authentication response.
 */
using TerminalSessionSshPromptCallback = std::function<
    cardio::promise<SshUserPromptResponse>(
        const SshUserPrompt &prompt, cardio::cancellation cancellation)>;

/**
 * Optional callbacks emitted by terminal session backends.
 */
struct TerminalSessionCallbacks {
  /** Called after bytes or serial modem-line activity are observed. */
  TerminalSessionActivityCallback activity;
  /** Called when a connection or serial modem-line state changes. */
  TerminalSessionIndicatorStateCallback indicator_state;
  /** Called when the backend connection lifecycle phase changes. */
  TerminalSessionConnectionPhaseCallback connection_phase;
  /** Collects all SSH user responses through the terminal overlay. */
  TerminalSessionSshPromptCallback ssh_prompt;
};

} // namespace elder_terms

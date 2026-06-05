#pragma once

#include <functional>

#include "activity-indicator-id.h"

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
 * Optional callbacks emitted by terminal session backends.
 */
struct TerminalSessionCallbacks {
  /** Called after a backend reaches a natural terminal-session end. */
  TerminalSessionEndedCallback ended;
  /** Called after bytes or serial modem-line activity are observed. */
  TerminalSessionActivityCallback activity;
  /** Called when a connection or serial modem-line state changes. */
  TerminalSessionIndicatorStateCallback indicator_state;
};

} // namespace elder_terms

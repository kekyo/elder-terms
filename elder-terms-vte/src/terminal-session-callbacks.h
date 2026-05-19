#pragma once

#include <functional>

namespace elder_terms {

/**
 * Callback invoked when a terminal session ends without an explicit stop.
 */
using TerminalSessionEndedCallback = std::function<void()>;

/**
 * Optional callbacks emitted by terminal session backends.
 */
struct TerminalSessionCallbacks {
  /** Called after a backend reaches a natural terminal-session end. */
  TerminalSessionEndedCallback ended;
};

} // namespace elder_terms

#pragma once

#include <functional>
#include <span>

#include "activity-indicator-id.h"
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
 * Optional callbacks emitted by terminal session backends.
 */
struct TerminalSessionCallbacks {
  /** Called after a backend reaches a natural terminal-session end. */
  TerminalSessionEndedCallback ended;
  /** Called after bytes or serial modem-line activity are observed. */
  TerminalSessionActivityCallback activity;
  /** Called when a connection or serial modem-line state changes. */
  TerminalSessionIndicatorStateCallback indicator_state;
  /** Called with normal terminal output, excluding active file transfers. */
  TerminalSessionOutputCallback output;
  /** Called when a valid ZMODEM auto-start preamble is detected. */
  TerminalSessionZmodemAutoStartCallback zmodem_auto_start;
};

} // namespace elder_terms

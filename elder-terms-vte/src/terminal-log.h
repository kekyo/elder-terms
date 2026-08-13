#pragma once

#include <chrono>
#include <functional>
#include <span>
#include <string>

#include <cardio.h>

#include <elder-terms/settings.h>

namespace elder_terms {

/**
 * Opaque state for asynchronous terminal stream logging.
 */
struct TerminalLogState;

/**
 * Supplies the local timestamp used to format a new connection log path.
 */
using TerminalLogNowCallback =
    std::function<std::chrono::system_clock::time_point()>;

/**
 * Reports whether a terminal log file is currently open for recording.
 */
using TerminalLogActiveCallback = std::function<void(bool active)>;

/**
 * Reports a non-fatal terminal logging failure.
 */
using TerminalLogWarningCallback =
    std::function<void(const std::string &warning)>;

/**
 * Dependencies and initial settings for a terminal log state.
 */
struct TerminalLogOptions {
  /** Initial effective terminal log settings. */
  TerminalLogSettings settings;
  /** Optional clock override; the system clock is used when empty. */
  TerminalLogNowCallback now;
  /** Optional callback receiving actual file open/close state. */
  TerminalLogActiveCallback active;
  /** Optional warning callback; stderr is used when empty. */
  TerminalLogWarningCallback warning;
};

/**
 * Creates an asynchronous terminal log state.
 *
 * @param options Initial settings and callbacks.
 * @returns New terminal log state owned by the caller.
 */
TerminalLogState *create_terminal_log(TerminalLogOptions options);

/**
 * Applies terminal log settings.
 *
 * @param state Terminal log state.
 * @param settings Updated effective settings.
 *
 * @remarks When connected, a changed setting closes the current file and
 * opens a newly formatted path when logging remains enabled.
 */
void apply_terminal_log_settings(TerminalLogState *state,
                                 TerminalLogSettings settings);

/**
 * Updates the backend connection state observed by the logger.
 *
 * @param state Terminal log state.
 * @param active True after the transport connects, false when it disconnects.
 *
 * @remarks Every false-to-true transition re-evaluates the configured path
 * format and queues a new file open.
 */
void set_terminal_log_connection_active(TerminalLogState *state,
                                        bool active);

/**
 * Queues one chunk of normal received terminal output for logging.
 *
 * @param state Terminal log state.
 * @param raw_bytes Backend bytes before terminal character conversion.
 * @param cooked_bytes UTF-8 bytes after terminal character conversion.
 */
void write_terminal_log(TerminalLogState *state,
                        std::span<const unsigned char> raw_bytes,
                        std::span<const unsigned char> cooked_bytes);

/**
 * Stops accepting data and asynchronously closes the current log file after
 * queued writes complete.
 *
 * @param state Terminal log state.
 * @returns Promise resolved after all queued log operations finish.
 */
cardio::promise<void> stop_terminal_log_async(TerminalLogState *state);

/**
 * Releases a stopped terminal log state.
 *
 * @param state Terminal log state, or null.
 *
 * @remarks The caller must await stop_terminal_log_async before destroying a
 * state that has accepted connection or write events.
 */
void destroy_terminal_log(TerminalLogState *state);

} // namespace elder_terms

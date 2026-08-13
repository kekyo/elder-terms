#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "terminal-transfer.h"

namespace elder_terms {

/**
 * Incremental detector state for ZMODEM auto-start preambles.
 *
 * @remarks Only a short byte suffix is retained for detecting ZHEX headers
 * split across terminal backend reads.
 */
struct TerminalZmodemAutoStartDetectorState {
  /** Retained suffix from previous remote payloads. */
  std::string tail;
};

/**
 * Scans remote bytes for ZMODEM ZRQINIT/ZRINIT auto-start headers.
 *
 * @param state Incremental detector state updated with the newest payload.
 * @param payload Raw bytes received from the remote side.
 * @returns Transfer direction to start, or std::nullopt when no valid preamble
 * was found.
 */
std::optional<TerminalTransferDirection>
feed_terminal_zmodem_auto_start_detector(
    TerminalZmodemAutoStartDetectorState *state, std::string_view payload);

} // namespace elder_terms

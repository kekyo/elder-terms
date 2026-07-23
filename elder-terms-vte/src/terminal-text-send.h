#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include <elder-terms/settings.h>

#include "terminal-transfer.h"

namespace elder_terms {

/**
 * Describes a UTF-8 text file used as a terminal text send source.
 */
struct TerminalTextSendFileSource {
  /** URI of the UTF-8 source text file. */
  std::string uri;
};

/**
 * Describes an in-memory UTF-8 terminal text send source.
 */
struct TerminalTextSendBufferSource {
  /** Complete UTF-8 text to send. */
  std::string utf8_text;
};

/**
 * Selects the source of one terminal text send operation.
 */
using TerminalTextSendSource =
    std::variant<TerminalTextSendFileSource, TerminalTextSendBufferSource>;

/**
 * Returns whether a text send source identifies non-empty source data.
 *
 * @param source File or buffered text source to validate.
 * @returns True when a file URI or buffered UTF-8 text is present.
 */
bool terminal_text_send_source_is_valid(const TerminalTextSendSource &source);

/**
 * Describes one requested text send operation.
 */
struct TerminalTextSendRequest {
  /** UTF-8 file or in-memory text source. */
  TerminalTextSendSource source;
  /** Terminal character encoding captured when the operation starts. */
  TerminalTextSettings text_settings;
  /** Maximum encoded payload rate in bytes per second. */
  std::uint64_t bytes_per_second = 1024;
  /** Receives transfer active/inactive state changes. */
  TerminalTransferActiveCallback active;
  /** Receives status text and conversion warnings. */
  TerminalTransferStatusCallback status;
  /** Receives source-file progress information. */
  TerminalTransferProgressCallback progress;
  /** Receives final send success state. */
  TerminalTransferFinishedCallback finished;
};

} // namespace elder_terms

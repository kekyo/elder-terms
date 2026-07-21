#pragma once

#include <cstdint>
#include <string>

#include <elder-terms/settings.h>

#include "terminal-transfer.h"

namespace elder_terms {

/**
 * Describes one requested text file send operation.
 */
struct TerminalTextSendRequest {
  /** URI of the UTF-8 source text file. */
  std::string source_file_uri;
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

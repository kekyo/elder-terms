#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <elder-terms/settings.h>

#include "terminal-transfer.h"

namespace elder_terms {

/**
 * Incrementally normalizes logical line endings for terminal text sends.
 */
class TerminalTextLineEndingNormalizer {
public:
  /**
   * Creates a line-ending normalizer.
   *
   * @param follow_return_code True to replace CRLF, CR, and LF with the
   * configured Return code.
   * @param return_code Return code used for each logical line ending.
   */
  TerminalTextLineEndingNormalizer(bool follow_return_code,
                                   TerminalReturnCode return_code);

  /**
   * Normalizes one input chunk.
   *
   * @param input Source bytes from a UTF-8 text stream.
   * @returns Bytes ready for character encoding.
   *
   * @remarks A trailing CR is retained until the next chunk so a split CRLF
   * is treated as one logical line ending.
   */
  std::vector<unsigned char>
  normalize(std::span<const unsigned char> input);

  /**
   * Flushes a retained trailing CR at the end of input.
   *
   * @returns Final normalized bytes.
   */
  std::vector<unsigned char> finish();

private:
  void append_newline(std::vector<unsigned char> *output) const;

  bool follow_return_code;
  TerminalReturnCode return_code;
  bool pending_carriage_return = false;
};

/**
 * Normalizes all logical line endings in one complete UTF-8 string.
 *
 * @param utf8_text Complete UTF-8 text to normalize.
 * @param follow_return_code True to replace CRLF, CR, and LF with the
 * configured Return code; false to preserve the original line endings.
 * @param return_code Return code used for each logical line ending. Auto is
 * represented as CR for text sends.
 * @returns Text with normalized or preserved line endings.
 */
std::string normalize_terminal_text_line_endings(
    const std::string &utf8_text, bool follow_return_code,
    TerminalReturnCode return_code);

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
  /** True to normalize logical line endings to text_settings.return_code. */
  bool follow_return_code = true;
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

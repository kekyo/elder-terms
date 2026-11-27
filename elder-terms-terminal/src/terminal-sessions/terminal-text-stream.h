// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace elder_terms {

/** Result of converting one terminal text stream chunk. */
struct TerminalTextConversionResult {
  /** Converted bytes for the destination stream. */
  std::vector<unsigned char> bytes;
  /** True when invalid or unrepresentable input required a replacement. */
  bool used_replacement = false;
};

/** Stateful character conversion without terminal key or protocol rewriting. */
struct TerminalTextStream;

/**
 * Releases one stream and its iconv descriptor.
 * @param stream Owned stream, or nullptr.
 */
void destroy_terminal_text_stream(TerminalTextStream *stream);

/** Exclusive stream ownership, including its required descriptor cleanup. */
using TerminalTextStreamPtr = std::unique_ptr<TerminalTextStream, decltype(&destroy_terminal_text_stream)>;

/**
 * Creates a byte-stream decoder producing UTF-8.
 * @param encoding Source iconv encoding name.
 * @return Owned decoder; invalid input is replaced with U+FFFD.
 * @throws std::system_error If iconv cannot create the conversion.
 */
TerminalTextStreamPtr create_terminal_text_decoder(const std::string &encoding);

/**
 * Creates a UTF-8 encoder producing bytes for a communication peer.
 * @param encoding Destination iconv encoding name.
 * @return Owned encoder; unrepresentable input is replaced with an encoded question mark.
 * @throws std::system_error If iconv cannot create the conversion.
 */
TerminalTextStreamPtr create_terminal_text_encoder(const std::string &encoding);

/**
 * Converts one chunk and retains incomplete characters for the next chunk.
 * @param stream Live stream that has not been finished.
 * @param bytes Borrowed source bytes, copied or consumed before return.
 * @return Converted bytes and replacement information.
 * @throws std::system_error On an iconv failure other than handled invalid input.
 * @throws std::logic_error After the stream has been finished.
 */
TerminalTextConversionResult convert_terminal_text(TerminalTextStream *stream,
    std::span<const unsigned char> bytes);

/**
 * Completes a stream, replacing incomplete input and emitting final shift bytes.
 * @param stream Live stream, finalized exactly once.
 * @return Remaining converted bytes and replacement information.
 * @throws std::system_error If iconv cannot complete the stream.
 * @throws std::logic_error After the stream has already been finished.
 */
TerminalTextConversionResult finish_terminal_text(TerminalTextStream *stream);

/** Reports whether the converter retains an incomplete source character. */
bool terminal_text_stream_has_pending(const TerminalTextStream *stream);

}

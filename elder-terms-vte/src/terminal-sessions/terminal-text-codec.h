#pragma once

#include <memory>
#include <span>
#include <vector>

#include <elder-terms/settings.h>

namespace elder_terms {

/**
 * Result of converting one terminal text stream chunk.
 */
struct TerminalTextConversionResult {
  /** Converted bytes for the destination stream. */
  std::vector<unsigned char> bytes;
  /** True when invalid or unrepresentable input required a replacement. */
  bool used_replacement = false;
};

/**
 * Stateful character and special-code converter at the VTE UTF-8 boundary.
 *
 * @remarks Backend input is decoded to UTF-8. VTE commit input is optionally
 * remapped for ADM3 cursor keys and encoded to the configured backend
 * character set. Conversion state and incomplete multibyte sequences are
 * retained between calls.
 */
class TerminalTextCodec {
private:
  class Impl;
  std::unique_ptr<Impl> impl;

public:
  /**
   * Creates a terminal text codec.
   *
   * @param settings Effective terminal text settings.
   * @throws std::system_error When iconv cannot create either conversion.
   */
  explicit TerminalTextCodec(const TerminalTextSettings &settings);

  /**
   * Releases iconv conversion descriptors.
   */
  ~TerminalTextCodec();

  TerminalTextCodec(const TerminalTextCodec &) = delete;
  TerminalTextCodec &operator=(const TerminalTextCodec &) = delete;

  /**
   * Decodes one backend byte chunk to UTF-8 for VTE.
   *
   * @param bytes Backend byte chunk.
   * @returns Converted UTF-8 bytes and replacement status.
   */
  TerminalTextConversionResult
  decode(std::span<const unsigned char> bytes);

  /**
   * Maps and encodes one UTF-8 VTE commit chunk for the backend.
   *
   * @param bytes UTF-8 bytes committed by VTE.
   * @returns Converted backend bytes and replacement status.
   */
  TerminalTextConversionResult
  encode(std::span<const unsigned char> bytes);
};

} // namespace elder_terms

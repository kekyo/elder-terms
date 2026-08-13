#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace elder_terms {

/**
 * Byte buffer used by the TELNET protocol parser and encoder.
 */
using TelnetBytes = std::vector<unsigned char>;

/**
 * Result of consuming bytes received from a TELNET server.
 */
struct TelnetProtocolResult {
  /** Bytes that should be rendered by VTE. */
  TelnetBytes terminal_data;
  /** Protocol responses that should be sent back to the server. */
  std::vector<TelnetBytes> responses;
};

/**
 * Stateful TELNET protocol parser for terminal sessions.
 */
class TelnetProtocol {
private:
  enum class ParseState {
    data,
    data_cr,
    command,
    negotiation,
    suboption,
    suboption_command,
  };

  ParseState state = ParseState::data;
  unsigned char pending_command = 0;
  std::uint16_t columns = 80;
  std::uint16_t rows = 24;
  bool naws_enabled = false;
  bool local_binary_enabled = false;
  bool remote_binary_enabled = false;
  bool local_binary_requested = false;
  bool remote_binary_requested = false;
  bool binary_rejected = false;
  bool text_send_pending_cr = false;
  std::string terminal_type;
  bool terminal_type_enabled = false;
  TelnetBytes suboption_bytes;

  void handle_data_byte(unsigned char byte, TelnetProtocolResult *result);
  void handle_data_cr_byte(unsigned char byte, TelnetProtocolResult *result);
  void handle_negotiation(unsigned char option, TelnetProtocolResult *result);
  void handle_suboption(TelnetProtocolResult *result);

public:
  /**
   * Creates a TELNET protocol parser with the terminal type to report.
   *
   * @param terminal_type Terminal type returned for TERMINAL-TYPE SEND.
   */
  explicit TelnetProtocol(std::string terminal_type);

  /**
   * Updates the window size used for NAWS responses.
   *
   * @param columns Terminal column count, clamped to 16 bits by the caller.
   * @param rows Terminal row count, clamped to 16 bits by the caller.
   */
  void set_window_size(std::uint16_t columns, std::uint16_t rows);

  /**
   * Returns whether the server enabled NAWS.
   *
   * @returns True when NAWS updates should be sent after resizes.
   */
  bool is_naws_enabled() const;

  /**
   * Returns whether both TELNET BINARY directions are enabled.
   *
   * @returns True after WILL/DO BINARY negotiation succeeds both ways.
   */
  bool is_binary_enabled() const;

  /**
   * Returns whether BINARY negotiation was rejected.
   *
   * @returns True after receiving DONT/WONT BINARY for a requested direction.
   */
  bool is_binary_rejected() const;

  /**
   * Consumes bytes received from the TELNET server.
   *
   * @param bytes Raw bytes received from the server.
   * @returns Terminal bytes and protocol responses produced by the parser.
   */
  TelnetProtocolResult receive(std::span<const unsigned char> bytes);

  /**
   * Encodes user input for TELNET transmission.
   *
   * @param bytes Raw user input bytes from VTE.
   * @returns Bytes with TELNET IAC escaping applied.
   */
  TelnetBytes encode_user_input(std::span<const unsigned char> bytes) const;

  /** Resets the streaming TELNET encoder for a new text send. */
  void begin_text_send_encoding();

  /**
   * Encodes one text-send chunk while retaining a trailing NVT CR.
   *
   * @param bytes Encoded terminal text payload chunk.
   * @returns TELNET-framed bytes ready for the network.
   */
  TelnetBytes encode_text_send(std::span<const unsigned char> bytes);

  /**
   * Finishes streaming text-send encoding.
   *
   * @returns A trailing NVT CR NUL sequence when a bare CR was pending.
   */
  TelnetBytes finish_text_send_encoding();

  /**
   * Encodes the TELNET BREAK control command.
   *
   * @returns IAC BRK bytes ready for the network.
   */
  TelnetBytes encode_break() const;

  /**
   * Encodes TELNET BINARY negotiation requests.
   *
   * @returns WILL BINARY and DO BINARY request bytes.
   */
  std::vector<TelnetBytes> encode_enable_binary();

  /**
   * Encodes the current terminal size as a NAWS subnegotiation.
   *
   * @returns TELNET NAWS subnegotiation bytes.
   */
  TelnetBytes encode_naws() const;
};

} // namespace elder_terms

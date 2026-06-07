#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace elder_terms {

/**
 * File transfer protocol family.
 */
enum class TerminalTransferProtocol {
  /** XMODEM single-file transfer. */
  xmodem,
  /** YMODEM batch-capable transfer. */
  ymodem,
  /** ZMODEM batch-capable transfer. */
  zmodem,
};

/**
 * File transfer direction.
 */
enum class TerminalTransferDirection {
  /** Local file to remote peer. */
  send,
  /** Remote file to local storage. */
  receive,
};

/**
 * XMODEM outbound payload packet size.
 */
enum class TerminalTransferXmodemPacketSize {
  /** Use classic 128-byte SOH frames. */
  bytes_128,
  /** Use 1024-byte STX frames where possible. */
  bytes_1024,
};

/**
 * XMODEM trailer checksum behavior.
 */
enum class TerminalTransferXmodemChecksumMode {
  /** Let XMODEM send operations follow the peer handshake. */
  automatic,
  /** Use the legacy 8-bit checksum trailer. */
  checksum,
  /** Use the 16-bit CRC trailer. */
  crc,
};

/**
 * YMODEM wire variant.
 */
enum class TerminalTransferYmodemVariant {
  /** Let YMODEM send operations follow the peer request. */
  automatic,
  /** Use classic YMODEM ACK/NAK handshakes. */
  standard,
  /** Use streaming YMODEM-g. */
  g,
};

/**
 * User-selectable transfer protocol options.
 */
struct TerminalTransferOptions {
  /** XMODEM packet size used by send operations. */
  TerminalTransferXmodemPacketSize xmodem_packet_size =
      TerminalTransferXmodemPacketSize::bytes_1024;
  /** XMODEM checksum mode. Automatic negotiates on send and requests CRC on
   * receive. */
  TerminalTransferXmodemChecksumMode xmodem_checksum_mode =
      TerminalTransferXmodemChecksumMode::automatic;
  /** YMODEM variant used by send and receive operations. */
  TerminalTransferYmodemVariant ymodem_variant =
      TerminalTransferYmodemVariant::automatic;
};

/**
 * Called when the terminal should enter or leave transfer presentation mode.
 */
using TerminalTransferActiveCallback = std::function<void(bool active)>;

/**
 * Called when transfer status text changes.
 */
using TerminalTransferStatusCallback =
    std::function<void(const std::string &status)>;

/**
 * Progress mode reported by an active transfer.
 */
enum class TerminalTransferProgressMode {
  /** Progress has no stable fraction and should be animated. */
  indeterminate,
  /** Progress has a known fraction from 0.0 to 1.0. */
  determinate,
};

/**
 * Describes the current transfer progress presentation.
 */
struct TerminalTransferProgress {
  /** Progress bar rendering mode. */
  TerminalTransferProgressMode mode = TerminalTransferProgressMode::indeterminate;
  /** Determinate progress fraction when known. */
  std::optional<double> fraction;
};

/**
 * Called when transfer progress presentation changes.
 */
using TerminalTransferProgressCallback =
    std::function<void(TerminalTransferProgress progress)>;

/**
 * Called when one transfer attempt completes.
 */
using TerminalTransferFinishedCallback =
    std::function<void(bool succeeded)>;

/**
 * Describes one requested X/Y/ZMODEM transfer.
 */
struct TerminalTransferRequest {
  /** Protocol selected by the user. */
  TerminalTransferProtocol protocol = TerminalTransferProtocol::zmodem;
  /** Transfer direction selected by the user. */
  TerminalTransferDirection direction = TerminalTransferDirection::send;
  /** Configured transfer base path or URI. Empty means use the runtime default. */
  std::string base_path;
  /** Source file URIs for send operations. Empty for receive operations. */
  std::vector<std::string> source_file_uris;
  /** Protocol options selected for this transfer. */
  TerminalTransferOptions options;
  /** Receives transfer active/inactive state changes. */
  TerminalTransferActiveCallback active;
  /** Receives status text for the status bar. */
  TerminalTransferStatusCallback status;
  /** Receives progress information for transfer UI. */
  TerminalTransferProgressCallback progress;
  /** Receives final transfer success state. */
  TerminalTransferFinishedCallback finished;
};

/**
 * Returns a stable lowercase protocol token.
 *
 * @param protocol Transfer protocol.
 * @returns Protocol token.
 */
const char *terminal_transfer_protocol_token(TerminalTransferProtocol protocol);

/**
 * Returns a human-readable protocol label.
 *
 * @param protocol Transfer protocol.
 * @returns Protocol label.
 */
const char *terminal_transfer_protocol_label(TerminalTransferProtocol protocol);

} // namespace elder_terms

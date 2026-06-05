#pragma once

#include <functional>
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
 * Called when the terminal should enter or leave transfer presentation mode.
 */
using TerminalTransferActiveCallback = std::function<void(bool active)>;

/**
 * Called when transfer status text changes.
 */
using TerminalTransferStatusCallback =
    std::function<void(const std::string &status)>;

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
  /** Receives transfer active/inactive state changes. */
  TerminalTransferActiveCallback active;
  /** Receives status text for the status bar. */
  TerminalTransferStatusCallback status;
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

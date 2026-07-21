#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

#include <cardio.h>
#include <libxyzm/async.h>

#include "terminal-transfer.h"

namespace elder_terms {

/**
 * Transport callbacks used by the terminal transfer runner.
 */
struct TerminalTransferTransport {
  /** Writes bytes to the active terminal backend. */
  xyzm_async_send_cb send;
  /** Reads bytes from the active terminal backend. */
  xyzm_async_recv_cb recv;
  /** Returns monotonic milliseconds for libxyzm. */
  xyzm_async_now_ms_cb now_ms;
};

/**
 * Removes path components and unsafe characters from a received file name.
 *
 * @param name Candidate file name from protocol metadata.
 * @param fallback File name used when the candidate is empty or unsafe.
 * @returns Sanitized file name.
 */
std::string sanitize_transfer_file_name(const std::string &name,
                                        const std::string &fallback);

/**
 * Formats a transfer progress status label.
 *
 * @param file_name Current file name.
 * @param transferred_bytes Bytes already transferred for the current file.
 * @param total_bytes Current file size when known.
 * @param eta_seconds Remaining seconds when an ETA is stable.
 * @returns Status text suitable for the status bar.
 */
std::string format_transfer_status(
    const std::string &file_name, std::uint64_t transferred_bytes,
    std::optional<std::uint64_t> total_bytes,
    std::optional<std::uint64_t> eta_seconds);

/**
 * Estimates remaining transfer seconds from current-file progress.
 *
 * @param baseline_transferred_bytes Transferred bytes at the ETA baseline.
 * @param transferred_bytes Current transferred bytes for the current file.
 * @param total_bytes Current file size.
 * @param elapsed_ms Milliseconds elapsed since the ETA baseline.
 * @returns Remaining seconds when enough progress has been observed.
 */
std::optional<std::uint64_t> estimate_transfer_eta_seconds(
    std::uint64_t baseline_transferred_bytes,
    std::uint64_t transferred_bytes, std::uint64_t total_bytes,
    std::uint64_t elapsed_ms);

/**
 * Resolves a configured transfer base path through GIO and returns its URI.
 *
 * @param base_path Configured path or URI. An empty value selects the XDG
 * Downloads directory, falling back to the Downloads directory beneath the
 * user home directory.
 * @returns GIO URI for the resolved base directory.
 */
std::string resolve_transfer_base_path_uri(const std::string &base_path);

/**
 * Builds libxyzm XMODEM send options from a transfer request.
 *
 * @param request Transfer request containing the selected XMODEM options.
 * @returns libxyzm XMODEM send options.
 */
xyzm_xmodem_send_opts_t
terminal_transfer_xmodem_send_options(const TerminalTransferRequest &request);

/**
 * Builds libxyzm XMODEM receive options from a transfer request.
 *
 * @param request Transfer request containing the selected XMODEM options.
 * @returns libxyzm XMODEM receive options.
 */
xyzm_xmodem_receive_opts_t terminal_transfer_xmodem_receive_options(
    const TerminalTransferRequest &request);

/**
 * Builds libxyzm YMODEM send options from a transfer request.
 *
 * @param request Transfer request containing the selected YMODEM options.
 * @returns libxyzm YMODEM send options.
 */
xyzm_ymodem_opts_t
terminal_transfer_ymodem_send_options(const TerminalTransferRequest &request);

/**
 * Builds libxyzm YMODEM receive options from a transfer request.
 *
 * @param request Transfer request containing the selected YMODEM options.
 * @returns libxyzm YMODEM receive options.
 */
xyzm_ymodem_opts_t terminal_transfer_ymodem_receive_options(
    const TerminalTransferRequest &request);

/**
 * Runs one X/Y/ZMODEM transfer using libxyzm asynchronous APIs.
 *
 * @param request Transfer request.
 * @param transport Backend transport callbacks.
 * @param cancellation Cancellation signal for the whole transfer.
 * @returns Promise that resolves after successful transfer completion.
 */
cardio::promise<void>
run_terminal_transfer_async(TerminalTransferRequest request,
                            TerminalTransferTransport transport,
                            cardio::cancellation cancellation);

} // namespace elder_terms

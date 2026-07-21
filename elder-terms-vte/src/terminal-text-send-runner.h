#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include <cardio.h>

#include "terminal-text-send.h"

namespace elder_terms {

/**
 * Backend and scheduling callbacks used by the text send runner.
 */
struct TerminalTextSendTransport {
  /** Writes one complete encoded payload chunk to the terminal backend. */
  std::function<cardio::promise<void>(std::span<const unsigned char> bytes,
                                      cardio::cancellation cancellation)>
      send;
  /** Returns monotonic microseconds. */
  std::function<std::uint64_t()> now_us;
  /** Asynchronously waits for at least the requested microseconds. */
  std::function<cardio::promise<void>(std::uint64_t delay_us,
                                      cardio::cancellation cancellation)>
      delay;
};

/**
 * Sends one finite UTF-8 text file with encoding conversion and throttling.
 *
 * @param request Text send request and presentation callbacks.
 * @param transport Backend write and monotonic scheduling callbacks.
 * @param cancellation Cancellation signal for the whole operation.
 * @returns Promise that resolves after the encoded file has been written.
 */
cardio::promise<void>
run_terminal_text_send_async(TerminalTextSendRequest request,
                             TerminalTextSendTransport transport,
                             cardio::cancellation cancellation);

} // namespace elder_terms

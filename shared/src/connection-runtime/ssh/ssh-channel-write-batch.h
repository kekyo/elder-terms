// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <cstdint>
#include <utility>

namespace elder_terms {

/** Outcome of writing channel bytes and, for a complete write, flushing them. */
template <typename Result>
struct SshChannelWriteBatchResult {
  /** Bytes accepted by the channel write, or its error code. */
  int written;
  /** Whether a complete write attempted a transport flush. */
  bool flushed;
  /** Result and readiness from the last attempted operation. */
  Result result;
};

/**
 * Dispatches one channel write and flushes a complete write.
 *
 * @tparam Dispatch Executes an operation on the SSH worker.
 * @tparam Write Writes channel bytes and returns a result with a value member.
 * @tparam Flush Flushes transport bytes and returns the same result type.
 * @param dispatch Worker dispatcher.
 * @param write Channel write callback.
 * @param flush Transport flush callback.
 * @param requested Number of bytes requested from the channel write.
 * @return Written byte count and the final operation result.
 */
template <typename Dispatch, typename Write, typename Flush>
auto dispatch_ssh_write_and_flush(Dispatch dispatch, Write write, Flush flush,
                                  std::uint32_t requested) {
  return dispatch([write = std::move(write), flush = std::move(flush),
                   requested]() mutable {
    auto result = write();
    const int written = result.value;
    if (written > 0 && static_cast<std::uint32_t>(written) == requested) {
      result = flush();
      return SshChannelWriteBatchResult<decltype(result)>{written, true,
                                                          std::move(result)};
    }
    return SshChannelWriteBatchResult<decltype(result)>{written, false,
                                                        std::move(result)};
  });
}

} // namespace elder_terms

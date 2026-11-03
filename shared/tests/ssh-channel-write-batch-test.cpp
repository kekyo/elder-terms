// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../src/connection-runtime/ssh/ssh-channel-write-batch.h"

#include <cstdint>
#include <initializer_list>
#include <stdexcept>

namespace elder_terms {

struct FakeSshResult {
  int value;
  int poll_flags;
};

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static void test_complete_write_flushes_in_one_dispatch() {
  int dispatch_count = 0;
  int flush_count = 0;
  const auto outcome = dispatch_ssh_write_and_flush(
      [&](auto operation) {
        ++dispatch_count;
        return operation();
      },
      []() { return FakeSshResult{1024, 1}; },
      [&]() {
        ++flush_count;
        return FakeSshResult{0, 2};
      },
      std::uint32_t{1024});
  expect(dispatch_count == 1, "write and flush should use one worker dispatch");
  expect(flush_count == 1, "complete write should flush before returning");
  expect(outcome.written == 1024 && outcome.flushed &&
             outcome.result.value == 0 && outcome.result.poll_flags == 2,
         "complete write should report its flush outcome");
}

static void test_partial_or_stalled_write_does_not_flush() {
  for (const int written : {512, 0, -1}) {
    int dispatch_count = 0;
    int flush_count = 0;
    const auto outcome = dispatch_ssh_write_and_flush(
        [&](auto operation) {
          ++dispatch_count;
          return operation();
        },
        [written]() { return FakeSshResult{written, 3}; },
        [&]() {
          ++flush_count;
          return FakeSshResult{0, 4};
        },
        std::uint32_t{1024});
    expect(dispatch_count == 1, "write should use one worker dispatch");
    expect(flush_count == 0 && !outcome.flushed,
           "incomplete write should wait or retry before flushing");
    expect(outcome.written == written && outcome.result.value == written &&
               outcome.result.poll_flags == 3,
           "incomplete write should preserve its status and readiness");
  }
}

static void test_flush_backpressure_is_reported_without_rewriting() {
  int write_count = 0;
  int flush_count = 0;
  const auto outcome = dispatch_ssh_write_and_flush(
      [](auto operation) { return operation(); },
      [&]() {
        ++write_count;
        return FakeSshResult{1024, 1};
      },
      [&]() {
        ++flush_count;
        return FakeSshResult{-2, 4};
      },
      std::uint32_t{1024});
  expect(write_count == 1 && flush_count == 1,
         "flush backpressure must not repeat accepted bytes");
  expect(outcome.flushed && outcome.written == 1024 &&
             outcome.result.value == -2 && outcome.result.poll_flags == 4,
         "flush backpressure should be available for readiness waiting");
}

} // namespace elder_terms

int main() {
  elder_terms::test_complete_write_flushes_in_one_dispatch();
  elder_terms::test_partial_or_stalled_write_does_not_flush();
  elder_terms::test_flush_backpressure_is_reported_without_rewriting();
  return 0;
}

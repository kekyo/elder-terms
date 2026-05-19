#include "../../src/terminal-sessions/serial-session/serial-line-monitor.h"

#ifndef ELDER_TERMS_ENABLE_TEST_DOUBLES
#error "serial-line-monitor-test must be built with test doubles enabled"
#endif

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace elder_terms_serial_line_monitor_test {

class FakeSerialLineMonitor {
private:
  std::vector<elder_terms::SerialLineSignals> samples;
  std::size_t index = 0;

public:
  explicit FakeSerialLineMonitor(
      std::vector<elder_terms::SerialLineSignals> samples)
      : samples(std::move(samples)) {
  }

  elder_terms::SerialLineSignals read() {
    if (index >= samples.size()) {
      return {};
    }
    const elder_terms::SerialLineSignals sample = samples[index];
    ++index;
    return sample;
  }
};

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static void test_initial_low_does_not_disconnect_until_high_seen() {
  FakeSerialLineMonitor monitor({
      {.cd = false, .cts = false, .dsr = false},
      {.cd = true, .cts = false, .dsr = false},
      {.cd = false, .cts = false, .dsr = false},
  });
  elder_terms::SerialCarrierTracker tracker(
      elder_terms::SerialCarrierDetect::cd);

  expect_true(tracker.update(monitor.read()) ==
                  elder_terms::SerialCarrierEvent::none,
              "initial low CD should not disconnect");
  expect_true(tracker.update(monitor.read()) ==
                  elder_terms::SerialCarrierEvent::none,
              "high CD should arm disconnect detection");
  expect_true(tracker.update(monitor.read()) ==
                  elder_terms::SerialCarrierEvent::disconnected,
              "CD high-to-low should disconnect");
}

static void test_selected_signal_controls_disconnect() {
  FakeSerialLineMonitor monitor({
      {.cd = true, .cts = false, .dsr = false},
      {.cd = true, .cts = true, .dsr = false},
      {.cd = true, .cts = false, .dsr = false},
  });
  elder_terms::SerialCarrierTracker tracker(
      elder_terms::SerialCarrierDetect::cts);

  expect_true(tracker.update(monitor.read()) ==
                  elder_terms::SerialCarrierEvent::none,
              "unselected CD should not arm CTS detection");
  expect_true(tracker.update(monitor.read()) ==
                  elder_terms::SerialCarrierEvent::none,
              "high CTS should arm disconnect detection");
  expect_true(tracker.update(monitor.read()) ==
                  elder_terms::SerialCarrierEvent::disconnected,
              "CTS high-to-low should disconnect");
}

} // namespace elder_terms_serial_line_monitor_test

int main() {
  try {
    elder_terms_serial_line_monitor_test::
        test_initial_low_does_not_disconnect_until_high_seen();
    elder_terms_serial_line_monitor_test::
        test_selected_signal_controls_disconnect();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}

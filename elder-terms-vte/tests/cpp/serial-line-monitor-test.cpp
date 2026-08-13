#include "../../src/terminal-sessions/serial-session/serial-line-monitor.h"

#ifndef ELDER_TERMS_ENABLE_TEST_DOUBLES
#error "serial-line-monitor-test must be built with test doubles enabled"
#endif

#include <sys/ioctl.h>

#include <array>
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
      {.cd = false},
      {.cd = true},
      {.cd = false},
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
      {.cts = false, .cd = true},
      {.cts = true, .cd = true},
      {.cts = false, .cd = true},
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

static void test_ignore_never_reports_signal_disconnects() {
  FakeSerialLineMonitor monitor({
      {.cts = true, .dsr = true, .cd = true},
      {.cts = false, .dsr = false, .cd = false},
  });
  elder_terms::SerialCarrierTracker tracker(
      elder_terms::SerialCarrierDetect::ignore);

  expect_true(tracker.update(monitor.read()) ==
                  elder_terms::SerialCarrierEvent::none,
              "ignore should not arm serial signal disconnect detection");
  expect_true(tracker.update(monitor.read()) ==
                  elder_terms::SerialCarrierEvent::none,
              "ignore should keep the session across every signal drop");
}

static void test_modem_status_maps_all_indicator_lines() {
  const elder_terms::SerialLineSignals signals =
      elder_terms::serial_line_signals_from_modem_status(
          TIOCM_RTS | TIOCM_CTS | TIOCM_DTR | TIOCM_DSR | TIOCM_CAR |
          TIOCM_RNG);

  expect_true(signals.rts, "RTS status should be decoded");
  expect_true(signals.cts, "CTS status should be decoded");
  expect_true(signals.dtr, "DTR status should be decoded");
  expect_true(signals.dsr, "DSR status should be decoded");
  expect_true(signals.cd, "CD status should be decoded");
  expect_true(signals.ri, "RI status should be decoded");
}

static void test_signals_build_indicator_states() {
  const std::array<elder_terms::ActivityIndicatorState, 6> states =
      elder_terms::serial_line_indicator_states({
          .rts = true,
          .cts = false,
          .dtr = true,
          .dsr = false,
          .cd = true,
          .ri = false,
      });

  expect_true(
      states ==
          std::array<elder_terms::ActivityIndicatorState, 6>{
              elder_terms::ActivityIndicatorState{
                  .indicator = elder_terms::ActivityIndicatorId::rts,
                  .active = true},
              elder_terms::ActivityIndicatorState{
                  .indicator = elder_terms::ActivityIndicatorId::cts,
                  .active = false},
              elder_terms::ActivityIndicatorState{
                  .indicator = elder_terms::ActivityIndicatorId::dtr,
                  .active = true},
              elder_terms::ActivityIndicatorState{
                  .indicator = elder_terms::ActivityIndicatorId::dsr,
                  .active = false},
              elder_terms::ActivityIndicatorState{
                  .indicator = elder_terms::ActivityIndicatorId::cd,
                  .active = true},
              elder_terms::ActivityIndicatorState{
                  .indicator = elder_terms::ActivityIndicatorId::ri,
                  .active = false},
          },
      "serial line states should preserve indicator order and signal levels");
}

} // namespace elder_terms_serial_line_monitor_test

int main() {
  try {
    elder_terms_serial_line_monitor_test::
        test_initial_low_does_not_disconnect_until_high_seen();
    elder_terms_serial_line_monitor_test::
        test_selected_signal_controls_disconnect();
    elder_terms_serial_line_monitor_test::
        test_ignore_never_reports_signal_disconnects();
    elder_terms_serial_line_monitor_test::
        test_modem_status_maps_all_indicator_lines();
    elder_terms_serial_line_monitor_test::
        test_signals_build_indicator_states();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}

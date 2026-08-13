#include <elder-terms/serial-device-event-monitor.h>

#ifndef ELDER_TERMS_ENABLE_TEST_DOUBLES
#error "serial-device-event-monitor-test must be built with test doubles enabled"
#endif

#include <glib.h>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace elder_terms_serial_device_event_monitor_test {

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static void drain_main_context() {
  while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
  }
}

static elder_terms::SerialDeviceEventMonitor create_monitor(int *callback_count) {
  elder_terms::SerialDeviceEventMonitorOptions options;
  options.enable_system_sources = false;
  return elder_terms::SerialDeviceEventMonitor(
      "/tmp/ttyELDERTERMS0", options,
      [callback_count]() { ++(*callback_count); });
}

static void test_event_burst_is_coalesced() {
  int callback_count = 0;
  elder_terms::SerialDeviceEventMonitor monitor = create_monitor(&callback_count);
  monitor.start();

  monitor.notify_device_event_for_test();
  monitor.notify_device_event_for_test();
  monitor.notify_device_event_for_test();
  drain_main_context();
  monitor.stop();

  expect_true(callback_count == 1,
              "serial device event burst should coalesce to one callback");
}

static void test_disabled_system_sources_report_no_event_sources() {
  int callback_count = 0;
  elder_terms::SerialDeviceEventMonitor monitor = create_monitor(&callback_count);
  monitor.start();

  expect_true(!monitor.has_event_sources(),
              "disabled serial device monitor should report no event sources");
  monitor.stop();
}

static void test_stopped_monitor_does_not_emit_pending_callback() {
  int callback_count = 0;
  elder_terms::SerialDeviceEventMonitor monitor = create_monitor(&callback_count);
  monitor.start();

  monitor.notify_device_event_for_test();
  monitor.stop();
  drain_main_context();

  expect_true(callback_count == 0,
              "stopped serial device monitor should cancel pending callback");
}

static void test_destroyed_monitor_does_not_emit_pending_callback() {
  int callback_count = 0;
  {
    elder_terms::SerialDeviceEventMonitor monitor = create_monitor(&callback_count);
    monitor.start();
    monitor.notify_device_event_for_test();
  }
  drain_main_context();

  expect_true(callback_count == 0,
              "destroyed serial device monitor should cancel pending callback");
}

} // namespace elder_terms_serial_device_event_monitor_test

int main() {
  try {
    elder_terms_serial_device_event_monitor_test::
        test_event_burst_is_coalesced();
    elder_terms_serial_device_event_monitor_test::
        test_disabled_system_sources_report_no_event_sources();
    elder_terms_serial_device_event_monitor_test::
        test_stopped_monitor_does_not_emit_pending_callback();
    elder_terms_serial_device_event_monitor_test::
        test_destroyed_monitor_does_not_emit_pending_callback();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}

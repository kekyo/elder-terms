#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace elder_terms {

/**
 * Callback emitted when serial device availability may have changed.
 */
using SerialDeviceEventCallback = std::function<void()>;

/**
 * Options controlling serial device event monitoring.
 */
struct SerialDeviceEventMonitorOptions {
  /** Root used for Linux device files. */
  std::filesystem::path dev_root = "/dev";
#ifdef ELDER_TERMS_ENABLE_TEST_DOUBLES
  /** Enables real udev and filesystem sources in tests. */
  bool enable_system_sources = true;
#endif
};

/**
 * Event-driven monitor for serial device add/remove activity.
 */
class SerialDeviceEventMonitor {
public:
  /**
   * Creates a monitor for a serial device selector.
   *
   * @param selector Value of [serial] device.
   * @param callback Callback invoked after relevant udev or filesystem events.
   */
  SerialDeviceEventMonitor(std::string selector,
                           SerialDeviceEventCallback callback);

  /**
   * Creates a monitor for a serial device selector with custom roots.
   *
   * @param selector Value of [serial] device.
   * @param options Monitor options.
   * @param callback Callback invoked after relevant udev or filesystem events.
   */
  SerialDeviceEventMonitor(std::string selector,
                           SerialDeviceEventMonitorOptions options,
                           SerialDeviceEventCallback callback);

  /**
   * Stops the monitor and releases resources.
   */
  ~SerialDeviceEventMonitor();

  SerialDeviceEventMonitor(const SerialDeviceEventMonitor &) = delete;
  SerialDeviceEventMonitor &
  operator=(const SerialDeviceEventMonitor &) = delete;

  /**
   * Starts udev and filesystem monitoring.
   */
  void start();

  /**
   * Stops monitoring and cancels pending callbacks.
   */
  void stop();

#ifdef ELDER_TERMS_ENABLE_TEST_DOUBLES
  /**
   * Injects one device event for tests.
   */
  void notify_device_event_for_test();
#endif

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace elder_terms

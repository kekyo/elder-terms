#pragma once

#include <functional>
#include <memory>
#include <string>

#include <elder-terms/export.h>
#include <elder-terms/serial-device.h>

namespace elder_terms {

/**
 * Callback emitted when serial device availability may have changed.
 */
using SerialDeviceEventCallback = std::function<void()>;

/**
 * Options controlling serial device event monitoring.
 */
struct SerialDeviceEventMonitorOptions {
  /** Filesystem roots used by serial device discovery. */
  SerialDevicePaths paths;
#ifdef ELDER_TERMS_ENABLE_TEST_DOUBLES
  /** Enables real udev and filesystem sources in tests. */
  bool enable_system_sources = true;
#endif
};

/**
 * Event-driven monitor for serial device add/remove activity.
 */
class ELDER_TERMS_API SerialDeviceEventMonitor {
public:
  /**
   * Creates a monitor for all serial device choices.
   *
   * @param callback Callback invoked after relevant udev or filesystem events.
   */
  explicit SerialDeviceEventMonitor(SerialDeviceEventCallback callback);

  /**
   * Creates a monitor for all serial device choices with custom roots.
   *
   * @param options Monitor options.
   * @param callback Callback invoked after relevant udev or filesystem events.
   */
  SerialDeviceEventMonitor(SerialDeviceEventMonitorOptions options,
                           SerialDeviceEventCallback callback);

  /**
   * Creates a monitor for one configured serial device target.
   *
   * @param target Configured serial device target.
   * @param callback Callback invoked after relevant udev or filesystem events.
   */
  SerialDeviceEventMonitor(std::string target,
                           SerialDeviceEventCallback callback);

  /**
   * Creates a monitor for one configured serial device target with custom roots.
   *
   * @param target Configured serial device target.
   * @param options Monitor options.
   * @param callback Callback invoked after relevant udev or filesystem events.
   */
  SerialDeviceEventMonitor(std::string target,
                           SerialDeviceEventMonitorOptions options,
                           SerialDeviceEventCallback callback);

  /** Stops the monitor and releases resources. */
  ~SerialDeviceEventMonitor();

  SerialDeviceEventMonitor(const SerialDeviceEventMonitor &) = delete;
  SerialDeviceEventMonitor &
  operator=(const SerialDeviceEventMonitor &) = delete;

  /** Starts udev and filesystem monitoring. */
  void start();

  /** Stops monitoring and cancels pending callbacks. */
  void stop();

  /**
   * Returns whether at least one event source is active.
   *
   * @returns True when udev or filesystem events can wake the monitor.
   */
  bool has_event_sources() const;

#ifdef ELDER_TERMS_ENABLE_TEST_DOUBLES
  /** Injects one device event for tests. */
  void notify_device_event_for_test();
#endif

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace elder_terms

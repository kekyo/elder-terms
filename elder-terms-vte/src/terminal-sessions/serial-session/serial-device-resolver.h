#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace elder_terms {

/**
 * Options controlling serial device resolution.
 */
struct SerialDeviceResolverOptions {
  /** Root used for Linux device files. */
  std::filesystem::path dev_root = "/dev";
  /** Root used for Linux sysfs. */
  std::filesystem::path sysfs_root = "/sys";
};

/**
 * Result of resolving a serial device selector.
 */
struct SerialDeviceResolveResult {
  /** True when a single device was resolved. */
  bool resolved = false;
  /** Device path to open when resolved is true. */
  std::filesystem::path path;
  /** Non-fatal warnings produced during resolution. */
  std::vector<std::string> warnings;
};

/**
 * Resolves a serial device selector using the configured Linux lookup order.
 *
 * @param selector Value of [serial] device.
 * @param options Resolver roots.
 * @returns Resolved path or warnings explaining why no path was selected.
 */
SerialDeviceResolveResult
resolve_serial_device(const std::string &selector,
                      const SerialDeviceResolverOptions &options);

/**
 * Resolves a serial device selector using the host /dev and /sys roots.
 *
 * @param selector Value of [serial] device.
 * @returns Resolved path or warnings explaining why no path was selected.
 */
SerialDeviceResolveResult resolve_serial_device(const std::string &selector);

} // namespace elder_terms

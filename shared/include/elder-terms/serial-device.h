#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * Strategy used to identify a serial device across detach and reattach events.
 */
enum class SerialDeviceMatchMode {
  /** Uses the concrete device node name, such as /dev/ttyUSB0. */
  exact_path,
  /** Uses a stable device identity under /dev/serial/by-id. */
  stable_id,
  /** Uses a physical connection point under /dev/serial/by-path. */
  physical_port,
};

/**
 * Filesystem roots used for serial device discovery and resolution.
 */
struct SerialDevicePaths {
  /** Root containing concrete Linux device nodes. */
  std::filesystem::path dev_root = "/dev";
  /** Root containing stable serial identity links. */
  std::filesystem::path by_id_root = "/dev/serial/by-id";
  /** Root containing physical serial port links. */
  std::filesystem::path by_path_root = "/dev/serial/by-path";
  /** Root containing sysfs tty entries. */
  std::filesystem::path sys_class_tty_root = "/sys/class/tty";
};

/**
 * One serial device target available for selection.
 */
struct SerialDeviceChoice {
  /** Path persisted for the selected identification mode. */
  std::string target_path;
  /** User-facing label for the target. */
  std::string display_label;
  /** Concrete device node currently reached by the target. */
  std::optional<std::string> current_node;
  /** USB serial number discovered from sysfs, when present. */
  std::optional<std::string> usb_serial;
};

/**
 * Result of resolving a configured serial target.
 */
struct SerialDeviceResolveResult {
  /** True when exactly one usable target was resolved. */
  bool resolved = false;
  /** Path that should be opened when resolved is true. */
  std::filesystem::path path;
  /** Non-fatal warnings explaining an unresolved target. */
  std::vector<std::string> warnings;
};

/**
 * Returns the stable INI representation of a serial match mode.
 *
 * @param mode Match mode to serialize.
 * @returns `path`, `by-id`, or `by-path`.
 */
ELDER_TERMS_API std::string
serial_device_match_mode_to_string(SerialDeviceMatchMode mode);

/**
 * Parses a serial match mode from its INI representation.
 *
 * @param value Value to parse.
 * @returns Parsed mode, or std::nullopt for an unknown value.
 */
ELDER_TERMS_API std::optional<SerialDeviceMatchMode>
parse_serial_device_match_mode(const std::string &value);

/**
 * Returns host discovery roots, with supported environment overrides applied.
 *
 * @returns Serial discovery roots for the current process.
 */
ELDER_TERMS_API SerialDevicePaths host_serial_device_paths();

/**
 * Lists selectable serial devices for one identification mode.
 *
 * @param mode Identification mode to enumerate.
 * @param paths Filesystem roots used for discovery.
 * @returns Available targets sorted by display label and target path.
 */
ELDER_TERMS_API std::vector<SerialDeviceChoice>
list_serial_device_choices(SerialDeviceMatchMode mode,
                           const SerialDevicePaths &paths);

/**
 * Lists selectable serial devices using the host discovery roots.
 *
 * @param mode Identification mode to enumerate.
 * @returns Available targets sorted by display label and target path.
 */
ELDER_TERMS_API std::vector<SerialDeviceChoice>
list_serial_device_choices(SerialDeviceMatchMode mode);

/**
 * Resolves the current concrete device node behind a stored target.
 *
 * @param target_path Stored device target, which may be a symbolic link.
 * @returns Current canonical node, or std::nullopt when absent.
 */
ELDER_TERMS_API std::optional<std::string>
resolve_serial_device_current_node(const std::string &target_path);

/**
 * Converts a serial target to the target representing the same current device
 * under another identification mode.
 *
 * @param mode Desired identification mode.
 * @param target_path Existing stored target.
 * @param paths Filesystem roots used for discovery.
 * @returns Converted target. A target already belonging to mode is retained
 * even while absent.
 */
ELDER_TERMS_API std::optional<std::string>
resolve_serial_device_target_for_mode(SerialDeviceMatchMode mode,
                                      const std::string &target_path,
                                      const SerialDevicePaths &paths);

/**
 * Converts a serial target using the host discovery roots.
 *
 * @param mode Desired identification mode.
 * @param target_path Existing stored target.
 * @returns Converted target, or std::nullopt when no corresponding target
 * exists.
 */
ELDER_TERMS_API std::optional<std::string>
resolve_serial_device_target_for_mode(SerialDeviceMatchMode mode,
                                      const std::string &target_path);

/**
 * Resolves the current path to open for a configured serial identity.
 *
 * @param mode Configured identification mode.
 * @param target_path Persisted serial target.
 * @param usb_serial Remembered USB serial used to recover renamed stable IDs.
 * @param paths Filesystem roots used for discovery.
 * @returns Resolved target or warnings describing the failure.
 *
 * @remarks USB serial matching applies only to stable_id. Ambiguous serial
 * matches are rejected instead of choosing an arbitrary device.
 */
ELDER_TERMS_API SerialDeviceResolveResult resolve_serial_device(
    SerialDeviceMatchMode mode, const std::string &target_path,
    const std::optional<std::string> &usb_serial,
    const SerialDevicePaths &paths);

/**
 * Resolves the current path to open using the host discovery roots.
 *
 * @param mode Configured identification mode.
 * @param target_path Persisted serial target.
 * @param usb_serial Remembered USB serial used to recover renamed stable IDs.
 * @returns Resolved target or warnings describing the failure.
 */
ELDER_TERMS_API SerialDeviceResolveResult resolve_serial_device(
    SerialDeviceMatchMode mode, const std::string &target_path,
    const std::optional<std::string> &usb_serial);

/**
 * Checks whether a serial target can be opened as a tty.
 *
 * @param target_path Target path to inspect.
 * @returns True when termios attributes can be read from the target.
 */
ELDER_TERMS_API bool
can_open_serial_device_target(const std::string &target_path);

} // namespace elder_terms

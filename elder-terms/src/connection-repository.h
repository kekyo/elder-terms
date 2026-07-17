#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <elder-terms/settings.h>

namespace elder_terms {

/**
 * Identifies one persistent connection INI file.
 */
struct ConnectionProfile {
  /** User-visible connection name. */
  std::string name;
  /** Persistent INI path. */
  std::filesystem::path path;
};

/**
 * Result of validating and normalizing a connection name.
 */
struct ConnectionNameValidationResult {
  /** True when the normalized name can be used. */
  bool valid = false;
  /** Connection name with surrounding whitespace removed. */
  std::string name;
  /** Human-readable validation failure. */
  std::string error;
};

/**
 * Result of persisting a connection profile.
 */
struct ConnectionSaveResult {
  /** True when the profile was persisted completely. */
  bool saved = false;
  /** Final INI path when saved. */
  std::filesystem::path path;
  /** Non-fatal or failure diagnostics. */
  std::vector<std::string> warnings;
};

/**
 * Returns the XDG connection profile directory.
 *
 * @returns `$XDG_CONFIG_HOME/elder-terms/connections`, using GLib's fallback
 * when XDG_CONFIG_HOME is unset.
 */
std::filesystem::path default_connection_directory();

/**
 * Lists regular `.ini` connection profiles in ascending name order.
 *
 * @param directory Connection profile directory.
 * @returns Discovered connection profiles, or an empty list when absent.
 */
std::vector<ConnectionProfile>
list_connection_profiles(const std::filesystem::path &directory);

/**
 * Validates a proposed connection name.
 *
 * @param candidate User-provided connection name.
 * @param profiles Existing profiles checked for collisions.
 * @param current_path Existing profile path allowed to retain its own name.
 * @returns Normalized name and validation status.
 */
ConnectionNameValidationResult validate_connection_name(
    const std::string &candidate,
    const std::vector<ConnectionProfile> &profiles,
    const std::optional<std::filesystem::path> &current_path);

/**
 * Loads one connection profile with standard defaults.
 *
 * @param path Persistent INI path.
 * @returns Loaded settings and read status.
 */
SettingsLoadResult load_connection_profile(const std::filesystem::path &path);

/**
 * Atomically saves a new or existing connection profile.
 *
 * @param directory Connection profile directory.
 * @param original_path Existing INI path, or nullopt for a new profile.
 * @param name Proposed connection name.
 * @param store Settings draft to persist.
 * @returns Save status, final path, and diagnostics.
 */
ConnectionSaveResult save_connection_profile(
    const std::filesystem::path &directory,
    const std::optional<std::filesystem::path> &original_path,
    const std::string &name, const SettingsStore &store);

} // namespace elder_terms

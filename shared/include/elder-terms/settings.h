#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <elder-terms/settings/general-settings.h>
#include <elder-terms/settings/local-session-settings.h>
#include <elder-terms/settings/serial-settings.h>
#include <elder-terms/settings/settings-store.h>
#include <elder-terms/settings/telnet-settings.h>
#include <elder-terms/settings/terminal-settings.h>

namespace elder_terms {

/**
 * Identifies the terminal connection backend.
 */
enum class TerminalConnectionKind {
  /** Spawns the user's local shell in VTE. */
  local_shell,
  /** Connects to a remote TELNET server. */
  telnet,
  /** Connects to a serial device. */
  serial,
};

/**
 * Backend-specific terminal connection settings.
 */
using TerminalConnectionSettings =
  std::variant<LocalShellConnectionSettings, TelnetConnectionSettings,
               SerialConnectionSettings>;

/**
 * Describes how the terminal session should connect to a backend.
 */
struct TerminalConnectionProfile {
  /** Selected connection backend. */
  TerminalConnectionKind kind = TerminalConnectionKind::local_shell;
  /** Backend-specific settings. */
  TerminalConnectionSettings settings = LocalShellConnectionSettings{};
};

/**
 * Paths used to build the initial settings store.
 */
struct SettingsLoadOptions {
  /** Optional persistent INI path passed with -c. */
  std::optional<std::filesystem::path> config_path;
  /** Optional read-only startup INI path passed with -s. */
  std::optional<std::filesystem::path> startup_config_path;
};

/**
 * Result of loading settings from disk.
 */
struct SettingsLoadResult {
  /** Loaded store, with defaults applied for missing or invalid values. */
  SettingsStore store;
  /** Non-fatal warnings encountered while loading. */
  std::vector<std::string> warnings;
};

/**
 * Result of saving settings to disk.
 */
struct SettingsSaveResult {
  /** True when the target INI file was written successfully. */
  bool saved = false;
  /** Non-fatal warnings encountered while saving. */
  std::vector<std::string> warnings;
};

/**
 * Creates a settings store initialized with application defaults.
 *
 * @param terminal_defaults Default terminal display settings.
 * @returns Settings store containing all registered keys.
 */
SettingsStore create_default_settings(TerminalDisplaySettings terminal_defaults);

/**
 * Loads settings from optional INI files.
 *
 * @param options Persistent and startup configuration paths.
 * @param default_terminal_zoom VTE's runtime default font scale.
 * @returns Loaded settings and non-fatal warnings.
 */
SettingsLoadResult load_settings(const SettingsLoadOptions &options,
                                 gdouble default_terminal_zoom);

/**
 * Saves settings to an INI file.
 *
 * @param store Settings to serialize.
 * @param config_path Target INI path.
 * @returns Save status and warnings.
 *
 * @remarks Values equal to their registered defaults are omitted.
 */
SettingsSaveResult save_settings(const SettingsStore &store,
                                 const std::filesystem::path &config_path);

/**
 * Extracts the terminal connection profile from a store.
 *
 * @param store Source settings store.
 * @returns Terminal connection profile.
 */
TerminalConnectionProfile terminal_connection_profile(const SettingsStore &store);

} // namespace elder_terms

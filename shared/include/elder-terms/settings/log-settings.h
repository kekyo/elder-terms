#pragma once

#include <string>
#include <vector>

#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Selects which received terminal bytes are written to a log file.
 */
enum class TerminalLogMode {
  /** Bytes received from the terminal backend before character conversion. */
  raw,
  /** UTF-8 text produced by terminal character conversion. */
  cooked,
};

/**
 * Effective terminal stream logging settings.
 */
struct TerminalLogSettings {
  /** True when a log file should be open while the backend is connected. */
  bool enabled = false;
  /** Directory beneath which formatted log paths are created. A leading
   * `{XDG_DOCUMENTS}` resolves to the XDG Documents directory, falling back
   * to the user home directory, and a leading `$HOME` resolves directly to
   * the user home directory. */
  std::string base_directory = "{XDG_DOCUMENTS}/logs/";
  /** Relative path format evaluated for each connection. */
  std::string file_name_format = "{YYYYMMDD}_{hhmmss}_{fff}.txt";
  /** Received byte representation written to the log. */
  TerminalLogMode mode = TerminalLogMode::raw;

  /** Compares every effective terminal log setting. */
  bool operator==(const TerminalLogSettings &) const = default;
};

/**
 * Returns the setting key for [log] enabled.
 *
 * @returns Setting key for terminal logging enablement.
 */
SettingKey terminal_log_enabled_setting_key();

/**
 * Returns the setting key for [log] base_directory.
 *
 * @returns Setting key for the terminal log base directory.
 */
SettingKey terminal_log_base_directory_setting_key();

/**
 * Returns the setting key for [log] file_name_format.
 *
 * @returns Setting key for the relative log path format.
 */
SettingKey terminal_log_file_name_format_setting_key();

/**
 * Returns the setting key for [log] mode.
 *
 * @returns Setting key for the raw/cooked log mode.
 */
SettingKey terminal_log_mode_setting_key();

/**
 * Validates a relative terminal log path format.
 *
 * @param format Candidate format containing optional `{YYYYMMDD}`, `{hhmmss}`,
 * and `{fff}` placeholders.
 * @param reason Receives a human-readable validation failure reason.
 * @returns True when the format resolves beneath the configured base
 * directory and names a file.
 */
bool terminal_log_file_name_format_is_valid(const std::string &format,
                                            std::string *reason);

/**
 * Returns the INI value for a terminal log mode.
 *
 * @param mode Terminal log mode.
 * @returns Stable setting value.
 */
const char *terminal_log_mode_to_string(TerminalLogMode mode);

/**
 * Returns terminal log setting definitions.
 *
 * @returns Setting definitions for the log INI section.
 */
std::vector<SettingDefinition> terminal_log_setting_definitions();

/**
 * Extracts terminal log settings from a store.
 *
 * @param store Source settings store.
 * @returns Effective terminal log settings.
 */
TerminalLogSettings terminal_log_settings(const SettingsStore &store);

} // namespace elder_terms

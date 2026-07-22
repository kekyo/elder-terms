#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <elder-terms/export.h>
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
  /** Directory format beneath which log paths are created. Named and temporal
   * placeholders may appear anywhere. `{documents}` and `{downloads}` use
   * their XDG user directories, falling back to `$HOME/Documents` and
   * `$HOME/Downloads`; `{home}` uses the home directory; and `{name}` uses the
   * sanitized connection name. */
  std::string base_directory = "{documents}/logs/";
  /** Path format evaluated for each connection using the same named and
   * temporal placeholders as the base directory. */
  std::string file_name_format = "{YYYYMMDD}_{hhmmss}_{fff}.txt";
  /** Effective connection name used by the `{name}` placeholder. */
  std::string connection_name = "elder-terms";
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
ELDER_TERMS_API SettingKey terminal_log_enabled_setting_key();

/**
 * Returns the setting key for [log] base_directory.
 *
 * @returns Setting key for the terminal log base directory.
 */
ELDER_TERMS_API SettingKey terminal_log_base_directory_setting_key();

/**
 * Returns the setting key for [log] file_name_format.
 *
 * @returns Setting key for the relative log path format.
 */
ELDER_TERMS_API SettingKey terminal_log_file_name_format_setting_key();

/**
 * Returns the setting key for [log] mode.
 *
 * @returns Setting key for the raw/cooked log mode.
 */
ELDER_TERMS_API SettingKey terminal_log_mode_setting_key();

/**
 * Validates a terminal log file path format.
 *
 * @param format Candidate format containing `{documents}`, `{downloads}`,
 * `{home}`, `{name}`, and the temporal fields `YYYY`, `MM`, `DD`, `hh`, `mm`,
 * `ss`, and `fff`. Temporal fields may be placed in separate braces or
 * combined with punctuation separators inside one pair of braces.
 * @param reason Receives a human-readable validation failure reason.
 * @returns True when the format has valid placeholders, contains no parent
 * directory traversal, and names a file.
 */
ELDER_TERMS_API bool
terminal_log_file_name_format_is_valid(const std::string &format,
                                       std::string *reason);

/**
 * Resolves an effective terminal log path.
 *
 * @param settings Effective terminal log settings.
 * @param now Timestamp used by date and time placeholders.
 * @returns Normalized path produced from the base and file name formats.
 *
 * @throws std::invalid_argument if the file name format is invalid.
 * @throws std::runtime_error if the local time or user home cannot be
 * resolved.
 */
ELDER_TERMS_API std::filesystem::path resolve_terminal_log_path(
    const TerminalLogSettings &settings,
    std::chrono::system_clock::time_point now);

/**
 * Returns the INI value for a terminal log mode.
 *
 * @param mode Terminal log mode.
 * @returns Stable setting value.
 */
ELDER_TERMS_API const char *
terminal_log_mode_to_string(TerminalLogMode mode);

/**
 * Returns terminal log setting definitions.
 *
 * @returns Setting definitions for the log INI section.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
terminal_log_setting_definitions();

/**
 * Extracts terminal log settings from a store.
 *
 * @param store Source settings store.
 * @returns Effective terminal log settings.
 */
ELDER_TERMS_API TerminalLogSettings
terminal_log_settings(const SettingsStore &store);

} // namespace elder_terms

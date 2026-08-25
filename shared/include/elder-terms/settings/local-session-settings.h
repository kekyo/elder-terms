#pragma once

#include <string>
#include <vector>

#include <elder-terms/export.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Settings for the local shell connection backend.
 */
struct LocalShellConnectionSettings {
  /** Parsed process argument vector, or empty to select the user's shell. */
  std::vector<std::string> argv;
};

/**
 * Returns local shell setting definitions.
 *
 * @returns Setting definitions for the local INI section.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
local_shell_connection_setting_definitions();

/**
 * Returns the setting key for [local] command_line.
 *
 * @returns Setting key for the local startup command line.
 */
ELDER_TERMS_API SettingKey local_command_line_setting_key();

/**
 * Checks whether a local startup command line has valid shell-style quoting.
 *
 * @param command_line Candidate command line. A blank value selects the user's
 * shell.
 * @param reason Receives a human-readable validation failure reason.
 * @returns True when the value is blank or can be split into process arguments.
 *
 * @remarks Quoting and backslash escapes are parsed, but variables, wildcards,
 * redirections, and pipelines are not expanded.
 */
ELDER_TERMS_API bool
local_command_line_is_valid(const std::string &command_line,
                            std::string *reason);

/**
 * Extracts local shell connection settings from a store.
 *
 * @param store Source settings store.
 * @returns Typed local shell connection settings.
 */
ELDER_TERMS_API LocalShellConnectionSettings
local_shell_connection_settings(const SettingsStore &store);

} // namespace elder_terms

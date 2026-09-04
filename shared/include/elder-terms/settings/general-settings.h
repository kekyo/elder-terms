#pragma once

#include <optional>
#include <string>
#include <vector>

#include <elder-terms/export.h>
#include <elder-terms/key-binding.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Identifies the configured connection type.
 */
enum class ConnectionKind {
  /** Runs the local shell in a terminal. */
  local_shell,
  /** Connects to a TELNET server in a terminal. */
  telnet,
  /** Connects to an SSH server in a terminal. */
  ssh,
  /** Connects to a serial device in a terminal. */
  serial,
  /** Browses and transfers files over SFTP without a terminal. */
  sftp,
  /** Browses and transfers files over unencrypted FTP without a terminal. */
  ftp,
};

/**
 * An opaque RGB color.
 */
struct RgbColor {
  /** Red channel. */
  guint8 red;
  /** Green channel. */
  guint8 green;
  /** Blue channel. */
  guint8 blue;
};

/**
 * Optional colors applied to a connection window.
 */
struct GeneralColorSettings {
  /** Shared title-bar and status-bar background, or no application color. */
  std::optional<RgbColor> exterior_background;
  /** Main content background, or no application color. */
  std::optional<RgbColor> background;
};

/**
 * Returns connection-specific general setting definitions.
 *
 * @param default_connection_name Connection name used when [general] name is
 * absent or blank.
 * @returns Setting definitions for the general INI section.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
general_setting_definitions(std::string default_connection_name);

/**
 * Returns the setting key for [general] name.
 *
 * @returns Setting key for the configured connection name.
 */
ELDER_TERMS_API SettingKey general_name_setting_key();

/**
 * Returns the setting key for [general] type.
 *
 * @returns Setting key for the selected connection type.
 */
ELDER_TERMS_API SettingKey general_type_setting_key();

/**
 * Returns the setting key for [general] open_connection.
 *
 * @returns Setting key for the connection launch hotkey.
 */
ELDER_TERMS_API SettingKey
general_open_connection_hotkey_setting_key();

/**
 * Returns the setting key for [general] exterior_background.
 *
 * @returns Setting key for the title-bar and status-bar background.
 */
ELDER_TERMS_API SettingKey
general_exterior_background_setting_key();

/**
 * Returns the setting key for [general] background.
 *
 * @returns Setting key for the main content background.
 */
ELDER_TERMS_API SettingKey general_background_setting_key();

/**
 * Returns the effective connection name.
 *
 * @param store Source settings store.
 * @returns Explicit [general] name, or the store's path-derived default when
 * the configured value is absent or blank.
 */
ELDER_TERMS_API std::string
general_connection_name(const SettingsStore &store);

/**
 * Returns the selected connection type.
 *
 * @param store Source settings store.
 * @returns Configured connection type, or local shell for an unavailable
 * value.
 */
ELDER_TERMS_API ConnectionKind
general_connection_kind(const SettingsStore &store);

/**
 * Extracts the effective connection launch hotkey text.
 *
 * @param store Source connection settings store.
 * @returns Configured key binding, or an empty string when disabled.
 */
ELDER_TERMS_API std::string
general_open_connection_hotkey_text(const SettingsStore &store);

/**
 * Extracts the parsed connection launch hotkey.
 *
 * @param store Source connection settings store.
 * @returns Parsed hotkey, or no value when disabled.
 */
ELDER_TERMS_API std::optional<KeyBinding>
general_open_connection_hotkey(const SettingsStore &store);

/**
 * Extracts connection window colors from a store.
 *
 * @param store Source settings store.
 * @returns Parsed RGB colors. A missing value denotes no application color.
 */
ELDER_TERMS_API GeneralColorSettings
general_color_settings(const SettingsStore &store);

/**
 * Checks whether the loaded general settings select the TELNET backend.
 *
 * @param store Source settings store.
 * @returns True when [general] type is telnet.
 */
ELDER_TERMS_API bool
general_settings_select_telnet_connection(const SettingsStore &store);

/**
 * Checks whether the loaded general settings select the SSH backend.
 *
 * @param store Source settings store.
 * @returns True when [general] type is ssh.
 */
ELDER_TERMS_API bool
general_settings_select_ssh_connection(const SettingsStore &store);

/**
 * Checks whether the loaded general settings select the SFTP backend.
 *
 * @param store Source settings store.
 * @returns True when [general] type is sftp.
 */
ELDER_TERMS_API bool
general_settings_select_sftp_connection(const SettingsStore &store);

/**
 * Checks whether the loaded general settings select the FTP backend.
 *
 * @param store Source settings store.
 * @returns True when [general] type is ftp.
 */
ELDER_TERMS_API bool
general_settings_select_ftp_connection(const SettingsStore &store);

/**
 * Checks whether the loaded general settings select the serial backend.
 *
 * @param store Source settings store.
 * @returns True when [general] type is serial.
 */
ELDER_TERMS_API bool
general_settings_select_serial_connection(const SettingsStore &store);

} // namespace elder_terms

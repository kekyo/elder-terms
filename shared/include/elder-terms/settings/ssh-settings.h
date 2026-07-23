#pragma once

#include <string>
#include <vector>

#include <glib.h>

#include <elder-terms/export.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Settings for the SSH connection backend.
 */
struct SshConnectionSettings {
  /** SSH server address or hostname. */
  std::string address;
  /** SSH server TCP port. */
  gint64 port;
  /** Remote username, or empty to use the local username. */
  std::string username;
  /** Preferred private-key identity, or empty for automatic discovery. */
  std::string identity_file;
  /** Terminal type sent with the remote PTY request. */
  std::string terminal_type;
};

/**
 * Returns SSH setting definitions.
 *
 * @returns Setting definitions for the ssh INI section.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
ssh_connection_setting_definitions();

/**
 * Returns the setting key for [ssh] address.
 *
 * @returns Setting key for the SSH server address.
 */
ELDER_TERMS_API SettingKey ssh_address_setting_key();

/**
 * Returns the setting key for [ssh] port.
 *
 * @returns Setting key for the SSH server port.
 */
ELDER_TERMS_API SettingKey ssh_port_setting_key();

/**
 * Returns the setting key for [ssh] username.
 *
 * @returns Setting key for the SSH username.
 */
ELDER_TERMS_API SettingKey ssh_username_setting_key();

/**
 * Returns the setting key for [ssh] identity_file.
 *
 * @returns Setting key for the preferred SSH private-key identity.
 */
ELDER_TERMS_API SettingKey ssh_identity_file_setting_key();

/**
 * Returns the setting key for [ssh] terminal_type.
 *
 * @returns Setting key for the SSH remote PTY terminal type.
 */
ELDER_TERMS_API SettingKey ssh_terminal_type_setting_key();

/**
 * Extracts SSH connection settings from a store.
 *
 * @param store Source settings store.
 * @returns Typed SSH connection settings.
 */
ELDER_TERMS_API SshConnectionSettings
ssh_connection_settings(const SettingsStore &store);

/**
 * Appends SSH-specific non-fatal warnings.
 *
 * @param store Source settings store.
 * @param warnings Warning sink.
 */
ELDER_TERMS_API void
append_ssh_connection_warnings(const SettingsStore &store,
                               std::vector<std::string> *warnings);

} // namespace elder_terms

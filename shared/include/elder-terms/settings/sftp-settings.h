#pragma once

#include <string>
#include <vector>

#include <elder-terms/export.h>
#include <elder-terms/settings/settings-store.h>
#include <elder-terms/settings/ssh-settings.h>

namespace elder_terms {

/**
 * Settings for the SFTP file transfer backend.
 */
struct SftpConnectionSettings {
  /** SSH server endpoint and login settings. */
  SshEndpointSettings endpoint;
  /** Initial local directory, or empty to use the runtime fallback. */
  std::string local_directory;
  /** Initial remote directory. */
  std::string remote_directory;
};

/**
 * Returns SFTP setting definitions.
 *
 * @returns Setting definitions for the sftp INI section.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
sftp_connection_setting_definitions();

/**
 * Returns the setting key for [sftp] local_directory.
 *
 * @returns Setting key for the initial local directory.
 */
ELDER_TERMS_API SettingKey sftp_local_directory_setting_key();

/**
 * Returns the setting key for [sftp] remote_directory.
 *
 * @returns Setting key for the initial remote directory.
 */
ELDER_TERMS_API SettingKey sftp_remote_directory_setting_key();

/**
 * Extracts SFTP connection settings from a store.
 *
 * @param store Source settings store.
 * @returns Typed SFTP connection settings.
 */
ELDER_TERMS_API SftpConnectionSettings
sftp_connection_settings(const SettingsStore &store);

} // namespace elder_terms

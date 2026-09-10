#pragma once

#include <string>
#include <vector>

#include <glib.h>

#include <elder-terms/export.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/** FTP data-channel establishment strategy. */
enum class FtpDataConnectionMode {
  /** The client connects to a server-selected data endpoint. */
  passive,
  /** The client listens for a server-initiated data connection. */
  active,
};

/** FTP transport encryption. */
enum class FtpTlsMode {
  /** Plain FTP. */
  none,
  /** Upgrade the FTP control connection before authentication. */
  explicit_tls,
  /** Start TLS immediately after establishing TCP. */
  implicit_tls,
};

/** Settings for the FTP and FTPS file transfer backend. */
struct FtpConnectionSettings {
  /** FTP server address or hostname. */
  std::string address;
  /** FTP control connection TCP port. */
  gint64 port;
  /** Remote username, or empty to prefill the current operating-system user. */
  std::string username;
  /** Data-channel establishment strategy. */
  FtpDataConnectionMode data_connection_mode =
      FtpDataConnectionMode::passive;
  /** Initial local directory, or empty to use the runtime fallback. */
  std::string local_directory;
  /** Initial remote directory. */
  std::string remote_directory;
  /** Control and data transport encryption. */
  FtpTlsMode tls_mode = FtpTlsMode::none;
  /** Absolute PEM CA bundle, or empty to use the system trust store. */
  std::string ca_file{};
  /** Invalid effective settings, including their keys and sources. */
  std::vector<std::string> validation_errors{};
};

/**
 * Returns the stable INI value for an FTP data connection mode.
 *
 * @param mode FTP data connection mode.
 * @returns `passive` or `active`.
 */
ELDER_TERMS_API const char *
ftp_data_connection_mode_to_string(FtpDataConnectionMode mode);

/**
 * Returns FTP setting definitions.
 *
 * @returns Setting definitions for the ftp INI section.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
ftp_connection_setting_definitions();

/**
 * Returns the INI spelling of an FTP TLS mode.
 * @param mode Transport encryption mode.
 * @returns none, explicit, or implicit.
 */
ELDER_TERMS_API const char *ftp_tls_mode_to_string(FtpTlsMode mode);

/** @returns Setting key for [ftp] tls_mode. */
ELDER_TERMS_API SettingKey ftp_tls_mode_setting_key();
/** @returns Setting key for [ftp] ca_file. */
ELDER_TERMS_API SettingKey ftp_ca_file_setting_key();

/** @returns Setting key for [ftp] address. */
ELDER_TERMS_API SettingKey ftp_address_setting_key();

/** @returns Setting key for [ftp] port. */
ELDER_TERMS_API SettingKey ftp_port_setting_key();

/** @returns Setting key for [ftp] username. */
ELDER_TERMS_API SettingKey ftp_username_setting_key();

/** @returns Setting key for [ftp] data_connection_mode. */
ELDER_TERMS_API SettingKey ftp_data_connection_mode_setting_key();

/** @returns Setting key for [ftp] local_directory. */
ELDER_TERMS_API SettingKey ftp_local_directory_setting_key();

/** @returns Setting key for [ftp] remote_directory. */
ELDER_TERMS_API SettingKey ftp_remote_directory_setting_key();

/**
 * Extracts FTP connection settings from a store.
 *
 * @param store Source settings store.
 * @returns Typed FTP connection settings.
 */
ELDER_TERMS_API FtpConnectionSettings
ftp_connection_settings(const SettingsStore &store);

/**
 * Appends FTP-specific non-fatal warnings.
 *
 * @param store Source settings store.
 * @param warnings Warning sink.
 */
ELDER_TERMS_API void
append_ftp_connection_warnings(const SettingsStore &store,
                               std::vector<std::string> *warnings);

} // namespace elder_terms

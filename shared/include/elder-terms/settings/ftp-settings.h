#pragma once

#include <optional>
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

/** Supported TLS protocol bounds. */
enum class FtpTlsVersion {
  /** TLS 1.0, requiring explicit configuration. */
  tls10 = 10,
  /** TLS 1.1, requiring explicit configuration. */
  tls11 = 11,
  /** TLS 1.2. */
  tls12 = 12,
  /** TLS 1.3. */
  tls13 = 13,
};

/** Preferred AUTH command; libcurl may try the other command. */
enum class FtpTlsAuthOrder {
  /** Prefer AUTH TLS. */
  tls,
  /** Prefer AUTH SSL; this does not select SSLv3. */
  ssl,
  /** Use libcurl's preference. */
  automatic,
};

/** Cryptographic policy independent of certificate exception handling. */
enum class FtpTlsCompatibility {
  /** Keep the TLS backend's normal security level. */
  standard,
  /** Explicitly use OpenSSL security level zero for this session. */
  openssl_legacy,
};

/** Action taken after a certificate validation failure. */
enum class FtpCertificateErrorAction {
  /** Refuse invalid certificates without confirmation. */
  reject,
  /** Ask the caller to approve a session-local exception. */
  prompt,
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
  /** Lowest permitted TLS version. */
  FtpTlsVersion tls_min_version = FtpTlsVersion::tls12;
  /** Highest permitted TLS version; empty uses the backend's upper bound. */
  std::optional<FtpTlsVersion> tls_max_version{};
  /** Explicit FTPS AUTH preference. */
  FtpTlsAuthOrder tls_auth_order = FtpTlsAuthOrder::tls;
  /** Session-local cryptographic compatibility policy. */
  FtpTlsCompatibility tls_compatibility = FtpTlsCompatibility::standard;
  /** OpenSSL cipher expression for TLS 1.2 and older; empty uses defaults. */
  std::string tls_cipher_list{};
  /** TLS 1.3 cipher names, separated by colons; empty uses defaults. */
  std::string tls13_cipher_list{};
  /** Whether a validation failure may be presented for explicit approval. */
  FtpCertificateErrorAction certificate_error_action = FtpCertificateErrorAction::reject;
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
/** @returns Setting key for [ftp] tls_min_version. */
ELDER_TERMS_API SettingKey ftp_tls_min_version_setting_key();
/** @returns Setting key for [ftp] tls_max_version. */
ELDER_TERMS_API SettingKey ftp_tls_max_version_setting_key();
/** @returns Setting key for [ftp] tls_auth_order. */
ELDER_TERMS_API SettingKey ftp_tls_auth_order_setting_key();
/** @returns Setting key for [ftp] tls_compatibility. */
ELDER_TERMS_API SettingKey ftp_tls_compatibility_setting_key();
/** @returns Setting key for [ftp] tls_cipher_list. */
ELDER_TERMS_API SettingKey ftp_tls_cipher_list_setting_key();
/** @returns Setting key for [ftp] tls13_cipher_list. */
ELDER_TERMS_API SettingKey ftp_tls13_cipher_list_setting_key();
/** @returns Setting key for [ftp] certificate_error_action. */
ELDER_TERMS_API SettingKey ftp_certificate_error_action_setting_key();
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

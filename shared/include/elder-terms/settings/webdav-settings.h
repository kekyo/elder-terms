#pragma once

#include <string>
#include <vector>

#include <elder-terms/export.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/** HTTP authentication methods permitted for a WebDAV connection. */
enum class WebdavAuthentication {
  /** Negotiate only Basic or Digest authentication. */
  automatic,
  /** Use HTTP Basic authentication. */
  basic,
  /** Use HTTP Digest authentication. */
  digest,
  /** Send no credentials. */
  none,
};

/** Immutable settings used by one WebDAV connection. */
struct WebdavConnectionSettings {
  /** Validated HTTP or HTTPS scheme. */
  std::string scheme = "https";
  /** Server hostname or numeric address, without a scheme or path. */
  std::string address;
  /** Effective TCP port, including the scheme-dependent default. */
  gint64 port = 443;
  /** Encoded absolute URL path identifying the browser's root collection. */
  std::string base_path = "/";
  /** Permitted HTTP authentication methods. */
  WebdavAuthentication authentication = WebdavAuthentication::automatic;
  /** Initial username for the runtime authentication prompt. */
  std::string username;
  /** Initial local directory, or empty for the shared runtime fallback. */
  std::string local_directory;
  /** Decoded absolute path relative to the root collection. */
  std::string remote_directory = "/";
  /** Absolute PEM CA bundle path, or empty for system defaults. */
  std::string ca_file;
  /** Whether certificate failures may be explicitly approved for this session. */
  bool prompt_certificate = false;
  /** DNS, TCP and TLS connection deadline in seconds. */
  gint64 connect_timeout_seconds = 30;
  /** Maximum time without network progress, excluding application pauses. */
  gint64 idle_timeout_seconds = 60;
  /** Invalid effective settings, with their key and configuration source. */
  std::vector<std::string> validation_errors;
};

/**
 * Returns a key in the WebDAV settings section.
 * @param name WebDAV setting name.
 * @returns Key identifying the named WebDAV setting.
 */
ELDER_TERMS_API SettingKey webdav_setting_key(std::string name);

/**
 * Defines the independently inherited WebDAV settings.
 * @returns Validated setting definitions for the webdav INI section.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
webdav_connection_setting_definitions();

/**
 * Resolves WebDAV settings and their validation failures.
 * @param store Source store with global and connection values.
 * @returns Effective settings; callers must reject validation errors before connecting.
 */
ELDER_TERMS_API WebdavConnectionSettings
webdav_connection_settings(const SettingsStore &store);

} // namespace elder_terms

// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <string>
#include <vector>

#include <elder-terms/export.h>
#include <elder-terms/http-authentication.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/** Immutable settings used by one HTTP connection. */
struct HttpConnectionSettings {
  /** Validated HTTP or HTTPS scheme. */
  std::string scheme = "https";
  /** Server hostname or numeric address, without a scheme or path. */
  std::string address;
  /** Effective TCP port, including the scheme-dependent default. */
  gint64 port = 443;
  /** Directory URL defining the allowed origin and path boundary. */
  std::string base_url;
  /** Optional initial page within base_url, resolved when settings are read. */
  std::string start_url;
  /** Permitted HTTP authentication methods. */
  HttpAuthentication authentication = HttpAuthentication::none;
  /** Initial username for the runtime authentication prompt. */
  std::string username;
  /** Initial local directory, or empty for the shared runtime fallback. */
  std::string local_directory;
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
 * Returns a key in the HTTP settings section.
 * @param name HTTP setting name.
 * @returns Key identifying the named HTTP setting.
 */
ELDER_TERMS_API SettingKey http_setting_key(std::string name);

/**
 * Defines the independently inherited HTTP settings.
 * @returns Validated setting definitions for the http INI section.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
http_connection_setting_definitions();

/**
 * Resolves HTTP settings and their validation failures.
 * @param store Source store with global and connection values.
 * @returns Effective settings; callers must reject validation errors before connecting.
 */
ELDER_TERMS_API HttpConnectionSettings
http_connection_settings(const SettingsStore &store);

} // namespace elder_terms

// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <string>
#include <vector>

#include <elder-terms/export.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/** Settings for one direct TCP SMB file transfer connection. */
struct SmbConnectionSettings {
  /** Server hostname or numeric address, without a scheme or path. */
  std::string address;
  /** SMB direct TCP port. */
  gint64 port = 445;
  /** Required single SMB share name. */
  std::string share;
  /** Authentication domain, or empty for the server default. */
  std::string domain;
  /** Initial username for the runtime authentication prompt. */
  std::string username;
  /** Initial local directory, or empty for the runtime fallback. */
  std::string local_directory;
  /** Initial absolute path within the share. */
  std::string remote_directory = "/";
  /** Require SMB message signing. */
  bool require_signing = true;
  /** Require SMB encryption; false still permits server-required encryption. */
  bool require_encryption = false;
  /** DNS and connection deadline in seconds. */
  gint64 connect_timeout_seconds = 30;
  /** Maximum time without network progress, excluding application pauses. */
  gint64 idle_timeout_seconds = 60;
  /** Invalid effective settings, with their key and configuration source. */
  std::vector<std::string> validation_errors;
};

/**
 * Returns a key in the SMB settings section.
 * @param name SMB setting name.
 * @returns Key identifying the named SMB setting.
 */
ELDER_TERMS_API SettingKey smb_setting_key(std::string name);

/**
 * Defines the independently inherited SMB settings.
 * @returns Validated setting definitions for the smb INI section.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
smb_connection_setting_definitions();

/**
 * Resolves SMB settings and their validation failures.
 * @param store Source store with global and connection values.
 * @returns Effective settings; callers must reject validation errors before connecting.
 */
ELDER_TERMS_API SmbConnectionSettings
smb_connection_settings(const SettingsStore &store);

} // namespace elder_terms

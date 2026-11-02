// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <elder-terms/export.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/** One NFS connection using TCP and numeric AUTH_SYS credentials. */
struct NfsConnectionSettings {
  /** Server hostname or numeric address, without a scheme or credentials. */
  std::string address;
  /** Explicit protocol version; never negotiated down to another version. */
  std::string version = "3";
  /** NFS service port, or zero for its standard discovery/default. */
  gint64 nfs_port = 0;
  /** MOUNT service port, or zero for PORTMAP discovery. */
  gint64 mount_port = 0;
  /** Initial v2/v3 export, or empty to show all available exports. */
  std::string export_path;
  /** Additional absolute v2/v3 export paths, independent of their basenames. */
  std::vector<std::string> known_exports;
  /** Request the MOUNT export list in addition to configured paths. */
  bool discover_exports = true;
  /** Initial NFS v4 namespace path, independent of v2/v3 exports. */
  std::string namespace_path;
  /** Additional absolute paths in the NFS v4 public namespace. */
  std::vector<std::string> known_namespace_paths;
  /** Explore the NFS v4 namespace root in addition to registered paths. */
  bool discover_namespace = true;
  /** Filename and symbolic-link target encoding; never affects file contents. */
  std::string filename_encoding = "UTF-8";
  /** Initial local directory, or empty for the application's default. */
  std::string local_directory;
  /** AUTH_SYS user ID, or no value for the current process user. */
  std::optional<std::uint32_t> uid;
  /** AUTH_SYS primary group ID, or no value for the current process group. */
  std::optional<std::uint32_t> gid;
  /** Additional AUTH_SYS groups, with at most 16 entries. */
  std::vector<std::uint32_t> supplementary_gids;
  /** Absolute deadline for connecting in seconds. */
  gint64 connect_timeout_seconds = 30;
  /** Absolute deadline for each filesystem request in seconds. */
  gint64 request_timeout_seconds = 60;
  /** Invalid effective settings, including their key and configuration source. */
  std::vector<std::string> validation_errors;
};

/** @param name NFS setting name. @returns Key in the nfs INI section. */
ELDER_TERMS_API SettingKey nfs_setting_key(std::string name);

/** @returns Definitions inherited independently from global to connection settings. */
ELDER_TERMS_API std::vector<SettingDefinition> nfs_connection_setting_definitions();

/**
 * Resolves effective NFS values without hiding invalid persisted input.
 * @param store Global defaults and connection overrides.
 * @returns Settings; validation_errors must be checked before connecting.
 */
ELDER_TERMS_API NfsConnectionSettings nfs_connection_settings(const SettingsStore &store);

} // namespace elder_terms

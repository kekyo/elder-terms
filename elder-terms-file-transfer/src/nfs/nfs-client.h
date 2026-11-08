// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include "../file-transfer/remote-file-client.h"

namespace elder_terms {

/**
 * Resolves an explicitly configured export into the current connection's identity.
 * @param client Open NFS server client.
 * @param export_path UTF-8 configured path; empty selects the virtual server root.
 * @returns Identity for browsing, independent of its display text.
 * @throws RemoteFileError When the export has not been registered or the client stopped.
 */
std::string nfs_export_identity(const std::shared_ptr<RemoteFileClient> &client, const std::string &export_path);

/** Immutable endpoint and filename policy for one NFS server browser. */
struct NfsClientOpenOptions {
  /** Server hostname or numeric address, without a URL scheme. */
  std::string address;
  /** Explicit protocol version; no version fallback is performed. */
  int version = 3;
  /** Zero for discovery/default, or an explicit TCP NFS service port. */
  int nfs_port = 0;
  /** Zero for discovery, or an explicit TCP MOUNT service port. */
  int mount_port = 0;
  /** Require a TCP source port below 1024; does not grant OS privileges. */
  bool require_reserved_port = false;
  /** Filename encoding, independent of locale and terminal settings. */
  std::string filename_encoding = "UTF-8";
  /** Known absolute exports in UTF-8; v4 uses its own namespace paths. */
  std::vector<std::string> known_exports;
  /** Whether to discover exports in addition to configured paths. */
  bool discover_exports = true;
  /** Known paths in the NFS v4 namespace; never interpreted as v2/v3 exports. */
  std::vector<std::string> known_namespace_paths;
  /** Explore the NFS v4 namespace root and expose accessible directory entries. */
  bool discover_namespace = true;
  /** AUTH_SYS user, or the current process identity when omitted. */
  std::optional<std::uint32_t> uid;
  /** AUTH_SYS primary group, or the current process identity when omitted. */
  std::optional<std::uint32_t> gid;
  /** AUTH_SYS supplementary groups; at most 16 values. */
  std::vector<std::uint32_t> supplementary_gids;
  /** Deadline for each connection or mount operation, in milliseconds. */
  int connect_timeout_ms = 30000;
  /** Absolute deadline for a filesystem request, in milliseconds. */
  int request_timeout_ms = 60000;
};

/**
 * Creates an NFS browser whose root lists distinct exported directories.
 * @param options Immutable endpoint, AUTH_SYS identity, and filename policy.
 * @param cancellation Cancels discovery and initial connection work.
 * @returns Server client; exports connect lazily when browsed or operated on.
 */
cardio::promise<std::shared_ptr<RemoteFileClient>> open_nfs_server_async(
    NfsClientOpenOptions options, cardio::cancellation cancellation);

/**
 * Stops active requests and retires all callbacks before returning.
 * @param client Client returned by open_nfs_server_async(); null is ignored.
 */
cardio::promise<void> stop_nfs_client_async(std::shared_ptr<RemoteFileClient> client);

} // namespace elder_terms

// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include "nfs-client.h"
#include "nfs-rpc.h"

namespace elder_terms {

/** Owned v2 file handles and one asynchronous RPC connection. */
struct Nfs2Filesystem;

/** One original byte name and its non-following metadata. */
struct Nfs2DirectoryEntry {
  /** Unconverted name copied from the READDIR response. */
  std::string bytes;
  /** File metadata; the server browser supplies display names and identities. */
  RemoteFileAttributes attributes;
};

/**
 * Mounts one export using MOUNT v1 and connects explicitly to NFS v2.
 * @param session Owned raw session registered with the server's stop handling.
 * @param options Immutable ports, deadlines and AUTH_SYS credentials.
 * @param address Resolved numeric address.
 * @param export_path Original export bytes, never a URL.
 * @param cancellation Cancels both MOUNT and NFS connection work.
 * @returns Filesystem owning a copied root handle.
 */
cardio::promise<std::shared_ptr<Nfs2Filesystem>> open_nfs2_filesystem_async(
    std::shared_ptr<NfsRpcSession> session, NfsClientOpenOptions options,
    std::string address, std::string export_path, cardio::cancellation cancellation);

/**
 * Lists every directory page and resolves non-following entry metadata.
 * @param filesystem Mounted export.
 * @param path Absolute path relative to the export, in original bytes.
 * @param cancellation Cancels the operation.
 * @returns Owned names and attributes, excluding dot entries.
 */
cardio::promise<std::vector<Nfs2DirectoryEntry>> list_nfs2_directory_async(
    std::shared_ptr<Nfs2Filesystem> filesystem, std::string path, cardio::cancellation cancellation);

/**
 * Resolves attributes without following links or escaping the export.
 * @param filesystem Mounted export.
 * @param path Absolute export-relative byte path.
 * @param cancellation Cancels the operation.
 * @returns Attributes, or no value for a missing path.
 */
cardio::promise<std::optional<RemoteFileAttributes>> stat_nfs2_async(
    std::shared_ptr<Nfs2Filesystem> filesystem, std::string path, cardio::cancellation cancellation);

/**
 * Opens a regular file by copying its v2 handle into the reader.
 * @param filesystem Mounted export.
 * @param path Absolute export-relative byte path.
 * @param cancellation Cancels lookup.
 * @returns Reader handling short responses and bounded 32-bit offsets.
 */
cardio::promise<std::unique_ptr<RemoteFileReader>> read_nfs2_file_async(
    std::shared_ptr<Nfs2Filesystem> filesystem, std::string path, cardio::cancellation cancellation);

/**
 * Reads a symbolic link's original target bytes without following it.
 * @param filesystem Mounted export.
 * @param path Absolute export-relative byte path.
 * @param cancellation Cancels the operation.
 * @returns Owned link target.
 */
cardio::promise<std::string> read_nfs2_link_async(
    std::shared_ptr<Nfs2Filesystem> filesystem, std::string path, cardio::cancellation cancellation);

/**
 * Prepares an upload whose private directory is claimed lazily with MKDIR.
 * @param filesystem Mounted export.
 * @param path Original byte path of the temporary upload container.
 * @param size Expected length, strictly below the ambiguous UINT32_MAX boundary.
 * @param permissions Final file mode, or the private default.
 * @param cancellation Cancels preparation.
 * @returns Writer reporting ownership before any mutation can reach the server.
 */
cardio::promise<std::unique_ptr<RemoteFileWriter>> write_nfs2_file_async(
    std::shared_ptr<Nfs2Filesystem> filesystem, std::string path, std::uint64_t size,
    std::optional<std::uint32_t> permissions, cardio::cancellation cancellation);

/**
 * Creates a directory without replacing an existing entry.
 * @param filesystem Mounted export.
 * @param path Original export-relative byte path.
 * @param permissions Requested directory mode, or 0755.
 * @param cancellation Cancels the request.
 */
cardio::promise<void> make_nfs2_directory_async(std::shared_ptr<Nfs2Filesystem> filesystem,
    std::string path, std::optional<std::uint32_t> permissions, cardio::cancellation cancellation);

/**
 * Removes a file, empty directory, or this connection's proven upload container.
 * @param filesystem Mounted export.
 * @param path Original export-relative byte path.
 * @param directory Whether an ordinary target is a directory.
 * @param cancellation Cancels the request.
 * @remarks Unknown upload ownership never authorizes cleanup.
 */
cardio::promise<void> remove_nfs2_async(std::shared_ptr<Nfs2Filesystem> filesystem,
    std::string path, bool directory, cardio::cancellation cancellation);

/**
 * Renames an item or commits an owned, completed upload.
 * @param filesystem Mounted export.
 * @param source Original source byte path or upload container path.
 * @param destination Original final byte path.
 * @param overwrite Whether an upload may atomically replace an existing file.
 * @param upload Whether the source is this connection's completed upload.
 * @param cancellation Cancels the operation without replaying mutations.
 */
cardio::promise<void> rename_nfs2_async(std::shared_ptr<Nfs2Filesystem> filesystem,
    std::string source, std::string destination, bool overwrite, bool upload, cardio::cancellation cancellation);

/**
 * Creates a link containing the supplied encoded bytes.
 * @param filesystem Mounted export.
 * @param target Already validated and encoded target path.
 * @param path Original byte path of the new link.
 * @param cancellation Cancels creation.
 */
cardio::promise<void> make_nfs2_link_async(std::shared_ptr<Nfs2Filesystem> filesystem,
    std::string target, std::string path, cardio::cancellation cancellation);

/**
 * Sets permissions and timestamps without following symbolic links.
 * @param filesystem Mounted export.
 * @param path Original byte path or owned upload container path.
 * @param attributes Requested metadata; absent fields remain unchanged.
 * @param cancellation Cancels the request.
 */
cardio::promise<void> set_nfs2_attributes_async(std::shared_ptr<Nfs2Filesystem> filesystem,
    std::string path, RemoteFileAttributes attributes, cardio::cancellation cancellation);

} // namespace elder_terms

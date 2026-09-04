#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cardio.h>

#include "remote-file-client.h"

namespace elder_terms {

/**
 * Direction of one remote file transfer.
 */
enum class FileTransferDirection {
  send,
  receive,
};

/** Filesystem endpoint containing an item operated on by the browser. */
enum class FileTransferEndpoint {
  local,
  remote,
};

/**
 * Decision applied to every conflict remaining in one transfer batch.
 */
enum class FileTransferConflictAction {
  overwrite,
  skip,
  cancel,
};

/**
 * Decision after one filesystem operation fails.
 */
enum class FileTransferFailureAction {
  retry,
  skip,
  abort,
};

/**
 * One source/destination collision reported by the transfer engine.
 */
struct FileTransferConflict {
  /** Transfer direction. */
  FileTransferDirection direction = FileTransferDirection::send;
  /** Selected or recursively discovered source path. */
  std::string source_path;
  /** Existing destination path. */
  std::string destination_path;
  /** Source item kind. */
  RemoteFileType source_type = RemoteFileType::other;
};

/**
 * One recoverable transfer failure.
 */
struct FileTransferFailure {
  /** Transfer direction. */
  FileTransferDirection direction = FileTransferDirection::send;
  /** Source path being processed. */
  std::string source_path;
  /** Destination path being processed. */
  std::string destination_path;
  /** Human-readable operation failure. */
  std::string message;
};

/**
 * Aggregate progress for a remote transfer batch.
 */
struct FileTransferProgress {
  /** Path of the item currently being transferred. */
  std::string current_path;
  /** Bytes copied so far. */
  std::uint64_t transferred_bytes = 0;
  /** Total bytes known after recursive discovery. */
  std::uint64_t total_bytes = 0;
  /** Completed item count. */
  std::uint64_t completed_items = 0;
  /** Total discovered item count. */
  std::uint64_t total_items = 0;
};

/**
 * UI callbacks used by a remote transfer.
 */
struct FileTransferCallbacks {
  /** Called once at the first destination conflict in a batch. */
  std::function<cardio::promise<FileTransferConflictAction>(
      const FileTransferConflict &, cardio::cancellation)>
      conflict;
  /** Called whenever a runtime failure needs Retry, Skip, or Abort. */
  std::function<cardio::promise<FileTransferFailureAction>(
      const FileTransferFailure &, cardio::cancellation)>
      failure;
  /** Called after discovery and after each copied chunk or completed item. */
  std::function<void(const FileTransferProgress &)> progress;
};

/**
 * One recursive remote send or receive request.
 */
struct FileTransferRequest {
  /** Transfer direction. */
  FileTransferDirection direction = FileTransferDirection::send;
  /** Local paths for send, or remote paths for receive. */
  std::vector<std::string> source_paths;
  /** Remote directory for send, or local directory for receive. */
  std::string destination_directory;
  /** Conflict, failure, and progress callbacks. */
  FileTransferCallbacks callbacks;
};

/**
 * Recursively sends or receives selected files, directories, and links.
 *
 * @param client Initialized remote file client.
 * @param request Transfer request.
 * @param cancellation Operation cancellation signal.
 *
 * @remarks
 * The first conflict decision is reused for the remainder of the batch.
 * Symbolic links are recreated without following them. Regular files are
 * committed from adjacent temporary paths and incomplete temporary files are
 * removed after failure or cancellation.
 */
cardio::promise<void>
run_file_transfer_async(std::shared_ptr<RemoteFileClient> client,
                        FileTransferRequest request,
                        cardio::cancellation cancellation);

/**
 * Renames one local or remote browser item without replacing another item.
 *
 * @param client Initialized remote file client. Required for a remote item.
 * @param endpoint Filesystem containing the item.
 * @param source_path Existing item path.
 * @param destination_path New sibling path.
 * @param cancellation Operation cancellation signal.
 *
 * @remarks
 * The destination is checked before the protocol rename operation. Services
 * that do not provide an atomic no-replace rename can still race with an
 * independent writer after that check.
 */
cardio::promise<void> rename_file_transfer_item_async(
    std::shared_ptr<RemoteFileClient> client,
    FileTransferEndpoint endpoint, std::string source_path,
    std::string destination_path, cardio::cancellation cancellation);

/**
 * Removes duplicate paths and paths covered by another selected ancestor.
 *
 * @param endpoint Filesystem containing the selected paths.
 * @param paths Selected item paths in display order.
 * @returns Original path strings reduced to independent deletion roots.
 */
std::vector<std::string> normalize_file_transfer_delete_paths(
    FileTransferEndpoint endpoint, std::vector<std::string> paths);

/**
 * Recursively deletes selected local or remote items.
 *
 * @param client Initialized remote file client. Required for remote items.
 * @param endpoint Filesystem containing the items.
 * @param paths Selected item paths. Duplicate descendants are ignored.
 * @param cancellation Operation cancellation signal.
 *
 * @remarks
 * Symbolic links are deleted without following their stored targets. Empty,
 * current-directory, parent-directory, and root paths are rejected before any
 * item is removed.
 */
cardio::promise<void> delete_file_transfer_items_async(
    std::shared_ptr<RemoteFileClient> client,
    FileTransferEndpoint endpoint, std::vector<std::string> paths,
    cardio::cancellation cancellation);

} // namespace elder_terms

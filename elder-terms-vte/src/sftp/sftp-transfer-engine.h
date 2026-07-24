#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cardio.h>

#include "sftp-client.h"

namespace elder_terms {

/**
 * Direction of one SFTP bulk transfer.
 */
enum class SftpTransferDirection {
  send,
  receive,
};

/**
 * Decision applied to every conflict remaining in one transfer batch.
 */
enum class SftpConflictAction {
  overwrite,
  skip,
  cancel,
};

/**
 * Decision after one filesystem operation fails.
 */
enum class SftpFailureAction {
  retry,
  skip,
  abort,
};

/**
 * One source/destination collision reported by the transfer engine.
 */
struct SftpTransferConflict {
  /** Transfer direction. */
  SftpTransferDirection direction = SftpTransferDirection::send;
  /** Selected or recursively discovered source path. */
  std::string source_path;
  /** Existing destination path. */
  std::string destination_path;
  /** Source item kind. */
  SftpFileType source_type = SftpFileType::other;
};

/**
 * One recoverable transfer failure.
 */
struct SftpTransferFailure {
  /** Transfer direction. */
  SftpTransferDirection direction = SftpTransferDirection::send;
  /** Source path being processed. */
  std::string source_path;
  /** Destination path being processed. */
  std::string destination_path;
  /** Human-readable operation failure. */
  std::string message;
};

/**
 * Aggregate progress for an SFTP transfer batch.
 */
struct SftpTransferProgress {
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
 * UI callbacks used by an SFTP transfer.
 */
struct SftpTransferCallbacks {
  /** Called once at the first destination conflict in a batch. */
  std::function<cardio::promise<SftpConflictAction>(
      const SftpTransferConflict &, cardio::cancellation)>
      conflict;
  /** Called whenever a runtime failure needs Retry, Skip, or Abort. */
  std::function<cardio::promise<SftpFailureAction>(
      const SftpTransferFailure &, cardio::cancellation)>
      failure;
  /** Called after discovery and after each copied chunk or completed item. */
  std::function<void(const SftpTransferProgress &)> progress;
};

/**
 * One recursive SFTP send or receive request.
 */
struct SftpTransferRequest {
  /** Transfer direction. */
  SftpTransferDirection direction = SftpTransferDirection::send;
  /** Local paths for send, or remote paths for receive. */
  std::vector<std::string> source_paths;
  /** Remote directory for send, or local directory for receive. */
  std::string destination_directory;
  /** Conflict, failure, and progress callbacks. */
  SftpTransferCallbacks callbacks;
};

/**
 * Recursively sends or receives selected files, directories, and links.
 *
 * @param client Initialized SFTP client.
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
run_sftp_transfer_async(std::shared_ptr<SftpClient> client,
                        SftpTransferRequest request,
                        cardio::cancellation cancellation);

} // namespace elder_terms

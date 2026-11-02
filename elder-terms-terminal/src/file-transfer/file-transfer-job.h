// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include "file-transfer-engine.h"
#include <chrono>

namespace elder_terms {

/** Lifecycle of one user-requested transfer, independent of any widget. */
enum class FileTransferJobState {
  queued, discovering, transferring, awaiting_decision, cancelling,
  completed, failed, cancelled,
};

/** Presentation-safe data for one job; contains no client or authentication data. */
struct FileTransferJobSnapshot {
  /** Process-unique identifier, never reused on reconnection or retry. */
  std::uint64_t id;
  /** Opaque owner identity, not a URL or authentication configuration. */
  std::string connection_id;
  /** Connection generation captured when the job was submitted. */
  std::uint64_t connection_generation;
  /** Display protocol, e.g. FTP, FTPS, SFTP, WebDAV, SMB, HTTP, or NFS. */
  std::string protocol;
  /** Copy direction. */
  FileTransferDirection direction;
  /** Selected roots; a multi-selection remains one job. */
  std::vector<std::string> source_paths;
  /** Destination captured at submission, unaffected by later navigation. */
  std::string destination_directory;
  /** Current lifecycle state. */
  FileTransferJobState state = FileTransferJobState::queued;
  /** Start time, absent for a job cancelled before execution. */
  std::optional<std::chrono::system_clock::time_point> started_at;
  /** Time at which processing and cleanup both finished. */
  std::optional<std::chrono::system_clock::time_point> finished_at;
  /** Last progress; discovery and unknown byte totals are explicit. */
  FileTransferProgress progress;
  /** Final result, absent while waiting or executing. */
  std::optional<FileTransferResult> result;
};

/** Opaque dispatcher-owned job and its cancellation state. */
struct FileTransferJob;

/**
 * Creates a waiting job. Use a new job to retry a finished request.
 * @param connection_id Opaque connection identity without credentials.
 * @param generation Current connection generation.
 * @param protocol User-visible protocol name.
 * @param request Direction and paths; callbacks are supplied at execution.
 * @param changed Observer invoked on the owning dispatcher; may be empty.
 * @returns A new job; no I/O is started.
 */
std::shared_ptr<FileTransferJob> create_file_transfer_job(std::string connection_id,
    std::uint64_t generation, std::string protocol, const FileTransferRequest &request,
    std::function<void(const FileTransferJobSnapshot &)> changed);

/** @param job Job on its owning dispatcher. @returns Its current immutable view. */
const FileTransferJobSnapshot &file_transfer_job_snapshot(const std::shared_ptr<FileTransferJob> &job);

/** @param state Lifecycle state. @returns Whether processing and cleanup have finished. */
bool file_transfer_job_finished(FileTransferJobState state);

/**
 * Requests cancellation; active jobs remain cancelling until cleanup completes.
 * @param job Job to cancel; completed jobs are unchanged.
 */
void cancel_file_transfer_job(const std::shared_ptr<FileTransferJob> &job);

/**
 * Runs a waiting job exactly once and captures failures in its final result.
 * @param job Waiting job retained through cleanup.
 * @param client Client for the captured connection generation.
 * @param callbacks Decision and progress handlers; may be empty.
 * @param cancellation Owner shutdown signal.
 * @remarks Runs on the owner dispatcher. Repeated calls do not repeat I/O.
 */
cardio::promise<void> run_file_transfer_job_async(std::shared_ptr<FileTransferJob> job,
    std::shared_ptr<RemoteFileClient> client, FileTransferCallbacks callbacks,
    cardio::cancellation cancellation);

/** Dispatcher-owned per-window history and single-connection execution queue. */
struct FileTransferQueue;
/** @param connection_id Credential-free owner identity. @param changed Job observer. @returns Empty queue. */
std::shared_ptr<FileTransferQueue> create_file_transfer_queue(std::string connection_id,
    std::function<void(const FileTransferJobSnapshot &)> changed);
/** @param queue Idle, disconnected queue. @param client New service. @param protocol Display name. */
void attach_file_transfer_queue(const std::shared_ptr<FileTransferQueue> &queue,
    std::shared_ptr<RemoteFileClient> client, std::string protocol);
/** @param queue Available service queue. @param request Captured copy request. @returns New waiting job. */
std::shared_ptr<FileTransferJob> enqueue_file_transfer_job(const std::shared_ptr<FileTransferQueue> &queue,
    const FileTransferRequest &request);
/** @param queue History owner. @returns Jobs in submission order, retained until explicitly cleared. */
const std::vector<std::shared_ptr<FileTransferJob>> &file_transfer_jobs(const std::shared_ptr<FileTransferQueue> &queue);
/** @param queue History owner. Removes terminal jobs only; pending work is unaffected. */
void clear_finished_file_transfer_jobs(const std::shared_ptr<FileTransferQueue> &queue);
/**
 * Retires a generation and requests cancellation of its operations.
 * @param queue Owner queue. @param closing True for window close; false marks waiting jobs as requiring retry.
 */
void disconnect_file_transfer_queue(const std::shared_ptr<FileTransferQueue> &queue, bool closing);
/**
 * Drains waiting requests in order; waits for cleanup before starting the next.
 * @param queue Queue retained until the drain ends. @param callbacks Decision handlers.
 * @param cancellation Owner shutdown. @remarks Caller must await this before replacing or releasing a connection.
 */
cardio::promise<void> run_file_transfer_queue_async(std::shared_ptr<FileTransferQueue> queue,
    FileTransferCallbacks callbacks, cardio::cancellation cancellation);
/**
 * Loads a browser directory in the connection's operation order.
 * @param queue Service queue. @param path Directory to load. @param cancellation Owner cancellation.
 * @returns Directory snapshot. @remarks Reads wait behind active transfers, including decision and cleanup phases.
 */
cardio::promise<RemoteDirectorySnapshot> load_file_transfer_queue_directory_async(
    std::shared_ptr<FileTransferQueue> queue, std::string path, cardio::cancellation cancellation);

} // namespace elder_terms

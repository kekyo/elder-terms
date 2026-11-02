// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "file-transfer-job.h"
#include <atomic>

namespace elder_terms {

struct FileTransferJob {
  FileTransferJobSnapshot snapshot;
  std::function<void(const FileTransferJobSnapshot &)> changed;
  cardio::cancellation_source cancellation;
};

static void publish_job(const std::shared_ptr<FileTransferJob> &job) {
  if (job->changed) job->changed(job->snapshot);
}

bool file_transfer_job_finished(FileTransferJobState state) {
  return state == FileTransferJobState::completed || state == FileTransferJobState::failed ||
      state == FileTransferJobState::cancelled;
}

std::shared_ptr<FileTransferJob> create_file_transfer_job(std::string connection_id,
    std::uint64_t generation, std::string protocol, const FileTransferRequest &request,
    std::function<void(const FileTransferJobSnapshot &)> changed) {
  static std::atomic<std::uint64_t> sequence{0};
  auto job = std::make_shared<FileTransferJob>();
  job->snapshot.id = ++sequence;
  job->snapshot.connection_id = std::move(connection_id);
  job->snapshot.connection_generation = generation;
  job->snapshot.protocol = std::move(protocol);
  job->snapshot.direction = request.direction;
  job->snapshot.source_paths = request.source_paths;
  job->snapshot.destination_directory = request.destination_directory;
  job->snapshot.progress.total_bytes = std::nullopt;
  job->changed = std::move(changed);
  return job;
}

const FileTransferJobSnapshot &file_transfer_job_snapshot(const std::shared_ptr<FileTransferJob> &job) {
  return job->snapshot;
}

static void finish_job(const std::shared_ptr<FileTransferJob> &job, FileTransferResult result) {
  if (file_transfer_job_finished(job->snapshot.state)) return;
  job->snapshot.state = result.outcome == FileTransferOutcome::completed ? FileTransferJobState::completed :
      result.outcome == FileTransferOutcome::cancelled ? FileTransferJobState::cancelled : FileTransferJobState::failed;
  job->snapshot.result = std::move(result);
  job->snapshot.finished_at = std::chrono::system_clock::now();
  publish_job(job);
}

void cancel_file_transfer_job(const std::shared_ptr<FileTransferJob> &job) {
  if (!job || file_transfer_job_finished(job->snapshot.state) || job->snapshot.state == FileTransferJobState::cancelling) return;
  if (job->snapshot.state == FileTransferJobState::queued) {
    FileTransferResult result;
    result.outcome = FileTransferOutcome::cancelled;
    finish_job(job, std::move(result));
  } else {
    job->snapshot.state = FileTransferJobState::cancelling;
    publish_job(job);
  }
  (void)job->cancellation.cancel();
}

cardio::promise<void> run_file_transfer_job_async(std::shared_ptr<FileTransferJob> job,
    std::shared_ptr<RemoteFileClient> client, FileTransferCallbacks callbacks,
    cardio::cancellation cancellation) {
  if (job->snapshot.state != FileTransferJobState::queued) co_return;
  if (cancellation.is_cancellation_requested()) { cancel_file_transfer_job(job); co_return; }
  job->snapshot.started_at = std::chrono::system_clock::now();
  job->snapshot.state = FileTransferJobState::discovering;
  auto stopping = cancellation.on_cancellation_requested([weak = std::weak_ptr(job)] {
    if (auto owner = weak.lock()) cancel_file_transfer_job(owner);
  });
  publish_job(job);
  FileTransferRequest request;
  request.direction = job->snapshot.direction;
  request.source_paths = job->snapshot.source_paths;
  request.destination_directory = job->snapshot.destination_directory;
  request.callbacks.conflict = [job, handler = callbacks.conflict](const FileTransferConflict &conflict,
      cardio::cancellation stop) -> cardio::promise<FileTransferConflictAction> {
    stop.throw_if_cancellation_requested();
    const auto previous = job->snapshot.state;
    job->snapshot.state = FileTransferJobState::awaiting_decision;
    publish_job(job);
    const auto decision = handler ? co_await handler(conflict, stop) : FileTransferConflictAction::cancel;
    stop.throw_if_cancellation_requested();
    job->snapshot.state = previous;
    publish_job(job);
    co_return decision;
  };
  request.callbacks.failure = [job, handler = callbacks.failure](const FileTransferFailure &failure,
      cardio::cancellation stop) -> cardio::promise<FileTransferFailureAction> {
    stop.throw_if_cancellation_requested();
    const auto previous = job->snapshot.state;
    job->snapshot.state = FileTransferJobState::awaiting_decision;
    publish_job(job);
    const auto decision = handler ? co_await handler(failure, stop) : FileTransferFailureAction::abort;
    stop.throw_if_cancellation_requested();
    job->snapshot.state = previous;
    publish_job(job);
    co_return decision;
  };
  request.callbacks.progress = [job, handler = callbacks.progress](const FileTransferProgress &progress) {
    job->snapshot.progress = progress;
    if (job->snapshot.state != FileTransferJobState::cancelling)
      job->snapshot.state = progress.discovery_complete ? FileTransferJobState::transferring : FileTransferJobState::discovering;
    publish_job(job);
    if (handler) handler(progress);
  };
  std::optional<FileTransferResult> result;
  request.callbacks.finished = [&result](const FileTransferResult &value) { result = value; };
  try {
    co_await run_file_transfer_async(std::move(client), std::move(request), job->cancellation.get_cancellation());
  } catch (...) {
    // The engine reports the result only after its handles and temporary files
    // have been retired. Preserve that result instead of parsing error text.
    if (!result) throw;
  }
  finish_job(job, std::move(*result));
  if (callbacks.finished) callbacks.finished(*job->snapshot.result);
}

} // namespace elder_terms

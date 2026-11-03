// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "file-transfer-job.h"
#include <atomic>
#include <algorithm>

namespace elder_terms {

struct FileTransferJob {
  FileTransferJobSnapshot snapshot;
  std::function<void(const FileTransferJobSnapshot &)> changed;
  cardio::cancellation_source cancellation;
};

std::string file_transfer_job_path_label(const FileTransferJobSnapshot &job, const std::string &path) {
  const auto found = job.path_labels.find(path);
  return found == job.path_labels.end() ? path : found->second;
}

static void remember_remote_path(const std::shared_ptr<FileTransferJob> &job,
    const std::shared_ptr<RemoteFileClient> &client, const std::string &path) {
  if (path.empty() || job->snapshot.path_labels.contains(path)) return;
  try { job->snapshot.path_labels.emplace(path, client->display_path(path)); }
  catch (const std::exception &) { job->snapshot.path_labels.emplace(path, "[unavailable remote path]"); }
}

static void remember_request_paths(const std::shared_ptr<FileTransferJob> &job,
    const std::shared_ptr<RemoteFileClient> &client) {
  if (job->snapshot.direction == FileTransferDirection::receive) {
    for (const auto &source : job->snapshot.source_paths) remember_remote_path(job, client, source);
  } else remember_remote_path(job, client, job->snapshot.destination_directory);
}

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
  job->snapshot.receive_as_file = request.receive_as_file;
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
  remember_request_paths(job, client);
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
  request.receive_as_file = job->snapshot.receive_as_file;
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
  request.callbacks.failure = [job, client, handler = callbacks.failure](const FileTransferFailure &failure,
      cardio::cancellation stop) -> cardio::promise<FileTransferFailureAction> {
    stop.throw_if_cancellation_requested();
    const auto previous = job->snapshot.state;
    remember_remote_path(job, client, failure.direction == FileTransferDirection::receive
        ? failure.source_path : failure.destination_path);
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
      job->snapshot.state = progress.discovery_complete || progress.transferred_bytes
          ? FileTransferJobState::transferring : FileTransferJobState::discovering;
    publish_job(job);
    if (handler) handler(progress);
  };
  std::optional<FileTransferResult> result;
  request.callbacks.finished = [&result](const FileTransferResult &value) { result = value; };
  try {
    co_await run_file_transfer_async(client, std::move(request), job->cancellation.get_cancellation());
  } catch (...) {
    // The engine reports the result only after its handles and temporary files
    // have been retired. Preserve that result instead of parsing error text.
    if (!result) throw;
  }
  for (const auto &failure : result->failures)
    remember_remote_path(job, client, failure.direction == FileTransferDirection::receive
        ? failure.source_path : failure.destination_path);
  if (job->snapshot.direction == FileTransferDirection::send)
    for (const auto &path : result->temporary_paths) remember_remote_path(job, client, path);
  finish_job(job, std::move(*result));
  if (callbacks.finished) callbacks.finished(*job->snapshot.result);
}

struct FileTransferQueue {
  std::string connection_id;
  std::string protocol;
  std::uint64_t generation = 0;
  std::shared_ptr<RemoteFileClient> client;
  std::function<void(const FileTransferJobSnapshot &)> changed;
  std::vector<std::shared_ptr<FileTransferJob>> jobs;
  cardio::primitives::mutex operations;
  cardio::cancellation_source stop;
  bool available = false;
  bool running = false;
};

std::shared_ptr<FileTransferQueue> create_file_transfer_queue(std::string connection_id,
    std::function<void(const FileTransferJobSnapshot &)> changed) {
  auto queue = std::make_shared<FileTransferQueue>();
  queue->connection_id = std::move(connection_id);
  queue->changed = std::move(changed);
  return queue;
}

void attach_file_transfer_queue(const std::shared_ptr<FileTransferQueue> &queue,
    std::shared_ptr<RemoteFileClient> client, std::string protocol) {
  if (queue->running || queue->operations.is_locked() || queue->available)
    throw std::logic_error("Retire the old connection before attaching a new one");
  if (!client) throw std::invalid_argument("Remote file client is required");
  queue->client = std::move(client);
  queue->protocol = std::move(protocol);
  ++queue->generation;
  queue->stop = cardio::cancellation_source{};
  queue->available = true;
}

std::shared_ptr<FileTransferJob> enqueue_file_transfer_job(const std::shared_ptr<FileTransferQueue> &queue,
    const FileTransferRequest &request) {
  if (!queue->available) throw std::logic_error("Reconnect before submitting a transfer");
  auto job = create_file_transfer_job(queue->connection_id, queue->generation, queue->protocol, request, queue->changed);
  remember_request_paths(job, queue->client);
  queue->jobs.push_back(job);
  publish_job(job);
  return job;
}

const std::vector<std::shared_ptr<FileTransferJob>> &file_transfer_jobs(const std::shared_ptr<FileTransferQueue> &queue) {
  return queue->jobs;
}

void clear_finished_file_transfer_jobs(const std::shared_ptr<FileTransferQueue> &queue) {
  std::erase_if(queue->jobs, [](const auto &job) { return file_transfer_job_finished(job->snapshot.state); });
}

void disconnect_file_transfer_queue(const std::shared_ptr<FileTransferQueue> &queue, bool closing) {
  if (!queue) return;
  queue->available = false;
  // Work on a copy: observers can update the visible history during notifications.
  const auto jobs = queue->jobs;
  for (const auto &job : jobs) {
    if (!closing && job->snapshot.state == FileTransferJobState::queued) {
      FileTransferResult result;
      result.outcome = FileTransferOutcome::failed;
      result.failures.push_back({job->snapshot.direction, {}, job->snapshot.destination_directory,
          "Disconnected before execution; retry after reconnecting"});
      finish_job(job, std::move(result));
    } else cancel_file_transfer_job(job);
  }
  (void)queue->stop.cancel();
}

struct QueueCancellation {
  cardio::cancellation_source source;
  ~QueueCancellation() { (void)source.cancel(); }
};

cardio::promise<void> run_file_transfer_queue_async(std::shared_ptr<FileTransferQueue> queue,
    FileTransferCallbacks callbacks, cardio::cancellation cancellation) {
  if (queue->running) co_return;
  queue->running = true;
  QueueCancellation stop{cardio::cancellations::any(cancellation, queue->stop.get_cancellation())};
  try {
    while (queue->available) {
      const auto next = std::find_if(queue->jobs.begin(), queue->jobs.end(), [](const auto &job) {
        return job->snapshot.state == FileTransferJobState::queued;
      });
      if (next == queue->jobs.end()) break;
      const auto job = *next;
      auto locking = queue->operations.lock(stop.source.get_cancellation());
      auto lock = std::move(co_await locking);
      if (!queue->available || job->snapshot.connection_generation != queue->generation) break;
      co_await run_file_transfer_job_async(job, queue->client, callbacks, stop.source.get_cancellation());
    }
  } catch (const cardio::canceled_exception &) {
    if (queue->available) disconnect_file_transfer_queue(queue, true);
  } catch (...) {
    queue->running = false;
    throw;
  }
  queue->running = false;
}

cardio::promise<RemoteDirectorySnapshot> load_file_transfer_queue_directory_async(
    std::shared_ptr<FileTransferQueue> queue, std::string path, cardio::cancellation cancellation,
    bool refresh, bool force_html, bool from_input) {
  if (!queue->available) throw cardio::canceled_exception();
  const auto generation = queue->generation;
  QueueCancellation stop{cardio::cancellations::any(cancellation, queue->stop.get_cancellation())};
  auto locking = queue->operations.lock(stop.source.get_cancellation());
  auto lock = std::move(co_await locking);
  if (!queue->available || generation != queue->generation) throw cardio::canceled_exception();
  if (from_input)
    path = co_await queue->client->resolve_input_path_async(std::move(path), stop.source.get_cancellation());
  co_return co_await queue->client->browse_directory_async(std::move(path), refresh, force_html, stop.source.get_cancellation());
}

} // namespace elder_terms

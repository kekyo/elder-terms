// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "smb-client.h"
#include "smb-share-list.h"

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/smb2-errors.h>
#include <smb2/libsmb2-share-enum.h>
#include <poll.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <map>

namespace elder_terms {

// Linked cancellation sources retain their input registrations. Retire those
// registrations when a request finishes, including successful short requests.
struct ScopedSmbCancellation {
  cardio::cancellation_source source;
  ~ScopedSmbCancellation() { (void)source.cancel(); }
  cardio::cancellation get_cancellation() const { return source.get_cancellation(); }
  bool cancel() { return source.cancel(); }
};

struct SmbSession {
  smb2_context *context = nullptr;
  cardio::primitives::mutex operations;
  cardio::cancellation_source stopping;
  int idle_timeout_ms = 60000;
  bool command_active = false;
  bool transfer_active = false;
  std::function<void()> disconnected;

  ~SmbSession() { if (context) smb2_destroy_context(context); }

  void abort() {
    if (context) smb2_destroy_context(context);
    context = nullptr;
    auto notify = std::move(disconnected);
    if (notify) notify();
  }

  // An abandoned stream ends the session. An active command owns its callback
  // state until its FD cancellation finishes, so only that command may abort it.
  void request_stop() {
    (void)stopping.cancel();
    if (!command_active) abort();
  }
};

struct SmbCompletion {
  bool done = false;
  int status = 0;
  void *data = nullptr;
  std::string message;
  std::function<void(void *)> consume;
  std::exception_ptr failure;
};

static void completed(smb2_context *context, int status, void *data, void *opaque) {
  auto &result = *static_cast<SmbCompletion *>(opaque);
  result.status = status;
  result.data = data;
  if (status < -4095) result.message = nterror_to_str(static_cast<uint32_t>(status));
  else if (status < 0) result.message = smb2_get_error(context);
  try { if (status == 0 && result.consume) result.consume(data); }
  catch (...) { result.failure = std::current_exception(); }
  result.done = true;
}

static void require_connected(const SmbSession &session) {
  if (!session.context || session.stopping.get_cancellation().is_cancellation_requested())
    throw RemoteFileError("SMB connection has ended. Reconnect to continue.", true);
}

// The caller holds operations for the complete logical operation. The context
// is destroyed before a failed command's coroutine-local callback state leaves.
static cardio::promise<SmbCompletion> execute_async(
    const std::shared_ptr<SmbSession> &session,
    std::function<int(smb2_command_cb, void *)> start,
    std::function<void(void *)> consume, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  require_connected(*session);
  ScopedSmbCancellation stopped{cardio::cancellations::any(cancellation, session->stopping.get_cancellation())};
  SmbCompletion result;
  result.consume = std::move(consume);
  session->command_active = true;
  try {
    const int status = start(completed, &result);
    if (status < 0) {
      result.done = true;
      result.status = status;
      result.message = smb2_get_error(session->context);
    }
    while (!result.done) {
      const int fd = smb2_get_fd(session->context);
      const int wanted = smb2_which_events(session->context);
      if (fd < 0) throw RemoteFileError("SMB socket is unavailable", true);
      auto interest = cardio::fd_event::none;
      if (wanted & POLLIN) interest |= cardio::fd_event::read;
      if (wanted & POLLOUT) interest |= cardio::fd_event::write;
      ScopedSmbCancellation deadline{cardio::cancellations::timeout(session->idle_timeout_ms)};
      ScopedSmbCancellation waiting{cardio::cancellations::any(stopped.get_cancellation(), deadline.get_cancellation())};
      const auto ready = co_await cardio::from_fd(fd, interest, waiting.get_cancellation());
      (void)deadline.cancel();
      int events = 0;
      if ((ready & cardio::fd_event::read) != cardio::fd_event::none) events |= POLLIN;
      if ((ready & cardio::fd_event::write) != cardio::fd_event::none) events |= POLLOUT;
      if ((ready & cardio::fd_event::error) != cardio::fd_event::none) events |= POLLERR;
      if ((ready & cardio::fd_event::hangup) != cardio::fd_event::none) events |= POLLHUP;
      if (smb2_service(session->context, events) < 0 && !(result.done && result.status != 0))
        throw RemoteFileError(smb2_get_error(session->context), true);
    }
  } catch (const cardio::canceled_exception &) {
    session->abort();
    session->command_active = false;
    if (stopped.get_cancellation().is_cancellation_requested()) throw;
    throw RemoteFileError("SMB operation timed out. Reconnect to continue.", true);
  } catch (...) {
    session->abort();
    session->command_active = false;
    throw;
  }
  session->command_active = false;
  if (result.failure) std::rethrow_exception(result.failure);
  co_return result;
}

static int error_number(const SmbCompletion &result) {
  return result.status < -4095 ? nterror_to_errno(static_cast<uint32_t>(result.status)) : -result.status;
}

static void check(const SmbCompletion &result, const char *operation) {
  if (result.status >= 0) return;
  const int error = error_number(result);
  const bool lost = error == ECONNRESET || error == ENOTCONN || error == EPIPE || error == ETIMEDOUT;
  throw RemoteFileError(std::string("SMB ") + operation + ": " + result.message, lost);
}

static std::string canonical_path(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  std::vector<std::string> parts;
  for (std::size_t start = 0; start <= path.size();) {
    const auto end = path.find('/', start);
    auto part = path.substr(start, end == std::string::npos ? end : end - start);
    if (part == "..") {
      if (parts.empty()) throw RemoteFileError("SMB path leaves the configured share", false);
      parts.pop_back();
    } else if (!part.empty() && part != ".") {
      if (part.find(':') != std::string::npos || part.find('\0') != std::string::npos ||
          part.find_first_of("\r\n") != std::string::npos || part.back() == '.' || part.back() == ' ')
        throw RemoteFileError("Unsupported SMB path component", false);
      parts.push_back(std::move(part));
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  std::string result;
  for (const auto &part : parts) result += "/" + part;
  return result.empty() ? "/" : result;
}

static std::string share_path(const std::string &path) { return path == "/" ? "" : path.substr(1); }

static cardio::promise<void> close_raw_async(const std::shared_ptr<SmbSession> &session,
    const smb2_create_reply &handle, cardio::cancellation cancellation) {
  smb2_close_request request{};
  std::memcpy(request.file_id, handle.file_id, SMB2_FD_SIZE);
  auto result = co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_close_async(session->context, &request, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session->context, pdu);
    return 0;
  }, {}, cancellation);
  if (result.status != 0) session->request_stop();
  check(result, "close");
}

static cardio::promise<std::optional<smb2_create_reply>> open_raw_async(
    const std::shared_ptr<SmbSession> &session, const std::string &path,
    uint32_t access, uint32_t disposition, uint32_t create_options, cardio::cancellation cancellation) {
  const auto name = share_path(path);
  smb2_create_request request{};
  request.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
  request.desired_access = access;
  request.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE | SMB2_FILE_SHARE_DELETE;
  request.create_disposition = disposition;
  request.create_options = SMB2_FILE_OPEN_REPARSE_POINT | create_options;
  request.name = name.c_str();
  smb2_create_reply handle{};
  auto result = co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_create_async(session->context, &request, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session->context, pdu);
    return 0;
  }, [&](void *data) {
    const auto &reply = *static_cast<smb2_create_reply *>(data);
    // Decoded create contexts belong to the callback. Retain only value fields.
    std::memcpy(handle.file_id, reply.file_id, SMB2_FD_SIZE);
    handle.file_attributes = reply.file_attributes;
    handle.end_of_file = reply.end_of_file;
    handle.last_access_time = reply.last_access_time;
    handle.last_write_time = reply.last_write_time;
  }, cancellation);
  if (result.status != 0 && error_number(result) == ENOENT) co_return std::nullopt;
  check(result, "open");
  co_return handle;
}

// Check each ancestor as a reparse point itself. Never deliberately traverse a
// server-side link, even if the server permits junctions outside its share.
static cardio::promise<bool> inspect_ancestors_async(const std::shared_ptr<SmbSession> &session,
    const std::string &path, cardio::cancellation cancellation) {
  for (auto separator = path.find('/', 1); separator != std::string::npos;
       separator = path.find('/', separator + 1)) {
    const auto parent = path.substr(0, separator);
    auto handle = co_await open_raw_async(session, parent, SMB2_FILE_READ_ATTRIBUTES, SMB2_FILE_OPEN, 0, cancellation);
    if (!handle) co_return false;
    const auto attributes = handle->file_attributes;
    co_await close_raw_async(session, *handle, cancellation);
    if ((attributes & SMB2_FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & SMB2_FILE_ATTRIBUTE_DIRECTORY))
      throw RemoteFileError("SMB path contains a link or a non-directory component", false);
  }
  co_return true;
}

static RemoteFileType file_type(uint32_t attributes) {
  if (attributes & SMB2_FILE_ATTRIBUTE_REPARSE_POINT) return RemoteFileType::other;
  return attributes & SMB2_FILE_ATTRIBUTE_DIRECTORY ? RemoteFileType::directory : RemoteFileType::regular;
}

static RemoteFileAttributes attributes_for(const std::string &path, const smb2_create_reply &handle) {
  smb2_timeval access{}, modified{};
  smb2_win_to_timeval(handle.last_access_time, &access);
  smb2_win_to_timeval(handle.last_write_time, &modified);
  return {.name = path.substr(path.find_last_of('/') + 1), .path = path,
      .type = file_type(handle.file_attributes), .size = handle.end_of_file,
      .permissions = {}, .access_time_unix_seconds = access.tv_sec,
      .modification_time_unix_seconds = modified.tv_sec};
}

class SmbReader final : public RemoteFileReader {
  std::shared_ptr<SmbSession> session_;
  smb2_create_reply handle_;
  bool closed_ = false;
  uint64_t offset_ = 0;
public:
  SmbReader(std::shared_ptr<SmbSession> session, const smb2_create_reply &handle)
      : session_(std::move(session)), handle_(handle) {}
  ~SmbReader() override { if (!closed_) session_->request_stop(); }
  cardio::promise<std::size_t> read_async(std::span<std::byte> buffer,
      cardio::cancellation cancellation) override {
    auto lock = std::move(co_await session_->operations.lock(cancellation));
    require_connected(*session_);
    if (closed_) throw RemoteFileError("SMB reader is closed", false);
    if (buffer.empty()) co_return 0;
    const auto amount = std::min<std::size_t>(buffer.size(), smb2_get_max_read_size(session_->context));
    if (amount == 0) throw RemoteFileError("SMB server supplied an invalid read limit", true);
    smb2_read_request request{};
    request.length = amount;
    request.offset = offset_;
    request.buf = reinterpret_cast<uint8_t *>(buffer.data());
    std::memcpy(request.file_id, handle_.file_id, SMB2_FD_SIZE);
    std::size_t count = 0;
    auto result = co_await execute_async(session_, [&](auto cb, auto data) {
      auto *pdu = smb2_cmd_read_async(session_->context, &request, cb, data);
      if (!pdu) return -ENOMEM;
      smb2_queue_pdu(session_->context, pdu);
      return 0;
    }, [&](void *data) {
      // The public read request supplies its buffer. The upstream high-level
      // reader likewise obtains the actual byte count from this raw reply.
      if (!data) throw RemoteFileError("SMB read response is missing", true);
      count = static_cast<smb2_read_reply *>(data)->data_length;
      if (count > amount) throw RemoteFileError("SMB read exceeded its buffer", true);
    }, cancellation);
    if (static_cast<uint32_t>(result.status) == SMB2_STATUS_END_OF_FILE) co_return 0;
    check(result, "read");
    offset_ += count;
    co_return count;
  }
  cardio::promise<void> close_async(cardio::cancellation cancellation) override {
    auto lock = std::move(co_await session_->operations.lock(cancellation));
    if (closed_) co_return;
    require_connected(*session_);
    co_await close_raw_async(session_, handle_, cancellation);
    closed_ = true;
  }
};

class SmbWriter final : public RemoteFileWriter {
  std::shared_ptr<SmbSession> session_;
  std::string path_;
  uint64_t expected_;
  uint64_t offset_ = 0;
  std::optional<smb2_create_reply> handle_;
  RemoteFileCreationState creation_ = RemoteFileCreationState::not_created;
  std::exception_ptr failure_;
  cardio::cancellation parent_cancellation_;
  bool closed_ = false;

  cardio::promise<void> create_async(cardio::cancellation cancellation) {
    if (failure_) std::rethrow_exception(failure_);
    if (handle_) co_return;
    ScopedSmbCancellation combined{cardio::cancellations::any(cancellation, parent_cancellation_)};
    combined.get_cancellation().throw_if_cancellation_requested();
    if (!co_await inspect_ancestors_async(session_, path_, combined.get_cancellation()))
      throw RemoteFileError("SMB upload parent does not exist", false);
    // Defer CREATE until this writer is returned to the engine. If the server
    // creates the file but its reply is lost, the engine can then report the
    // uncertain path without deleting a file whose ownership was not proven.
    creation_ = RemoteFileCreationState::unknown;
    try {
      handle_ = co_await open_raw_async(session_, path_, SMB2_FILE_WRITE_DATA | SMB2_FILE_READ_ATTRIBUTES,
          SMB2_FILE_CREATE, SMB2_FILE_NON_DIRECTORY_FILE, combined.get_cancellation());
      if (!handle_) {
        creation_ = RemoteFileCreationState::not_created;
        throw RemoteFileError("SMB upload parent does not exist", false);
      }
      creation_ = RemoteFileCreationState::created;
    } catch (const RemoteFileError &error) {
      if (!error.connection_lost) creation_ = RemoteFileCreationState::not_created;
      failure_ = std::current_exception();
      throw;
    } catch (...) {
      failure_ = std::current_exception();
      throw;
    }
  }

public:
  SmbWriter(std::shared_ptr<SmbSession> session, std::string path,
      uint64_t expected, cardio::cancellation cancellation)
      : session_(std::move(session)), path_(std::move(path)), expected_(expected),
        parent_cancellation_(std::move(cancellation)) {}
  ~SmbWriter() override { if (handle_ && !closed_) session_->request_stop(); }
  RemoteFileCreationState creation_state() const noexcept override { return creation_; }

  cardio::promise<void> write_all_async(std::span<const std::byte> buffer,
      cardio::cancellation cancellation) override {
    ScopedSmbCancellation combined{cardio::cancellations::any(cancellation, parent_cancellation_)};
    auto lock = std::move(co_await session_->operations.lock(combined.get_cancellation()));
    require_connected(*session_);
    if (closed_) throw RemoteFileError("SMB writer is closed", false);
    if (buffer.size() > expected_ - offset_)
      throw RemoteFileError("SMB upload exceeds its expected size", false);
    co_await create_async(combined.get_cancellation());
    while (!buffer.empty()) {
      const auto amount = std::min<std::size_t>(buffer.size(), smb2_get_max_write_size(session_->context));
      if (!amount) throw RemoteFileError("SMB server supplied an invalid write limit", true);
      smb2_write_request request{};
      request.length = amount;
      request.offset = offset_;
      request.buf = reinterpret_cast<const uint8_t *>(buffer.data());
      std::memcpy(request.file_id, handle_->file_id, SMB2_FD_SIZE);
      std::size_t written = 0;
      auto result = co_await execute_async(session_, [&](auto cb, auto data) {
        auto *pdu = smb2_cmd_write_async(session_->context, &request, 0, cb, data);
        if (!pdu) return -ENOMEM;
        smb2_queue_pdu(session_->context, pdu);
        return 0;
      }, [&](void *data) {
        if (!data) throw RemoteFileError("SMB write response is missing", true);
        written = static_cast<smb2_write_reply *>(data)->count;
        if (!written || written > amount)
          throw RemoteFileError("SMB server returned an invalid write count", true);
      }, combined.get_cancellation());
      check(result, "write");
      offset_ += written;
      buffer = buffer.subspan(written);
    }
  }

  cardio::promise<void> close_async(cardio::cancellation cancellation) override {
    auto lock = std::move(co_await session_->operations.lock(cancellation));
    if (closed_) co_return;
    require_connected(*session_);
    co_await create_async(cancellation);
    if (offset_ == expected_) {
      smb2_flush_request request{};
      std::memcpy(request.file_id, handle_->file_id, SMB2_FD_SIZE);
      auto result = co_await execute_async(session_, [&](auto cb, auto data) {
        auto *pdu = smb2_cmd_flush_async(session_->context, &request, cb, data);
        if (!pdu) return -ENOMEM;
        smb2_queue_pdu(session_->context, pdu);
        return 0;
      }, {}, cancellation);
      check(result, "flush");
    }
    co_await close_raw_async(session_, *handle_, cancellation);
    closed_ = true;
    if (offset_ != expected_)
      throw RemoteFileError("SMB upload ended before its expected size", false);
  }
};

static cardio::promise<void> set_info_async(const std::shared_ptr<SmbSession> &session,
    const smb2_create_reply &handle, uint8_t information_class, void *information,
    cardio::cancellation cancellation) {
  smb2_set_info_request request{};
  request.info_type = SMB2_0_INFO_FILE;
  request.file_info_class = information_class;
  request.input_data = information;
  std::memcpy(request.file_id, handle.file_id, SMB2_FD_SIZE);
  auto result = co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_set_info_async(session->context, &request, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session->context, pdu);
    return 0;
  }, {}, cancellation);
  check(result, "update item");
}

// Attribute changes, deletion and renaming operate on the opened item itself,
// including the no-follow check. Always release that handle on operation errors.
static cardio::promise<void> modify_async(const std::shared_ptr<SmbSession> &session,
    const std::string &path, uint32_t access, std::optional<RemoteFileType> required_type,
    std::function<cardio::promise<void>(const smb2_create_reply &)> operation,
    cardio::cancellation cancellation) {
  if (path == "/") throw RemoteFileError("SMB share root cannot be changed", false);
  if (!co_await inspect_ancestors_async(session, path, cancellation))
    throw RemoteFileError("SMB item does not exist", false);
  auto handle = co_await open_raw_async(session, path, access | SMB2_FILE_READ_ATTRIBUTES,
      SMB2_FILE_OPEN, 0, cancellation);
  if (!handle) throw RemoteFileError("SMB item does not exist", false);
  std::exception_ptr failure;
  try {
    const auto type = file_type(handle->file_attributes);
    if (type == RemoteFileType::other || (required_type && type != *required_type))
      throw RemoteFileError("Unsupported SMB item type", false);
    co_await operation(*handle);
  } catch (...) { failure = std::current_exception(); }
  try { co_await close_raw_async(session, *handle, cancellation); }
  catch (...) {
    session->request_stop();
    if (!failure) failure = std::current_exception();
  }
  if (failure) std::rethrow_exception(failure);
}

static cardio::promise<void> rename_raw_async(const std::shared_ptr<SmbSession> &session,
    const std::string &source, const std::string &destination, bool overwrite,
    cardio::cancellation cancellation) {
  if (destination == "/") throw RemoteFileError("SMB share root cannot be replaced", false);
  if (!co_await inspect_ancestors_async(session, destination, cancellation))
    throw RemoteFileError("SMB destination parent does not exist", false);
  auto target = co_await open_raw_async(session, destination, SMB2_FILE_READ_ATTRIBUTES, SMB2_FILE_OPEN, 0, cancellation);
  if (target) {
    const auto type = file_type(target->file_attributes);
    co_await close_raw_async(session, *target, cancellation);
    if (type == RemoteFileType::other) throw RemoteFileError("SMB links cannot be replaced", false);
  }
  co_await modify_async(session, source, SMB2_DELETE, {}, [&](const auto &handle) -> cardio::promise<void> {
    const auto name = share_path(destination);
    smb2_file_rename_info information{};
    information.replace_if_exist = overwrite;
    information.file_name = reinterpret_cast<const uint8_t *>(name.c_str());
    co_await set_info_async(session, handle, SMB2_FILE_RENAME_INFORMATION, &information, cancellation);
  }, cancellation);
}

class SmbClient final : public RemoteFileClient {
  std::shared_ptr<SmbSession> session_;
public:
  explicit SmbClient(std::shared_ptr<SmbSession> session) : session_(std::move(session)) {}
  ~SmbClient() override { session_->request_stop(); }
  RemoteFileCapabilities capabilities() const noexcept override { return {.write_operations = true, .modification_time = true, .upload_commit = true}; }
  cardio::promise<void> stop_async() {
    session_->request_stop();
    auto lock = std::move(co_await session_->operations.lock());
    session_->abort();
  }
  cardio::promise<RemoteDirectorySnapshot> load_directory_async(std::string path,
      cardio::cancellation cancellation) override {
    path = canonical_path(std::move(path));
    auto lock = std::move(co_await session_->operations.lock(cancellation));
    require_connected(*session_);
    if (!co_await inspect_ancestors_async(session_, path, cancellation))
      throw RemoteFileError("SMB directory does not exist", false);
    auto handle = co_await open_raw_async(session_, path, SMB2_FILE_READ_ATTRIBUTES, SMB2_FILE_OPEN, 0, cancellation);
    if (!handle) throw RemoteFileError("SMB directory does not exist", false);
    const auto type = file_type(handle->file_attributes);
    co_await close_raw_async(session_, *handle, cancellation);
    if (type != RemoteFileType::directory) throw RemoteFileError("SMB path is not a regular directory", false);
    const auto name = share_path(path);
    auto result = co_await execute_async(session_, [&](auto cb, auto data) {
      return smb2_opendir_async(session_->context, name.c_str(), cb, data);
    }, {}, cancellation);
    check(result, "list directory");
    auto *directory = static_cast<smb2dir *>(result.data);
    RemoteDirectorySnapshot snapshot{path, {}};
    try {
      while (auto *entry = smb2_readdir(session_->context, directory)) {
        const std::string entry_name(entry->name);
        if (entry_name == "." || entry_name == "..") continue;
        if (entry_name.find_first_of("/\\") != std::string::npos)
          throw RemoteFileError("Invalid SMB directory entry name", false);
        const auto child_path = canonical_path((path == "/" ? "" : path) + "/" + entry_name);
        snapshot.entries.push_back({.name = entry_name, .path = child_path,
            .type = file_type(entry->st.smb2_attributes), .size = entry->st.smb2_size,
            .permissions = {}, .access_time_unix_seconds = entry->st.smb2_atime,
            .modification_time_unix_seconds = entry->st.smb2_mtime});
      }
    } catch (...) { smb2_closedir(session_->context, directory); throw; }
    smb2_closedir(session_->context, directory);
    co_return snapshot;
  }
  cardio::promise<std::optional<RemoteFileAttributes>> lstat_async(std::string path,
      cardio::cancellation cancellation) override {
    path = canonical_path(std::move(path));
    auto lock = std::move(co_await session_->operations.lock(cancellation));
    require_connected(*session_);
    if (!co_await inspect_ancestors_async(session_, path, cancellation)) co_return std::nullopt;
    auto handle = co_await open_raw_async(session_, path, SMB2_FILE_READ_ATTRIBUTES, SMB2_FILE_OPEN, 0, cancellation);
    if (!handle) co_return std::nullopt;
    auto attributes = attributes_for(path, *handle);
    co_await close_raw_async(session_, *handle, cancellation);
    co_return attributes;
  }
  cardio::promise<std::unique_ptr<RemoteFileReader>> open_read_async(std::string path,
      cardio::cancellation cancellation) override {
    path = canonical_path(std::move(path));
    auto lock = std::move(co_await session_->operations.lock(cancellation));
    require_connected(*session_);
    if (!co_await inspect_ancestors_async(session_, path, cancellation))
      throw RemoteFileError("SMB file does not exist", false);
    auto handle = co_await open_raw_async(session_, path, SMB2_FILE_READ_DATA | SMB2_FILE_READ_ATTRIBUTES,
        SMB2_FILE_OPEN, 0, cancellation);
    if (!handle) throw RemoteFileError("SMB file does not exist", false);
    if (file_type(handle->file_attributes) != RemoteFileType::regular) {
      co_await close_raw_async(session_, *handle, cancellation);
      throw RemoteFileError("SMB downloads require a regular file", false);
    }
    co_return std::make_unique<SmbReader>(session_, *handle);
  }
  cardio::promise<std::string> read_link_async(std::string, cardio::cancellation) override {
    throw RemoteFileError("SMB links are not supported", false); co_return "";
  }
  cardio::promise<void> make_directory_async(std::string path, std::optional<uint32_t>,
      cardio::cancellation cancellation) override {
    path = canonical_path(std::move(path));
    auto lock = std::move(co_await session_->operations.lock(cancellation));
    require_connected(*session_);
    if (path == "/") throw RemoteFileError("SMB share root already exists", false);
    if (!co_await inspect_ancestors_async(session_, path, cancellation))
      throw RemoteFileError("SMB parent directory does not exist", false);
    auto handle = co_await open_raw_async(session_, path, SMB2_FILE_READ_ATTRIBUTES,
        SMB2_FILE_CREATE, SMB2_FILE_DIRECTORY_FILE, cancellation);
    if (!handle) throw RemoteFileError("SMB parent directory does not exist", false);
    co_await close_raw_async(session_, *handle, cancellation);
  }
  cardio::promise<void> remove_file_async(std::string path, cardio::cancellation cancellation) override {
    path = canonical_path(std::move(path));
    auto lock = std::move(co_await session_->operations.lock(cancellation));
    require_connected(*session_);
    co_await modify_async(session_, path, SMB2_DELETE, RemoteFileType::regular,
        [&](const auto &handle) -> cardio::promise<void> {
      smb2_file_disposition_info information{1};
      co_await set_info_async(session_, handle, SMB2_FILE_DISPOSITION_INFORMATION, &information, cancellation);
    }, cancellation);
  }
  cardio::promise<void> remove_directory_async(std::string path, cardio::cancellation cancellation) override {
    path = canonical_path(std::move(path));
    auto lock = std::move(co_await session_->operations.lock(cancellation));
    require_connected(*session_);
    co_await modify_async(session_, path, SMB2_DELETE, RemoteFileType::directory,
        [&](const auto &handle) -> cardio::promise<void> {
      smb2_file_disposition_info information{1};
      co_await set_info_async(session_, handle, SMB2_FILE_DISPOSITION_INFORMATION, &information, cancellation);
    }, cancellation);
  }
  cardio::promise<void> rename_async(std::string source, std::string destination,
      cardio::cancellation cancellation) override {
    source = canonical_path(std::move(source));
    destination = canonical_path(std::move(destination));
    auto lock = std::move(co_await session_->operations.lock(cancellation));
    require_connected(*session_);
    co_await rename_raw_async(session_, source, destination, false, cancellation);
  }
  cardio::promise<void> commit_upload_async(std::string source, std::string destination,
      bool overwrite, cardio::cancellation cancellation) override {
    source = canonical_path(std::move(source));
    destination = canonical_path(std::move(destination));
    auto lock = std::move(co_await session_->operations.lock(cancellation));
    require_connected(*session_);
    co_await rename_raw_async(session_, source, destination, overwrite, cancellation);
  }
  cardio::promise<void> make_symbolic_link_async(std::string, std::string, cardio::cancellation) override {
    throw RemoteFileError("SMB links are not supported", false); co_return;
  }
  cardio::promise<void> set_attributes_async(std::string path, RemoteFileAttributes attributes,
      cardio::cancellation cancellation) override {
    if (!attributes.modification_time_unix_seconds) co_return;
    path = canonical_path(std::move(path));
    auto lock = std::move(co_await session_->operations.lock(cancellation));
    require_connected(*session_);
    co_await modify_async(session_, path, SMB2_FILE_WRITE_ATTRIBUTES, {},
        [&](const auto &handle) -> cardio::promise<void> {
      smb2_file_basic_info information{};
      information.last_write_time.tv_sec = *attributes.modification_time_unix_seconds;
      co_await set_info_async(session_, handle, SMB2_FILE_BASIC_INFORMATION, &information, cancellation);
    }, cancellation);
  }
  cardio::promise<std::unique_ptr<RemoteFileWriter>> open_write_async(std::string path, uint64_t expected,
      std::optional<uint32_t>, cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    require_connected(*session_);
    path = canonical_path(std::move(path));
    if (path == "/") throw RemoteFileError("SMB upload requires a file path", false);
    co_return std::make_unique<SmbWriter>(session_, std::move(path), expected, cancellation);
  }
  bool try_begin_transfer() override {
    if (session_->transfer_active) return false;
    session_->transfer_active = true;
    return true;
  }
  void end_transfer() override { session_->transfer_active = false; }
};

static cardio::promise<std::vector<std::string>> resolve_async(std::string host,
    cardio::cancellation cancellation) {
  auto resolver = std::shared_ptr<GResolver>(g_resolver_get_default(), g_object_unref);
  auto pending = cardio::gio::submit<GList *>(
      [resolver, host](GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
        g_resolver_lookup_by_name_async(resolver.get(), host.c_str(), cancel, callback, data);
      }, [](GObject *object, GAsyncResult *result, GError **error) {
        return g_resolver_lookup_by_name_finish(G_RESOLVER(object), result, error);
      }, cancellation);
  auto addresses = std::unique_ptr<GList, decltype(&g_resolver_free_addresses)>(
      co_await pending, g_resolver_free_addresses);
  cancellation.throw_if_cancellation_requested();
  std::vector<std::string> result;
  for (auto *node = addresses.get(); node; node = node->next) {
    gchar *address = g_inet_address_to_string(G_INET_ADDRESS(node->data));
    result.emplace_back(address);
    g_free(address);
  }
  co_return result;
}

static cardio::promise<std::shared_ptr<SmbSession>> open_session_async(
    SmbClientOpenOptions options, std::string share, cardio::cancellation cancellation) {
  if (!options.connection.validation_errors.empty())
    throw std::invalid_argument(options.connection.validation_errors.front());
  auto session = std::make_shared<SmbSession>();
  session->idle_timeout_ms = static_cast<int>(options.connection.idle_timeout_seconds * 1000);
  ScopedSmbCancellation deadline{cardio::cancellations::timeout(options.connection.connect_timeout_seconds * 1000)};
  ScopedSmbCancellation canceled{cardio::cancellations::any(cancellation, deadline.get_cancellation())};
  std::exception_ptr failure;
  try {
    const auto addresses = co_await resolve_async(options.connection.address, canceled.get_cancellation());
    for (const auto &ip : addresses) {
      canceled.get_cancellation().throw_if_cancellation_requested();
      session->abort();
      session->context = smb2_init_context();
      if (!session->context) throw std::bad_alloc();
      auto *url = smb2_parse_url(session->context, "smb://localhost/share?sec=ntlmssp");
      if (!url) throw RemoteFileError("SMB NTLMSSP authentication is unavailable", false);
      smb2_destroy_url(url);
      smb2_set_user(session->context, options.connection.username.c_str());
      smb2_set_password(session->context, options.password.c_str());
      smb2_set_domain(session->context, options.connection.domain.c_str());
      smb2_set_sign(session->context, options.connection.require_signing);
      smb2_set_security_mode(session->context, SMB2_NEGOTIATE_SIGNING_ENABLED |
          (options.connection.require_signing ? SMB2_NEGOTIATE_SIGNING_REQUIRED : 0));
      if (options.connection.require_encryption) smb2_set_seal(session->context, 1);
      const auto address = (ip.find(':') != std::string::npos ? "[" + ip + "]" : ip) + ":" +
          std::to_string(options.connection.port);
      try {
        check(co_await execute_async(session, [&](auto cb, auto data) {
          return smb2_connect_share_async(session->context, address.c_str(), share.c_str(),
              options.connection.username.c_str(), cb, data);
        }, {}, canceled.get_cancellation()), "connect");
        session->disconnected = std::move(options.disconnected);
        (void)deadline.cancel();
        co_return session;
      } catch (const cardio::canceled_exception &) { throw; }
      catch (...) { failure = std::current_exception(); }
    }
    if (failure) std::rethrow_exception(failure);
    throw RemoteFileError("SMB server has no usable address", true);
  } catch (const cardio::canceled_exception &) {
    if (cancellation.is_cancellation_requested()) throw;
    throw RemoteFileError("SMB connection timed out", true);
  }
}

struct SmbServerLifetime {
  cardio::cancellation_source stopping;
  std::function<void()> disconnected;
  bool lost = false;
};

struct SmbServerPath {
  std::string canonical;
  std::string share;
  std::string relative;
};

static SmbServerPath server_path(std::string path) {
  path = canonical_path(std::move(path));
  if (path == "/") return {"/", {}, "/"};
  const auto end = path.find('/', 1);
  const auto share = path.substr(1, end == std::string::npos ? end : end - 1);
  if (g_ascii_strcasecmp(share.c_str(), "IPC$") == 0)
    throw RemoteFileError("IPC$ is not a file share", false);
  return {path, share, end == std::string::npos ? "/" : path.substr(end)};
}

static std::string share_key(const std::string &share) {
  if (!g_utf8_validate(share.c_str(), -1, nullptr)) throw RemoteFileError("Invalid SMB share name", false);
  auto *folded = g_utf8_casefold(share.c_str(), -1);
  std::string result(folded);
  g_free(folded);
  return result;
}

static void shares_completed(smb2_context *context, int status, void *data, void *opaque) {
  // Share enumeration owns its decoded reply even for positive WERROR results.
  // completed() retains only copied data and captures exceptions from decoding.
  completed(context, status, data, opaque);
  if (data) smb2_free_data(context, data);
  static_cast<SmbCompletion *>(opaque)->data = nullptr;
}

class SmbServerClient final : public RemoteFileClient {
  SmbClientOpenOptions options_;
  std::shared_ptr<SmbServerLifetime> lifetime_ = std::make_shared<SmbServerLifetime>();
  cardio::primitives::mutex connections_;
  std::map<std::string, std::shared_ptr<SmbClient>> shares_;
  std::shared_ptr<SmbSession> enumeration_;
  bool transferring_ = false;

  void require_alive() const {
    if (lifetime_->lost || lifetime_->stopping.get_cancellation().is_cancellation_requested())
      throw RemoteFileError("SMB server connection has ended. Reconnect to continue.", true);
  }

  static void require_mutable(const SmbServerPath &path) {
    if (path.share.empty() || path.relative == "/")
      throw RemoteFileError("SMB server and share roots cannot be changed", false);
  }

  cardio::promise<std::shared_ptr<SmbClient>> share_async(const std::string &share,
      cardio::cancellation cancellation) {
    require_alive();
    ScopedSmbCancellation stop{cardio::cancellations::any(cancellation, lifetime_->stopping.get_cancellation())};
    auto lock = std::move(co_await connections_.lock(stop.get_cancellation()));
    require_alive();
    const auto key = share_key(share);
    const auto found = shares_.find(key);
    if (found != shares_.end()) co_return found->second;
    auto session = co_await open_session_async(options_, share, stop.get_cancellation());
    auto client = std::make_shared<SmbClient>(std::move(session));
    shares_.emplace(key, client);
    co_return client;
  }

  cardio::promise<RemoteDirectorySnapshot> list_shares_async(cardio::cancellation cancellation) {
    require_alive();
    ScopedSmbCancellation stop{cardio::cancellations::any(cancellation, lifetime_->stopping.get_cancellation())};
    std::shared_ptr<SmbSession> session;
    {
      auto lock = std::move(co_await connections_.lock(stop.get_cancellation()));
      require_alive();
      if (!enumeration_) enumeration_ = co_await open_session_async(options_, "IPC$", stop.get_cancellation());
      session = enumeration_;
    }
    auto lock = std::move(co_await session->operations.lock(stop.get_cancellation()));
    RemoteDirectorySnapshot listing;
    const auto result = co_await execute_async(session, [&](auto, auto data) {
      return smb2_share_enum_async(session->context, SMB2_SHARE_INFO_1, shares_completed, data);
    }, [&](void *data) {
      listing = decode_smb_share_list(0, static_cast<smb2_share_enum_reply *>(data));
    }, stop.get_cancellation());
    if (result.status != 0) (void)decode_smb_share_list(result.status, nullptr);
    co_return listing;
  }

public:
  explicit SmbServerClient(SmbClientOpenOptions options) : options_(std::move(options)) {
    lifetime_->disconnected = std::move(options_.disconnected);
    options_.disconnected = [weak = std::weak_ptr<SmbServerLifetime>(lifetime_)] {
      auto state = weak.lock();
      if (!state || state->lost || state->stopping.get_cancellation().is_cancellation_requested()) return;
      state->lost = true;
      if (state->disconnected) state->disconnected();
    };
  }
  ~SmbServerClient() override {
    lifetime_->stopping.cancel();
    if (enumeration_) enumeration_->request_stop();
  }
  RemoteFileCapabilities capabilities() const noexcept override {
    return {.write_operations = true, .modification_time = true, .upload_commit = true};
  }
  RemotePathCapabilities path_capabilities(const std::string &path) const override {
    const auto parts = server_path(path);
    return {.create_children = !parts.share.empty(), .modify = !parts.share.empty() && parts.relative != "/"};
  }
  cardio::promise<void> stop_async() {
    lifetime_->stopping.cancel();
    auto lock = std::move(co_await connections_.lock());
    if (enumeration_) enumeration_->request_stop();
    for (const auto &[name, client] : shares_) { (void)name; co_await client->stop_async(); }
    if (enumeration_) {
      auto operation = std::move(co_await enumeration_->operations.lock());
      enumeration_->abort();
    }
    shares_.clear();
    enumeration_.reset();
  }
  cardio::promise<RemoteDirectorySnapshot> load_directory_async(std::string path,
      cardio::cancellation cancellation) override {
    const auto parts = server_path(std::move(path));
    if (parts.share.empty()) co_return co_await list_shares_async(cancellation);
    auto client = co_await share_async(parts.share, cancellation);
    auto result = co_await client->load_directory_async(parts.relative, cancellation);
    const auto prefix = "/" + parts.share;
    result.canonical_path = prefix + (result.canonical_path == "/" ? "" : result.canonical_path);
    for (auto &entry : result.entries) entry.path = prefix + entry.path;
    co_return result;
  }
  cardio::promise<std::optional<RemoteFileAttributes>> lstat_async(std::string path,
      cardio::cancellation cancellation) override {
    require_alive();
    cancellation.throw_if_cancellation_requested();
    const auto parts = server_path(std::move(path));
    if (parts.share.empty()) co_return RemoteFileAttributes{.name = {}, .path = "/", .type = RemoteFileType::directory,
        .size = {}, .permissions = {}, .access_time_unix_seconds = {}, .modification_time_unix_seconds = {}};
    auto client = co_await share_async(parts.share, cancellation);
    auto result = co_await client->lstat_async(parts.relative, cancellation);
    if (result) {
      result->path = "/" + parts.share + (result->path == "/" ? "" : result->path);
      if (parts.relative == "/") result->name = parts.share;
    }
    co_return result;
  }
  cardio::promise<std::unique_ptr<RemoteFileReader>> open_read_async(std::string path,
      cardio::cancellation cancellation) override {
    const auto parts = server_path(std::move(path));
    if (parts.share.empty() || parts.relative == "/") throw RemoteFileError("SMB read requires a file path", false);
    auto client = co_await share_async(parts.share, cancellation);
    co_return std::move(co_await client->open_read_async(parts.relative, cancellation));
  }
  cardio::promise<std::unique_ptr<RemoteFileWriter>> open_write_async(std::string path, uint64_t size,
      std::optional<uint32_t> permissions, cardio::cancellation cancellation) override {
    const auto parts = server_path(std::move(path)); require_mutable(parts);
    auto client = co_await share_async(parts.share, cancellation);
    co_return std::move(co_await client->open_write_async(parts.relative, size, permissions, cancellation));
  }
  cardio::promise<void> make_directory_async(std::string path, std::optional<uint32_t> permissions,
      cardio::cancellation cancellation) override {
    const auto parts = server_path(std::move(path)); require_mutable(parts);
    auto client = co_await share_async(parts.share, cancellation);
    co_await client->make_directory_async(parts.relative, permissions, cancellation);
  }
  cardio::promise<void> remove_file_async(std::string path, cardio::cancellation cancellation) override {
    const auto parts = server_path(std::move(path)); require_mutable(parts);
    auto client = co_await share_async(parts.share, cancellation);
    co_await client->remove_file_async(parts.relative, cancellation);
  }
  cardio::promise<void> remove_directory_async(std::string path, cardio::cancellation cancellation) override {
    const auto parts = server_path(std::move(path)); require_mutable(parts);
    auto client = co_await share_async(parts.share, cancellation);
    co_await client->remove_directory_async(parts.relative, cancellation);
  }
  cardio::promise<void> rename_async(std::string source, std::string destination,
      cardio::cancellation cancellation) override {
    const auto from = server_path(std::move(source)), to = server_path(std::move(destination));
    require_mutable(from); require_mutable(to);
    if (share_key(from.share) != share_key(to.share)) throw RemoteFileError("SMB rename cannot cross shares", false);
    auto client = co_await share_async(from.share, cancellation);
    co_await client->rename_async(from.relative, to.relative, cancellation);
  }
  cardio::promise<void> commit_upload_async(std::string source, std::string destination,
      bool overwrite, cardio::cancellation cancellation) override {
    const auto from = server_path(std::move(source)), to = server_path(std::move(destination));
    require_mutable(from); require_mutable(to);
    if (share_key(from.share) != share_key(to.share)) throw RemoteFileError("SMB upload commit cannot cross shares", false);
    auto client = co_await share_async(from.share, cancellation);
    co_await client->commit_upload_async(from.relative, to.relative, overwrite, cancellation);
  }
  cardio::promise<void> set_attributes_async(std::string path, RemoteFileAttributes attributes,
      cardio::cancellation cancellation) override {
    const auto parts = server_path(std::move(path)); require_mutable(parts);
    auto client = co_await share_async(parts.share, cancellation);
    co_await client->set_attributes_async(parts.relative, std::move(attributes), cancellation);
  }
  cardio::promise<std::string> read_link_async(std::string, cardio::cancellation) override {
    throw RemoteFileError("SMB links are not supported", false); co_return "";
  }
  cardio::promise<void> make_symbolic_link_async(std::string, std::string, cardio::cancellation) override {
    throw RemoteFileError("SMB links are not supported", false); co_return;
  }
  bool try_begin_transfer() override { if (transferring_) return false; transferring_ = true; return true; }
  void end_transfer() override { transferring_ = false; }
};

cardio::promise<std::shared_ptr<RemoteFileClient>> open_smb_client_async(
    SmbClientOpenOptions options, cardio::cancellation cancellation) {
  const auto share = options.connection.share;
  auto session = co_await open_session_async(std::move(options), share, cancellation);
  co_return std::make_shared<SmbClient>(std::move(session));
}

cardio::promise<void> stop_smb_client_async(std::shared_ptr<RemoteFileClient> client) {
  if (auto smb = std::dynamic_pointer_cast<SmbClient>(client)) co_await smb->stop_async();
  if (auto server = std::dynamic_pointer_cast<SmbServerClient>(client)) co_await server->stop_async();
}

cardio::promise<std::shared_ptr<RemoteFileClient>> open_smb_server_async(
    SmbClientOpenOptions options, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (!options.connection.validation_errors.empty()) throw std::invalid_argument(options.connection.validation_errors.front());
  co_return std::make_shared<SmbServerClient>(std::move(options));
}

} // namespace elder_terms

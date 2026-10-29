// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "smb-client.h"

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/smb2-errors.h>
#include <poll.h>
#include <algorithm>
#include <cstring>
#include <functional>

namespace elder_terms {

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
  auto stopped = cardio::cancellations::any(cancellation, session->stopping.get_cancellation());
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
      auto deadline = cardio::cancellations::timeout(session->idle_timeout_ms);
      auto waiting = cardio::cancellations::any(stopped.get_cancellation(), deadline.get_cancellation());
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
    uint32_t access, uint32_t disposition, cardio::cancellation cancellation) {
  const auto name = share_path(path);
  smb2_create_request request{};
  request.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
  request.desired_access = access;
  request.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE | SMB2_FILE_SHARE_DELETE;
  request.create_disposition = disposition;
  request.create_options = SMB2_FILE_OPEN_REPARSE_POINT;
  request.name = name.c_str();
  smb2_create_reply handle{};
  auto result = co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_create_async(session->context, &request, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session->context, pdu);
    return 0;
  }, [&](void *data) { handle = *static_cast<smb2_create_reply *>(data); }, cancellation);
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
    auto handle = co_await open_raw_async(session, parent, SMB2_FILE_READ_ATTRIBUTES, SMB2_FILE_OPEN, cancellation);
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

class SmbClient final : public RemoteFileClient {
  std::shared_ptr<SmbSession> session_;
public:
  explicit SmbClient(std::shared_ptr<SmbSession> session) : session_(std::move(session)) {}
  ~SmbClient() override { session_->request_stop(); }
  RemoteFileCapabilities capabilities() const noexcept override { return {.write_operations = false}; }
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
    auto handle = co_await open_raw_async(session_, path, SMB2_FILE_READ_ATTRIBUTES, SMB2_FILE_OPEN, cancellation);
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
    auto handle = co_await open_raw_async(session_, path, SMB2_FILE_READ_ATTRIBUTES, SMB2_FILE_OPEN, cancellation);
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
        SMB2_FILE_OPEN, cancellation);
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
  cardio::promise<void> make_directory_async(std::string, std::optional<uint32_t>, cardio::cancellation) override {
    throw RemoteFileError("SMB changes are not available", false); co_return;
  }
  cardio::promise<void> remove_file_async(std::string, cardio::cancellation) override {
    throw RemoteFileError("SMB changes are not available", false); co_return;
  }
  cardio::promise<void> remove_directory_async(std::string, cardio::cancellation) override {
    throw RemoteFileError("SMB changes are not available", false); co_return;
  }
  cardio::promise<void> rename_async(std::string, std::string, cardio::cancellation) override {
    throw RemoteFileError("SMB changes are not available", false); co_return;
  }
  cardio::promise<void> make_symbolic_link_async(std::string, std::string, cardio::cancellation) override {
    throw RemoteFileError("SMB links are not supported", false); co_return;
  }
  cardio::promise<void> set_attributes_async(std::string, RemoteFileAttributes, cardio::cancellation) override { co_return; }
  cardio::promise<std::unique_ptr<RemoteFileWriter>> open_write_async(std::string, uint64_t,
      std::optional<uint32_t>, cardio::cancellation) override {
    throw RemoteFileError("SMB changes are not available", false); co_return nullptr;
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

cardio::promise<std::shared_ptr<RemoteFileClient>> open_smb_client_async(
    SmbClientOpenOptions options, cardio::cancellation cancellation) {
  if (!options.connection.validation_errors.empty())
    throw std::invalid_argument(options.connection.validation_errors.front());
  auto session = std::make_shared<SmbSession>();
  session->idle_timeout_ms = static_cast<int>(options.connection.idle_timeout_seconds * 1000);
  auto deadline = cardio::cancellations::timeout(options.connection.connect_timeout_seconds * 1000);
  auto canceled = cardio::cancellations::any(cancellation, deadline.get_cancellation());
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
          return smb2_connect_share_async(session->context, address.c_str(), options.connection.share.c_str(),
              options.connection.username.c_str(), cb, data);
        }, {}, canceled.get_cancellation()), "connect");
        session->disconnected = std::move(options.disconnected);
        (void)deadline.cancel();
        co_return std::make_shared<SmbClient>(session);
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

cardio::promise<void> stop_smb_client_async(std::shared_ptr<RemoteFileClient> client) {
  if (auto smb = std::dynamic_pointer_cast<SmbClient>(client)) co_await smb->stop_async();
}

} // namespace elder_terms

// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "smb1-client.h"
#include "smb1-protocol.h"
#include <algorithm>
#include <filesystem>
#include <cstring>

namespace elder_terms {

using smb1::Command;
using smb1::Payload;
using smb1::Cursor;

struct Smb1Cancellation {
  cardio::cancellation_source source;
  ~Smb1Cancellation() { (void)source.cancel(); }
  cardio::cancellation get() const { return source.get_cancellation(); }
};

struct Smb1Session {
  std::shared_ptr<GSubprocess> process;
  std::optional<cardio::promise<void>> reaped;
  cardio::primitives::mutex operations;
  cardio::cancellation_source stopping;
  std::function<void()> disconnected;
  std::uint64_t next_id = 0;
  std::int64_t timeout_ms = 60000;
  bool stopped = false;

  void stop() {
    if (stopped) return;
    stopped = true;
    stopping.cancel();
    if (process) g_subprocess_force_exit(process.get());
    auto notify = std::move(disconnected);
    if (notify) notify();
  }
  ~Smb1Session() { disconnected = {}; stop(); }
};

static cardio::promise<void> watch_worker(std::shared_ptr<GSubprocess> process,
                                         std::weak_ptr<Smb1Session> weak) {
  auto waiting = cardio::gio::submit<bool>(
      [process](GCancellable *cancel, GAsyncReadyCallback callback, void *data) {
        g_subprocess_wait_async(process.get(), cancel, callback, data);
      },
      [process](GObject *, GAsyncResult *result, GError **error) {
        return g_subprocess_wait_finish(process.get(), result, error) != 0;
      });
  (void)co_await waiting;
  if (auto session = weak.lock()) session->stop();
}

static cardio::promise<Payload> request(const std::shared_ptr<Smb1Session> &session,
    Command command, Payload payload, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (session->stopped) throw RemoteFileError("SMB1 connection has ended. Reconnect to continue.", true);
  Smb1Cancellation stopped{cardio::cancellations::any(cancellation, session->stopping.get_cancellation())};
  auto lock = std::move(co_await session->operations.lock(stopped.get()));
  if (session->stopped) throw RemoteFileError("SMB1 connection has ended. Reconnect to continue.", true);
  Smb1Cancellation deadline{cardio::cancellations::timeout(session->timeout_ms)};
  Smb1Cancellation waiting{cardio::cancellations::any(stopped.get(), deadline.get())};
  std::exception_ptr failure;
  try {
    const smb1::Frame frame{++session->next_id, command, std::move(payload)};
    co_await smb1::send_frame(g_subprocess_get_stdin_pipe(session->process.get()), frame, waiting.get());
    auto response = co_await smb1::receive_frame(g_subprocess_get_stdout_pipe(session->process.get()), waiting.get());
    if (response.id != frame.id) throw std::runtime_error("SMB1 helper response identifier mismatch");
    if (response.command == Command::error) {
      Cursor error{response.payload.bytes};
      const bool lost = error.number() != 0;
      auto message = error.text();
      error.finish();
      if (lost) session->stop();
      throw RemoteFileError(message, lost);
    }
    if (response.command != command) throw std::runtime_error("SMB1 helper response operation mismatch");
    co_return response.payload;
  } catch (const RemoteFileError &error) {
    if (!error.connection_lost) throw;
    failure = std::current_exception();
  } catch (const cardio::canceled_exception &) {
    failure = cancellation.is_cancellation_requested() ? std::current_exception() :
        std::make_exception_ptr(RemoteFileError(session->stopped ?
          "SMB1 worker stopped. Reconnect to continue." : "SMB1 operation timed out. Reconnect to continue.", true));
  } catch (const std::exception &error) {
    failure = std::make_exception_ptr(RemoteFileError(error.what(), true));
  }
  // Killing a worker does not undo remote changes. Retire its pipes and reap
  // it before reporting cancellation or an uncertain operation result.
  session->stop();
  if (session->reaped) co_await *session->reaped;
  std::rethrow_exception(failure);
}

static std::string canonical(std::string path) {
  if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos ||
      !g_utf8_validate(path.data(), static_cast<gssize>(path.size()), nullptr))
    throw RemoteFileError("SMB1 requires an absolute UTF-8 path", false);
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  for (std::size_t offset = 1; offset < path.size();) {
    const auto end = path.find('/', offset);
    const auto part = path.substr(offset, end == std::string::npos ? end : end - offset);
    if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ' ||
        part.find_first_of("\\:*?\r\n") != std::string::npos ||
        std::any_of(part.begin(), part.end(), [](unsigned char c) { return c < 32 || c == 127; }))
      throw RemoteFileError("Unsupported SMB1 path component", false);
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  return path;
}

static void mutable_path(const std::string &path) {
  if (path.find('/', 1) == std::string::npos)
    throw RemoteFileError("SMB1 server and share roots cannot be changed", false);
}

static Payload path_payload(const std::string &path) {
  Payload payload; payload.text(canonical(path)); return payload;
}

class Smb1Reader final : public RemoteFileReader {
  std::shared_ptr<Smb1Session> session_;
  std::uint64_t id_;
  bool closed_ = false;
  std::vector<std::byte> buffered_;
  std::size_t offset_ = 0;
public:
  Smb1Reader(std::shared_ptr<Smb1Session> session, std::uint64_t id) : session_(std::move(session)), id_(id) {}
  ~Smb1Reader() override { if (!closed_) session_->stop(); }
  cardio::promise<std::size_t> read_async(std::span<std::byte> buffer, cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    if (closed_) throw RemoteFileError("SMB1 reader is closed", false);
    if (buffer.empty()) co_return 0;
    if (offset_ == buffered_.size()) {
      Payload payload; payload.number(id_);
      const auto response = co_await request(session_, Command::read, std::move(payload), cancellation);
      Cursor cursor{response.bytes};
      const auto bytes = cursor.data();
      if (bytes.size() > smb1::chunk_size) throw RemoteFileError("SMB1 read exceeds the chunk size", true);
      buffered_.assign(bytes.begin(), bytes.end());
      offset_ = 0;
      cursor.finish();
    }
    const auto count = std::min(buffer.size(), buffered_.size() - offset_);
    std::copy_n(buffered_.begin() + static_cast<std::ptrdiff_t>(offset_), count, buffer.begin());
    offset_ += count;
    co_return count;
  }
  cardio::promise<void> close_async(cardio::cancellation cancellation) override {
    if (closed_) co_return;
    closed_ = true;
    Payload payload; payload.number(id_);
    (void)co_await request(session_, Command::close, std::move(payload), cancellation);
  }
};

class Smb1Writer final : public RemoteFileWriter {
  std::shared_ptr<Smb1Session> session_;
  std::string path_;
  std::uint64_t expected_, id_ = 0;
  RemoteFileCreationState creation_ = RemoteFileCreationState::unknown;
  bool closed_ = false;
  cardio::promise<void> prepare(cardio::cancellation cancellation) {
    if (id_ != 0) co_return;
    auto payload = path_payload(path_); payload.number(expected_);
    const auto response = co_await request(session_, Command::write_open, std::move(payload), cancellation);
    Cursor cursor{response.bytes}; id_ = cursor.number(); cursor.finish();
    creation_ = RemoteFileCreationState::created;
  }
public:
  Smb1Writer(std::shared_ptr<Smb1Session> session, std::string path, std::uint64_t expected)
      : session_(std::move(session)), path_(std::move(path)), expected_(expected) {}
  ~Smb1Writer() override { if (!closed_) session_->stop(); }
  RemoteFileCreationState creation_state() const noexcept override { return creation_; }
  cardio::promise<void> write_all_async(std::span<const std::byte> buffer, cardio::cancellation cancellation) override {
    if (closed_) throw RemoteFileError("SMB1 writer is closed", false);
    co_await prepare(cancellation);
    while (!buffer.empty()) {
      const auto chunk = buffer.first(std::min(buffer.size(), smb1::chunk_size));
      Payload payload; payload.number(id_); payload.data(chunk);
      (void)co_await request(session_, Command::write, std::move(payload), cancellation);
      buffer = buffer.subspan(chunk.size());
    }
  }
  cardio::promise<void> close_async(cardio::cancellation cancellation) override {
    if (closed_) co_return;
    try {
      co_await prepare(cancellation);
      Payload payload; payload.number(id_);
      (void)co_await request(session_, Command::close, std::move(payload), cancellation);
    } catch (...) { closed_ = true; throw; }
    closed_ = true;
  }
};

static RemoteFileAttributes decode_attributes(Cursor &cursor, const std::string &parent) {
  RemoteFileAttributes result;
  result.name = cursor.text();
  result.path = canonical(parent + (parent.back() == '/' ? "" : "/") + result.name);
  result.type = cursor.number() ? RemoteFileType::directory : RemoteFileType::regular;
  result.size = cursor.number();
  const auto time = cursor.number();
  if (time >= 116444736000000000ULL)
    result.modification_time_unix_seconds = static_cast<std::int64_t>((time - 116444736000000000ULL) / 10000000);
  return result;
}

class Smb1Client final : public RemoteFileClient {
  std::shared_ptr<Smb1Session> session_;
  bool transferring_ = false;
  cardio::promise<void> mutation(Command command, std::string path, cardio::cancellation cancellation) {
    path = canonical(std::move(path)); mutable_path(path);
    (void)co_await request(session_, command, path_payload(path), cancellation);
  }
public:
  explicit Smb1Client(std::shared_ptr<Smb1Session> session) : session_(std::move(session)) {}
  ~Smb1Client() override { session_->disconnected = {}; session_->stop(); }
  RemoteFileCapabilities capabilities() const noexcept override { return {.write_operations = true, .upload_commit = true}; }
  RemotePathCapabilities path_capabilities(const std::string &path) const override {
    const auto value = canonical(path);
    return {.create_children = value != "/", .modify = value.find('/', 1) != std::string::npos};
  }
  cardio::promise<void> stop() {
    session_->disconnected = {};
    session_->stop();
    auto lock = std::move(co_await session_->operations.lock());
    if (session_->reaped) co_await *session_->reaped;
  }
  cardio::promise<RemoteDirectorySnapshot> load_directory_async(std::string path, cardio::cancellation cancellation) override {
    path = canonical(std::move(path));
    const auto response = co_await request(session_, path == "/" ? Command::shares : Command::list,
        path == "/" ? Payload{} : path_payload(path), cancellation);
    Cursor cursor{response.bytes};
    const auto count = cursor.number();
    if (count > smb1::maximum_frame / 8) throw RemoteFileError("SMB1 listing is too large", false);
    RemoteDirectorySnapshot result{.canonical_path = path, .entries = {}};
    for (std::uint64_t index = 0; index < count; ++index) {
      if (path == "/") {
        auto name = cursor.text();
        RemoteFileAttributes item;
        item.name = name; item.path = canonical("/" + name);
        item.type = RemoteFileType::directory; item.role = RemoteFileRole::smb_share;
        result.entries.push_back(std::move(item));
      } else result.entries.push_back(decode_attributes(cursor, path));
    }
    cursor.finish();
    co_return result;
  }
  cardio::promise<std::optional<RemoteFileAttributes>> lstat_async(std::string path, cardio::cancellation cancellation) override {
    path = canonical(std::move(path));
    if (path == "/") {
      RemoteFileAttributes root;
      root.name = "/"; root.path = "/"; root.type = RemoteFileType::directory;
      co_return root;
    }
    const auto response = co_await request(session_, Command::stat, path_payload(path), cancellation);
    Cursor cursor{response.bytes};
    if (cursor.number() == 0) { cursor.finish(); co_return std::nullopt; }
    auto item = decode_attributes(cursor, parent_path(path));
    if (path.find('/', 1) == std::string::npos) item.role = RemoteFileRole::smb_share;
    cursor.finish(); co_return item;
  }
  cardio::promise<std::unique_ptr<RemoteFileReader>> open_read_async(std::string path, cardio::cancellation cancellation) override {
    const auto response = co_await request(session_, Command::read_open, path_payload(path), cancellation);
    Cursor cursor{response.bytes}; const auto id = cursor.number(); cursor.finish();
    co_return std::make_unique<Smb1Reader>(session_, id);
  }
  cardio::promise<std::unique_ptr<RemoteFileWriter>> open_write_async(std::string path, std::uint64_t expected,
      std::optional<std::uint32_t> permissions, cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    path = canonical(std::move(path)); mutable_path(path);
    if (permissions) throw RemoteFileError("SMB1 cannot set POSIX permissions", false);
    co_return std::make_unique<Smb1Writer>(session_, std::move(path), expected);
  }
  cardio::promise<void> make_directory_async(std::string path, std::optional<std::uint32_t> permissions,
      cardio::cancellation cancellation) override {
    if (permissions) throw RemoteFileError("SMB1 cannot set POSIX permissions", false);
    co_await mutation(Command::mkdir, std::move(path), cancellation);
  }
  cardio::promise<void> remove_file_async(std::string path, cardio::cancellation cancellation) override {
    co_await mutation(Command::remove, std::move(path), cancellation);
  }
  cardio::promise<void> remove_directory_async(std::string path, cardio::cancellation cancellation) override {
    co_await mutation(Command::rmdir, std::move(path), cancellation);
  }
  cardio::promise<void> rename_async(std::string from, std::string to, cardio::cancellation cancellation) override {
    from = canonical(std::move(from)); to = canonical(std::move(to)); mutable_path(from); mutable_path(to);
    auto payload = path_payload(from); payload.text(to);
    (void)co_await request(session_, Command::rename, std::move(payload), cancellation);
  }
  cardio::promise<void> commit_upload_async(std::string from, std::string to, bool overwrite,
      cardio::cancellation cancellation) override {
    from = canonical(std::move(from)); to = canonical(std::move(to)); mutable_path(from); mutable_path(to);
    auto payload = path_payload(from); payload.text(to); payload.number(overwrite);
    (void)co_await request(session_, Command::commit, std::move(payload), cancellation);
  }
  cardio::promise<std::string> read_link_async(std::string, cardio::cancellation) override {
    throw RemoteFileError("SMB1 links are not supported", false); co_return "";
  }
  cardio::promise<void> make_symbolic_link_async(std::string, std::string, cardio::cancellation) override {
    throw RemoteFileError("SMB1 links are not supported", false); co_return;
  }
  cardio::promise<void> set_attributes_async(std::string, RemoteFileAttributes, cardio::cancellation) override {
    throw RemoteFileError("SMB1 metadata changes are not supported", false); co_return;
  }
  bool try_begin_transfer() override { if (transferring_) return false; transferring_ = true; return true; }
  void end_transfer() override { transferring_ = false; }
};

cardio::promise<std::shared_ptr<RemoteFileClient>> open_smb1_server_async(
    SmbClientOpenOptions options, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (!options.connection.validation_errors.empty()) throw std::invalid_argument(options.connection.validation_errors.front());
  if (options.connection.require_signing || options.connection.require_encryption)
    throw std::invalid_argument("SMB1 does not support required signing or encryption");
  if (options.connection.port != 445 && options.connection.port != 139)
    throw std::invalid_argument("SMB1 supports only ports 445 and 139");
  Smb1Cancellation deadline{cardio::cancellations::timeout(options.connection.connect_timeout_seconds * 1000)};
  Smb1Cancellation pending{cardio::cancellations::any(cancellation, deadline.get())};
  const std::shared_ptr<GResolver> resolver(g_resolver_get_default(), g_object_unref);
  auto lookup = cardio::gio::submit<GList *>(
      [resolver, address = options.connection.address](GCancellable *cancel, GAsyncReadyCallback callback, void *data) {
        g_resolver_lookup_by_name_async(resolver.get(), address.c_str(), cancel, callback, data);
      }, [](GObject *object, GAsyncResult *result, GError **error) {
        return g_resolver_lookup_by_name_finish(G_RESOLVER(object), result, error);
      }, pending.get());
  auto *addresses = co_await lookup;
  std::string address;
  for (auto *entry = addresses; entry; entry = entry->next) {
    if (g_inet_address_get_family(G_INET_ADDRESS(entry->data)) != G_SOCKET_FAMILY_IPV4) continue;
    auto *text = g_inet_address_to_string(G_INET_ADDRESS(entry->data)); address = text; g_free(text); break;
  }
  g_resolver_free_addresses(addresses);
  if (address.empty()) throw std::invalid_argument("SMB1 requires an IPv4 address");
  const auto executable = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "elder-terms-smb1-worker";
  GError *error = nullptr;
  auto *process = g_subprocess_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE),
      &error, executable.c_str(), nullptr);
  if (!process) {
    const std::string message = error ? error->message : "Could not start SMB1 worker";
    g_clear_error(&error); throw RemoteFileError(message, true);
  }
  auto session = std::make_shared<Smb1Session>();
  session->process = std::shared_ptr<GSubprocess>(process, g_object_unref);
  session->timeout_ms = options.connection.connect_timeout_seconds * 1000;
  session->reaped.emplace(watch_worker(session->process, session));
  Payload payload; payload.text(address); payload.text(options.connection.address); payload.number(options.connection.port);
  payload.text(options.connection.domain); payload.text(options.connection.username); payload.text(options.password);
  std::exception_ptr failure;
  try { (void)co_await request(session, Command::connect, std::move(payload), pending.get()); }
  catch (...) { failure = std::current_exception(); }
  if (failure) {
    session->stop();
    co_await *session->reaped;
    std::rethrow_exception(failure);
  }
  session->timeout_ms = options.connection.idle_timeout_seconds * 1000;
  session->disconnected = std::move(options.disconnected);
  co_return std::make_shared<Smb1Client>(std::move(session));
}

cardio::promise<void> stop_smb1_client_async(std::shared_ptr<RemoteFileClient> client) {
  if (auto smb = std::dynamic_pointer_cast<Smb1Client>(client)) co_await smb->stop();
}

} // namespace elder_terms

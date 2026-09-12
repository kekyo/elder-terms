#include "webdav-client.h"
#include "curl-http-session.h"
#include "webdav-url.h"
#include "../../src/file-transfer/file-transfer-engine.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}



struct PausedResponse {
  cardio::promise_source<void> paused;
  bool resume = false;
  std::string body;
  bool finished = false;
  std::size_t callbacks = 0;
  elder_terms::CurlHttpResult result;
  std::exception_ptr failure;
};


static cardio::promise<void> perform_paused_request_async(
    std::shared_ptr<elder_terms::CurlHttpSession> session,
    elder_terms::CurlHttpRequest request, std::shared_ptr<PausedResponse> state,
    cardio::cancellation cancellation) {
  try { state->result = co_await elder_terms::perform_http_request_async(session, std::move(request), cancellation); }
  catch (...) { state->failure = std::current_exception(); }
  state->finished = true;
  state->paused.try_resolve();
}

static cardio::promise<void> check_paused_http_async(elder_terms::WebdavClientOpenOptions options) {
  using namespace elder_terms;
  options.connection.idle_timeout_seconds = 1;
  const auto endpoint = webdav_endpoint(options.connection);
  auto session = create_curl_http_session(options.connection, options.password);
  auto probe_session = create_curl_http_session(options.connection, options.password);
  auto state = std::make_shared<PausedResponse>();
  auto paused = state->paused.get_promise();
  CurlHttpRequest request;
  request.method = "GET";
  request.url = webdav_resource_url(endpoint, "/hello.txt");
  request.receive = [state](std::span<const std::byte> bytes) {
    if (!state->resume) {
      state->paused.try_resolve();
      return std::size_t{CURL_WRITEFUNC_PAUSE};
    }
    state->body.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    return bytes.size();
  };
  auto receiving = perform_paused_request_async(session, std::move(request), state, {});
  std::exception_ptr failure;
  try {
    co_await paused;
    if (state->failure) std::rethrow_exception(state->failure);
    expect(!state->finished, "The HTTP request must pause before it completes");
    CurlHttpRequest probe;
    probe.method = "GET";
    probe.url = webdav_resource_url(endpoint, "/idle");
    bool timed_out = false;
    try { (void)co_await perform_http_request_async(probe_session, std::move(probe), {}); }
    catch (const std::runtime_error &error) { timed_out = std::string(error.what()).find("stopped making progress") != std::string::npos; }
    expect(timed_out, "An independent idle request must expire before the paused receiver resumes");
    state->resume = true;
    resume_curl_http_session(session);
  } catch (...) { failure = std::current_exception(); }
  if (failure) co_await stop_curl_http_session_async(session);
  co_await receiving;
  co_await stop_curl_http_session_async(session);
  co_await stop_curl_http_session_async(probe_session);
  if (failure) std::rethrow_exception(failure);
  if (state->failure) std::rethrow_exception(state->failure);
  expect(state->result.code == CURLE_OK && state->result.status == 200 && state->body == "Hello DAV!\r\n",
         "Intentional receive pause must neither count as idle time nor duplicate the replayed chunk");
}

static cardio::promise<void> check_cancelled_pause_async(
    elder_terms::WebdavClientOpenOptions options, bool upload) {
  using namespace elder_terms;
  const auto endpoint = webdav_endpoint(options.connection);
  auto session = create_curl_http_session(options.connection, options.password);
  if (upload) {
    CurlHttpRequest initial;
    initial.method = "GET";
    initial.url = webdav_resource_url(endpoint, "/hello.txt");
    const auto result = co_await perform_http_request_async(session, std::move(initial), {});
    expect(result.code == CURLE_OK && result.status == 200,
           "A paused upload must start with an authenticated session");
  }
  auto state = std::make_shared<PausedResponse>();
  auto paused = state->paused.get_promise();
  cardio::cancellation_source cancellation;
  CurlHttpRequest request;
  request.method = upload ? "PUT" : "GET";
  request.url = webdav_resource_url(endpoint, upload ? "/cancelled-pause.bin" : "/hello.txt");
  if (upload) {
    request.upload_size = 1;
    request.send = [state](std::span<std::byte>) {
      ++state->callbacks;
      state->paused.try_resolve();
      return std::size_t{CURL_READFUNC_PAUSE};
    };
  } else {
    request.receive = [state](std::span<const std::byte>) {
      ++state->callbacks;
      state->paused.try_resolve();
      return std::size_t{CURL_WRITEFUNC_PAUSE};
    };
  }
  auto transferring = perform_paused_request_async(session, std::move(request), state, cancellation.get_cancellation());
  co_await paused;
  const bool was_paused = !state->finished;
  (void)cancellation.cancel();
  co_await transferring;
  bool canceled = false;
  try { if (state->failure) std::rethrow_exception(state->failure); }
  catch (const cardio::canceled_exception &) { canceled = true; }
  std::exception_ptr failure;
  try {
    expect(was_paused && canceled, "Cancellation must retire a paused curl operation without requiring resume");
    expect(state->callbacks == 1, "Cancellation must not replay a paused user callback");
    CurlHttpRequest next;
    next.method = "GET";
    next.url = webdav_resource_url(endpoint, "/hello.txt");
    const auto result = co_await perform_http_request_async(session, std::move(next), {});
    if (!(result.code == CURLE_OK && result.status == 200 && result.body == "Hello DAV!\r\n"))
      std::cerr << "Paused " << (upload ? "upload" : "download") << " reuse: curl="
                << static_cast<int>(result.code) << ", status=" << result.status
                << ", bytes=" << result.body.size() << ", error=" << result.error << '\n';
    expect(result.code == CURLE_OK && result.status == 200 && result.body == "Hello DAV!\r\n",
           "A session must be reusable immediately after paused-transfer cancellation");
  } catch (...) { failure = std::current_exception(); }
  co_await stop_curl_http_session_async(session);
  if (failure) std::rethrow_exception(failure);
}

static cardio::promise<void> check_rejected_pause_async(elder_terms::WebdavClientOpenOptions options) {
  using namespace elder_terms;
  const auto endpoint = webdav_endpoint(options.connection);
  auto session = create_curl_http_session(options.connection, options.password);
  auto probe = create_curl_http_session(options.connection, options.password);
  auto state = std::make_shared<PausedResponse>();
  auto paused = state->paused.get_promise();
  CurlHttpRequest request;
  request.method = "PUT";
  request.url = webdav_resource_url(endpoint, "/reject-paused-upload.bin");
  request.upload_size = 1;
  // Receive 100 Continue before pausing, so the server already has the headers.
  request.headers.emplace_back("Expect: 100-continue");
  request.send = [state](std::span<std::byte>) {
    ++state->callbacks;
    state->paused.try_resolve();
    return std::size_t{CURL_READFUNC_PAUSE};
  };
  auto transferring = perform_paused_request_async(session, std::move(request), state, {});
  std::exception_ptr failure;
  try {
    co_await paused;
    if (state->failure) std::rethrow_exception(state->failure);
    expect(!state->finished, "The upload must pause before its rejection is released");
    CurlHttpRequest release;
    release.method = "GET";
    release.url = webdav_resource_url(endpoint, "/release-paused-rejection");
    const auto released = co_await perform_http_request_async(probe, std::move(release), {});
    expect(released.code == CURLE_OK && released.status == 204, "The server must release the early rejection");
    co_await transferring;
    if (state->failure) std::rethrow_exception(state->failure);
    expect(state->result.code == CURLE_OK && state->result.status == 403 && state->callbacks == 1,
           "An early rejection must finish a paused upload without requesting more body bytes");
    CurlHttpRequest next;
    next.method = "GET";
    next.url = webdav_resource_url(endpoint, "/hello.txt");
    const auto result = co_await perform_http_request_async(session, std::move(next), {});
    expect(result.code == CURLE_OK && result.status == 200 && result.body == "Hello DAV!\r\n",
           "A session must be reusable after the server rejects a paused upload");
  } catch (...) { failure = std::current_exception(); }
  co_await stop_curl_http_session_async(session);
  co_await transferring;
  co_await stop_curl_http_session_async(probe);
  if (failure) std::rethrow_exception(failure);
}

struct DownloadDirectory {
  std::filesystem::path path;
  ~DownloadDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

static std::string read_local_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  expect(input.good(), "Downloaded file must exist");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

static cardio::promise<elder_terms::FileTransferConflictAction> overwrite_conflict(
    const elder_terms::FileTransferConflict &, cardio::cancellation) {
  co_return elder_terms::FileTransferConflictAction::overwrite;
}

static cardio::promise<void> check_downloads_async(std::shared_ptr<elder_terms::RemoteFileClient> client) {
  using namespace elder_terms;
  for (const auto &[path, expected] : std::vector<std::pair<std::string, std::string>>{
      {"/hello.txt", "Hello DAV!\r\n"}, {"/get-redirect", "Hello DAV!\r\n"}, {"/資料 #+%.txt", ""}, {"/unknown.txt", "unknown length\n"}}) {
    auto opening = client->open_read_async(path, {});
    auto reader = std::move(co_await opening);
    std::string actual;
    std::array<std::byte, 8192> buffer{};
    for (;;) {
      const auto count = co_await reader->read_async(buffer, {});
      if (!count) break;
      actual.append(reinterpret_cast<const char *>(buffer.data()), count);
    }
    co_await reader->close_async({});
    expect(actual == expected, "GET must preserve empty, unknown-length and special-name file contents");
  }
  {
    auto opening = client->open_read_async("/large.bin", {});
    auto reader = std::move(co_await opening);
    std::array<std::byte, 64 * 1024> buffer{};
    std::uint64_t offset = 0;
    for (;;) {
      const auto count = co_await reader->read_async(buffer, {});
      if (!count) break;
      for (std::size_t index = 0; index < count; ++index)
        expect(std::to_integer<unsigned>(buffer[index]) == ((offset + index) * 31 + 7) % 256,
               "Streaming GET must preserve every byte across buffer boundaries");
      offset += count;
    }
    co_await reader->close_async({});
    expect(offset == 40 * 1024 * 1024 + 13, "GET must stream files larger than the XML response limit");
  }
  {
    cardio::cancellation_source cancellation;
    auto opening = client->open_read_async("/held.bin", cancellation.get_cancellation());
    auto reader = std::move(co_await opening);
    std::array<std::byte, 8192> buffer{};
    expect(co_await reader->read_async(buffer, {}) > 0, "Cancellation fixture must start sending before cancellation");
    (void)cancellation.cancel();
    bool canceled = false;
    try { (void)co_await reader->read_async(buffer, cancellation.get_cancellation()); }
    catch (const cardio::canceled_exception &) { canceled = true; }
    co_await reader->close_async({});
    expect(canceled, "GET cancellation must propagate and release an unfinished response");
    expect((co_await client->load_directory_async("/", {})).entries.size() == 4,
           "The session must be usable after a cancelled download");
  }
  for (const auto &path : {"/get-outside", "/partial.bin", "/missing"}) {
    std::unique_ptr<RemoteFileReader> reader;
    bool failed = false;
    try {
      auto opening = client->open_read_async(path, {});
      reader = std::move(co_await opening);
      std::array<std::byte, 8192> buffer{};
      while (co_await reader->read_async(buffer, {})) {}
    } catch (const std::exception &) { failed = true; }
    if (reader) co_await reader->close_async({});
    expect(failed, "GET must reject escaping redirects, unsolicited partial responses and absent resources");
  }
  for (const bool abandon : {false, true}) {
    auto opening = client->open_read_async("/held.bin", {});
    auto reader = std::move(co_await opening);
    std::array<std::byte, 8192> buffer{};
    expect(co_await reader->read_async(buffer, {}) > 0, "Early-close fixture must send data before the reader is released");
    if (!abandon) co_await reader->close_async({});
    reader.reset();
    expect((co_await client->load_directory_async("/", {})).entries.size() == 4,
           "Closing or destroying an unfinished reader must release the session operation slot");
  }
  {
    auto opening = client->open_read_async("/held.bin", {});
    auto reader = std::move(co_await opening);
    std::array<std::byte, 8192> buffer{};
    expect(co_await reader->read_async(buffer, {}) > 0, "Pending-read fixture must first deliver its initial chunk");
    auto reading = reader->read_async(buffer, {});
    reader.reset();
    bool canceled = false;
    try { (void)co_await reading; } catch (const cardio::canceled_exception &) { canceled = true; }
    expect(canceled && (co_await client->load_directory_async("/", {})).entries.size() == 4,
           "Destroying a reader during an outstanding read must cancel safely and release its session");
  }
  char pattern[] = "/tmp/elder-webdav-download-XXXXXX";
  const auto *created = ::mkdtemp(pattern);
  expect(created != nullptr, "Temporary download directory must be created");
  const DownloadDirectory directory{created};
  FileTransferProgress last_progress;
  FileTransferRequest request;
  request.direction = FileTransferDirection::receive;
  request.source_paths = {"/hello.txt", "/資料 #+%.txt", "/unknown.txt", "/nested"};
  request.destination_directory = directory.path.string();
  request.callbacks.progress = [&last_progress](const FileTransferProgress &progress) { last_progress = progress; };
  auto receiving = run_file_transfer_async(client, std::move(request), {});
  co_await receiving;
  expect(read_local_file(directory.path / "hello.txt") == "Hello DAV!\r\n" &&
         read_local_file(directory.path / "資料 #+%.txt").empty() &&
         read_local_file(directory.path / "unknown.txt") == "unknown length\n" &&
         read_local_file(directory.path / "nested/child.txt") == "child\n",
         "The shared engine must download selected files and a recursive collection");
  const std::optional<std::uint64_t> total = last_progress.total_bytes;
  expect(!total && last_progress.transferred_bytes == 33 &&
         last_progress.completed_items == last_progress.total_items,
         "Unknown source size must remain unknown in aggregate progress while actual bytes are counted");
  const auto destination = directory.path / "truncated.bin";
  { std::ofstream output(destination, std::ios::binary); output << "original content"; }
  FileTransferRequest failing;
  failing.direction = FileTransferDirection::receive;
  failing.source_paths = {"/truncated.bin"};
  failing.destination_directory = directory.path.string();
  failing.callbacks.conflict = overwrite_conflict;
  bool failed = false;
  try {
    auto pending = run_file_transfer_async(client, std::move(failing), {});
    co_await pending;
  } catch (const std::exception &) { failed = true; }
  expect(failed && read_local_file(destination) == "original content",
         "Truncated GET must fail without replacing the existing destination");
  FileTransferRequest identity_request;
  identity_request.direction = FileTransferDirection::receive;
  identity_request.source_paths = {"/identity.bin"};
  identity_request.destination_directory = directory.path.string();
  auto identity_pending = run_file_transfer_async(client, std::move(identity_request), {});
  co_await identity_pending;
  expect(read_local_file(directory.path / "identity.bin") == "identity transfer\n",
         "Identity encoding with HTTP optional whitespace preserves the file content");
  const auto encoded_destination = directory.path / "encoded.bin";
  { std::ofstream output(encoded_destination, std::ios::binary); output << "original encoded destination"; }
  FileTransferRequest encoded_request;
  encoded_request.direction = FileTransferDirection::receive;
  encoded_request.source_paths = {"/encoded.bin"};
  encoded_request.destination_directory = directory.path.string();
  encoded_request.callbacks.conflict = overwrite_conflict;
  bool encoded_failed = false;
  try {
    auto pending = run_file_transfer_async(client, std::move(encoded_request), {});
    co_await pending;
  } catch (const std::exception &) { encoded_failed = true; }
  expect(encoded_failed && read_local_file(encoded_destination) == "original encoded destination",
         "Unnegotiated HTTP content encoding must not be committed as corrupted file bytes");
  for (const auto &entry : std::filesystem::directory_iterator(directory.path))
    expect(entry.path().filename().string().find(".elder-terms-part-") == std::string::npos,
           "Failed downloads must remove their local temporary files");
}

static cardio::promise<std::string> read_remote_text_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client, std::string path) {
  auto reader = std::move(co_await client->open_read_async(std::move(path), {}));
  std::string result;
  std::array<std::byte, 4096> buffer{};
  for (;;) {
    const auto count = co_await reader->read_async(buffer, {});
    if (!count) break;
    result.append(reinterpret_cast<const char *>(buffer.data()), count);
    expect(result.size() <= 1024 * 1024, "Small fixture resource must have a bounded body");
  }
  co_await reader->close_async({});
  co_return result;
}

static cardio::promise<void> send_paths_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client,
    std::vector<std::string> paths, std::string destination) {
  elder_terms::FileTransferRequest request;
  request.direction = elder_terms::FileTransferDirection::send;
  request.source_paths = std::move(paths);
  request.destination_directory = std::move(destination);
  request.callbacks.conflict = overwrite_conflict;
  co_await elder_terms::run_file_transfer_async(client, std::move(request), {});
}

static cardio::promise<std::string> failed_send_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client,
    std::string path) {
  std::vector<std::string> paths{std::move(path)};
  try { co_await send_paths_async(client, std::move(paths), "/uploaded"); }
  catch (const std::exception &error) { co_return std::string(error.what()); }
  throw std::runtime_error("Faulted mutation must not be reported as a successful upload");
}

static cardio::promise<void> check_uploads_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client,
    elder_terms::WebdavAuthentication authentication) {
  using namespace elder_terms;
  char pattern[] = "/tmp/elder-webdav-upload-XXXXXX";
  const auto *created = ::mkdtemp(pattern);
  expect(created != nullptr, "Temporary upload directory must be created");
  const DownloadDirectory directory{created};
  const auto write = [&](const char *name, const std::string &content) {
    const auto path = directory.path / name;
    std::ofstream output(path, std::ios::binary);
    output << content;
    expect(output.good(), "Local upload source must be written");
    return path.string();
  };
  std::filesystem::create_directories(directory.path / "bundle/nested");
  const auto replacement = write("replace.txt", "original destination");
  write("bundle/資料 #+%.txt", "special name content");
  write("bundle/empty.txt", "");
  write("bundle/nested/child.txt", "recursive content");
  const auto large = directory.path / "large.bin";
  {
    std::array<char, 65536> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) bytes[index] = static_cast<char>((index * 31 + 7) & 255);
    std::ofstream output(large, std::ios::binary);
    std::uint64_t remaining = 40 * 1024 * 1024 + 13;
    while (remaining) {
      const auto count = std::min<std::uint64_t>(remaining, bytes.size());
      output.write(bytes.data(), static_cast<std::streamsize>(count));
      remaining -= count;
    }
    expect(output.good(), "Large source fixture must be written");
  }
  co_await client->make_directory_async("/uploaded", std::nullopt, {});
  std::vector<std::string> first{replacement};
  co_await send_paths_async(client, std::move(first), "/uploaded");
  write("replace.txt", "replacement destination");
  std::vector<std::string> sources{replacement, large.string(), (directory.path / "bundle").string()};
  co_await send_paths_async(client, std::move(sources), "/uploaded");
  expect(co_await read_remote_text_async(client, "/uploaded/replace.txt") == "replacement destination",
         "An approved replacement must commit the completed new content");
  expect(co_await read_remote_text_async(client, "/uploaded/bundle/資料 #+%.txt") == "special name content" &&
         (co_await read_remote_text_async(client, "/uploaded/bundle/empty.txt")).empty() &&
         co_await read_remote_text_async(client, "/uploaded/bundle/nested/child.txt") == "recursive content",
         "Recursive upload must preserve empty and special-name files");
  std::filesystem::create_directories(directory.path / "kind-conflicts");
  const auto file_over_collection = write("kind-conflicts/bundle", "cannot replace a collection");
  expect(!(co_await failed_send_async(client, file_over_collection)).empty() &&
         co_await read_remote_text_async(client, "/uploaded/bundle/nested/child.txt") == "recursive content",
         "File/collection conflicts must preserve the completed collection and its children");
  std::filesystem::create_directories(directory.path / "kind-conflicts/replace.txt");
  write("kind-conflicts/replace.txt/child.txt", "cannot replace a file");
  expect(!(co_await failed_send_async(client, (directory.path / "kind-conflicts/replace.txt").string())).empty() &&
         co_await read_remote_text_async(client, "/uploaded/replace.txt") == "replacement destination",
         "Collection/file conflicts must preserve the completed destination file");
  co_await rename_file_transfer_item_async(client, FileTransferEndpoint::remote,
                                         "/uploaded/large.bin", "/uploaded/renamed.bin", {});
  expect(!(co_await client->lstat_async("/uploaded/large.bin", {})), "MOVE must remove the original name");
  {
    auto reader = std::move(co_await client->open_read_async("/uploaded/renamed.bin", {}));
    std::array<std::byte, 65536> buffer{};
    std::uint64_t offset = 0;
    for (;;) {
      const auto count = co_await reader->read_async(buffer, {});
      if (!count) break;
      for (std::size_t index = 0; index < count; ++index)
        expect(std::to_integer<unsigned>(buffer[index]) == ((offset + index) * 31 + 7) % 256,
               "Large upload and rename must preserve every byte");
      offset += count;
    }
    co_await reader->close_async({});
    expect(offset == 40 * 1024 * 1024 + 13, "PUT must not truncate a large upload");
  }
  bool conflict = false;
  try { co_await client->rename_async("/uploaded/replace.txt", "/uploaded/renamed.bin", {}); }
  catch (const std::exception &) { conflict = true; }
  expect(conflict && co_await read_remote_text_async(client, "/uploaded/replace.txt") == "replacement destination",
         "Ordinary MOVE must refuse an existing destination without removing its source");
  std::vector<std::string> removals{"/uploaded/renamed.bin", "/uploaded/bundle"};
  co_await delete_file_transfer_items_async(client, FileTransferEndpoint::remote, std::move(removals), {});
  expect(!(co_await client->lstat_async("/uploaded/bundle", {})) &&
         !(co_await client->lstat_async("/uploaded/renamed.bin", {})),
         "Shared deletion must remove selected files and explicit collection trees");

  for (const auto *name : {"put-collision.txt", "put-drop.txt", "move-drop.txt", "cleanup-denied.txt", "redirect-write.txt"}) {
    const auto path = write(name, "faulted upload content");
    const auto message = co_await failed_send_async(client, path);
    expect(!message.empty(), "Mutation failure must have a diagnostic");
    const auto snapshot = co_await client->load_directory_async("/uploaded", {});
    const auto temporary = std::find_if(snapshot.entries.begin(), snapshot.entries.end(), [name](const auto &entry) {
      return entry.name.starts_with(std::string(name) + ".elder-terms-part-");
    });
    if (std::string_view(name) == "put-collision.txt") {
      expect(temporary != snapshot.entries.end() &&
             co_await read_remote_text_async(client, temporary->path) == "foreign temporary file",
             "Conditional PUT collision must neither overwrite nor delete a foreign file");
    } else if (std::string_view(name) == "put-drop.txt" || std::string_view(name) == "cleanup-denied.txt") {
      expect(temporary != snapshot.entries.end() && message.find(temporary->path) != std::string::npos,
             "Uncertain or refused temporary cleanup must report its remaining path");
    } else if (std::string_view(name) == "move-drop.txt") {
      expect(co_await read_remote_text_async(client, "/uploaded/move-drop.txt") == "faulted upload content",
             "A lost MOVE response must report failure even when the server committed it");
    }
  }
  if (authentication == WebdavAuthentication::digest) {
    for (const auto &[name, size] : {std::pair{"rewind.txt", 512 * 1024}, std::pair{"rewind-small.txt", 17}}) {
      const auto path = write(name, std::string(size, 'r'));
      const auto message = co_await failed_send_async(client, path);
      expect(message.find("consumed") != std::string::npos &&
             !(co_await client->lstat_async(std::string("/uploaded/") + name, {})),
             "Digest renewal after consuming either a small or large PUT must fail without committing a replay");
    }
  }
  const auto dropped = write("delete-drop.txt", "delete before losing response");
  std::vector<std::string> drop_sources{dropped};
  co_await send_paths_async(client, std::move(drop_sources), "/uploaded");
  bool delete_failed = false;
  try { co_await client->remove_file_async("/uploaded/delete-drop.txt", {}); }
  catch (const std::exception &) { delete_failed = true; }
  expect(delete_failed && !(co_await client->lstat_async("/uploaded/delete-drop.txt", {})),
         "A lost DELETE response must not turn into automatic retry or success");
  co_await client->make_directory_async("/uploaded/partial", std::nullopt, {});
  const auto blocked = write("blocked.txt", "retained child");
  std::vector<std::string> blocked_sources{blocked};
  co_await send_paths_async(client, std::move(blocked_sources), "/uploaded/partial");
  bool partial_failed = false;
  std::vector<std::string> partial_paths{"/uploaded/partial"};
  try { co_await delete_file_transfer_items_async(client, FileTransferEndpoint::remote, std::move(partial_paths), {}); }
  catch (const std::exception &) { partial_failed = true; }
  expect(partial_failed && co_await read_remote_text_async(client, "/uploaded/partial/blocked.txt") == "retained child",
         "A 207 response with a failed child must not report successful tree deletion");
  std::vector<std::string> final_paths{"/uploaded"};
  co_await delete_file_transfer_items_async(client, FileTransferEndpoint::remote, std::move(final_paths), {});
  expect(!(co_await client->lstat_async("/uploaded", {})), "Explicit final tree removal must restore the fixture root");
}

static cardio::promise<void> check_cancelled_upload_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client) {
  using namespace elder_terms;
  char pattern[] = "/tmp/elder-webdav-cancel-upload-XXXXXX";
  const auto *created = ::mkdtemp(pattern);
  expect(created != nullptr, "Temporary upload directory must be created");
  const DownloadDirectory directory{created};
  const auto source = directory.path / "cancel-upload.txt";
  { std::ofstream output(source, std::ios::binary); output << std::string(131072, 'c'); }
  co_await client->make_directory_async("/uploaded", std::nullopt, {});
  cardio::cancellation_source cancellation;
  FileTransferRequest request;
  request.direction = FileTransferDirection::send;
  request.source_paths = {source.string()};
  request.destination_directory = "/uploaded";
  auto sending = run_file_transfer_async(client, std::move(request), cancellation.get_cancellation());
  co_await cardio::from_fd(STDIN_FILENO, cardio::fd_event::read);
  char command = 0;
  expect(::read(STDIN_FILENO, &command, 1) == 1 && command == 'c',
         "Parent must observe the completed PUT body before cancellation");
  (void)cancellation.cancel();
  std::string message;
  try { co_await sending; }
  catch (const std::exception &error) { message = error.what(); }
  const auto snapshot = co_await client->load_directory_async("/uploaded", {});
  const auto temporary = std::find_if(snapshot.entries.begin(), snapshot.entries.end(), [](const auto &entry) {
    return entry.name.starts_with("cancel-upload.txt.elder-terms-part-");
  });
  expect(temporary != snapshot.entries.end() && message.find(temporary->path) != std::string::npos,
         "Cancellation before the final PUT response must report the uncertain temporary path");
  expect(!(co_await client->lstat_async("/uploaded/cancel-upload.txt", {})),
         "Cancellation while waiting for the final PUT response must not commit the upload");
  std::vector<std::string> cleanup{"/uploaded"};
  co_await delete_file_transfer_items_async(client, FileTransferEndpoint::remote, std::move(cleanup), {});
}

static cardio::promise<void> check_bounded_upload_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client, bool abandon) {
  auto writer = std::move(co_await client->open_write_async("/body-held.bin", 64 * 1024 * 1024, std::nullopt, {}));
  const std::string payload(64 * 1024 * 1024, 'b');
  cardio::cancellation_source cancellation;
  auto writing = writer->write_all_async(
      {reinterpret_cast<const std::byte *>(payload.data()), payload.size()},
      cancellation.get_cancellation());
  auto queued = client->lstat_async("/hello.txt", {});
  expect(!writing.is_ready() && !queued.is_ready(),
         "A paused server must bound upload buffering and retain the request slot");
  co_await cardio::from_fd(STDIN_FILENO, cardio::fd_event::read);
  char command = 0;
  expect(::read(STDIN_FILENO, &command, 1) == 1 && command == 'c',
         "Parent must observe the paused request body before cancellation");
  if (abandon) writer.reset();
  else (void)cancellation.cancel();
  bool cancelled = false;
  try { co_await writing; }
  catch (const cardio::canceled_exception &) { cancelled = true; }
  expect(cancelled, "A blocked upload must propagate cancellation");
  if (writer) {
    try { co_await writer->close_async({}); } catch (const std::exception &) {}
  }
  writer.reset();
  const auto attributes = co_await queued;
  expect(attributes && attributes->size == 12,
         "Cancellation must retire the upload and release queued metadata on the same session");
  expect(co_await read_remote_text_async(client, "/hello.txt") == "Hello DAV!\r\n",
         "A cancelled bounded upload must leave the session usable");
}

static cardio::promise<void> check_async(elder_terms::WebdavClientOpenOptions options,
    bool expect_failure, std::string mode, cardio::dispatcher_group_glib &group, std::exception_ptr &failure) {
  std::shared_ptr<elder_terms::RemoteFileClient> client;
  try {
    if (options.connection.remote_directory == "/hold") {
      cardio::cancellation_source source;
      auto opening = elder_terms::open_webdav_client_async(std::move(options), source.get_cancellation());
      co_await cardio::from_fd(STDIN_FILENO, cardio::fd_event::read);
      char command = 0;
      expect(::read(STDIN_FILENO, &command, 1) == 1 && command == 'c', "Parent must observe the pending request before cancellation");
      (void)source.cancel();
      bool canceled = false;
      try { client = co_await opening; } catch (const cardio::canceled_exception &) { canceled = true; }
      expect(canceled, "Pending HTTP work must cancel without waiting for a server response");
      group.shutdown();
      co_return;
    }
    const auto transport_options = options;
    std::exception_ptr opening_failure;
    try { client = co_await elder_terms::open_webdav_client_async(std::move(options), {}); }
    catch (...) { opening_failure = std::current_exception(); }
    if (expect_failure) {
      expect(opening_failure != nullptr, "Invalid credentials or certificates must fail");
      if (transport_options.connection.remote_directory == "/unsupported-auth") {
        std::string message;
        try { std::rethrow_exception(opening_failure); }
        catch (const std::exception &error) { message = error.what(); }
        expect(message.find("Unsupported authentication") != std::string::npos,
               "An unsupported-only challenge must identify the mechanism, not imply an incorrect password");
      }
    } else if (mode == "reject-paused-upload") {
      if (opening_failure) std::rethrow_exception(opening_failure);
      co_await check_rejected_pause_async(transport_options);
    } else if (mode == "bounded-upload" || mode == "abandon-upload") {
      if (opening_failure) std::rethrow_exception(opening_failure);
      co_await check_bounded_upload_async(client, mode == "abandon-upload");
    } else if (mode == "cancel-upload") {
      if (opening_failure) std::rethrow_exception(opening_failure);
      co_await check_cancelled_upload_async(client);
    } else {
      if (opening_failure) std::rethrow_exception(opening_failure);
      const auto snapshot = co_await client->load_directory_async("/", {});
      expect(snapshot.canonical_path == "/" && snapshot.entries.size() == 4, "DAV listing must use the configured virtual root");
      const auto unknown = std::find_if(snapshot.entries.begin(), snapshot.entries.end(), [](const auto &item) { return item.name == "unknown.txt"; });
      expect(unknown != snapshot.entries.end() && !unknown->size, "Missing content length must remain unknown");
      const auto nested = co_await client->load_directory_async("/nested", {});
      expect(nested.canonical_path == "/nested" && nested.entries.size() == 1 && nested.entries.front().path == "/nested/child.txt",
             "Nested browsing must preserve the virtual root and child paths");
      const auto info = co_await client->lstat_async("/", {});
      expect(info && info->type == elder_terms::RemoteFileType::directory, "Depth zero must inspect a collection");
      const auto missing = co_await client->lstat_async("/missing", {});
      expect(!missing, "HTTP 404 must report absence");
      expect(client->try_begin_transfer() && !client->try_begin_transfer(), "Transfer slot must be exclusive");
      client->end_transfer();
      expect(client->try_begin_transfer(), "Transfer slot must be reusable");
      client->end_transfer();
      co_await check_downloads_async(client);
      co_await check_paused_http_async(transport_options);
      co_await check_cancelled_pause_async(transport_options, false);
      co_await check_cancelled_pause_async(transport_options, true);
      co_await check_uploads_async(client, transport_options.connection.authentication);
      const auto capabilities = client->capabilities();
      expect(!capabilities.symbolic_links && !capabilities.permissions && !capabilities.access_time && !capabilities.modification_time,
             "DAV must not advertise POSIX operations");
    }
  } catch (...) { failure = std::current_exception(); }
  try { if (client) co_await elder_terms::stop_webdav_client_async(client); }
  catch (...) { failure = std::current_exception(); }
  group.shutdown();
}

int main(int argc, char **argv) {
  try {
    expect(argc == 7 || argc == 8 || argc == 9, "Expected scheme, port, auth, password, CA path, expected result and optional initial directory");
    elder_terms::WebdavClientOpenOptions options;
    options.connection.scheme = argv[1];
    options.connection.address = "127.0.0.1";
    options.connection.port = std::stoll(argv[2]);
    options.connection.base_path = "/dav/";
    if (argc >= 8) options.connection.remote_directory = argv[7];
    const std::string auth(argv[3]);
    options.connection.authentication = auth == "none" ? elder_terms::WebdavAuthentication::none
        : auth == "basic" ? elder_terms::WebdavAuthentication::basic
        : auth == "digest" ? elder_terms::WebdavAuthentication::digest : elder_terms::WebdavAuthentication::automatic;
    options.connection.username = "alice";
    options.password = argv[4];
    options.connection.ca_file = argv[5];
    std::exception_ptr failure;
    {
      cardio::dispatcher_group_glib group;
      cardio::dispatcher_host_glib_auto dispatcher(group);
      auto task = check_async(std::move(options), std::string(argv[6]) == "failure", argc == 9 ? argv[8] : "", group, failure);
      dispatcher.park();
    }
    if (failure) std::rethrow_exception(failure);
    std::cout << "webdav-client-test: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

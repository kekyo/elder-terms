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
  elder_terms::CurlHttpResult result;
  std::exception_ptr failure;
};


static cardio::promise<void> receive_paused_response_async(
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
  auto receiving = receive_paused_response_async(session, std::move(request), state, {});
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

static cardio::promise<void> check_cancelled_pause_async(elder_terms::WebdavClientOpenOptions options) {
  using namespace elder_terms;
  const auto endpoint = webdav_endpoint(options.connection);
  auto session = create_curl_http_session(options.connection, options.password);
  auto state = std::make_shared<PausedResponse>();
  auto paused = state->paused.get_promise();
  cardio::cancellation_source cancellation;
  CurlHttpRequest request;
  request.method = "GET";
  request.url = webdav_resource_url(endpoint, "/hello.txt");
  request.receive = [state](std::span<const std::byte>) {
    state->paused.try_resolve();
    return std::size_t{CURL_WRITEFUNC_PAUSE};
  };
  auto receiving = receive_paused_response_async(session, std::move(request), state, cancellation.get_cancellation());
  co_await paused;
  const bool was_paused = !state->finished;
  (void)cancellation.cancel();
  co_await receiving;
  bool canceled = false;
  try { if (state->failure) std::rethrow_exception(state->failure); }
  catch (const cardio::canceled_exception &) { canceled = true; }
  std::exception_ptr failure;
  try {
    expect(was_paused && canceled, "Cancellation must retire a paused curl response without requiring resume");
    CurlHttpRequest next;
    next.method = "GET";
    next.url = webdav_resource_url(endpoint, "/hello.txt");
    const auto result = co_await perform_http_request_async(session, std::move(next), {});
    expect(result.code == CURLE_OK && result.status == 200 && result.body == "Hello DAV!\r\n",
           "A session must be reusable immediately after paused-response cancellation");
  } catch (...) { failure = std::current_exception(); }
  co_await stop_curl_http_session_async(session);
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
  for (const auto &entry : std::filesystem::directory_iterator(directory.path))
    expect(entry.path().filename().string().find(".elder-terms-part-") == std::string::npos,
           "Failed downloads must remove their local temporary files");
}

static cardio::promise<void> check_async(elder_terms::WebdavClientOpenOptions options,
    bool expect_failure, cardio::dispatcher_group_glib &group, std::exception_ptr &failure) {
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
      co_await check_cancelled_pause_async(transport_options);
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
    expect(argc == 7 || argc == 8, "Expected scheme, port, auth, password, CA path, expected result and optional initial directory");
    elder_terms::WebdavClientOpenOptions options;
    options.connection.scheme = argv[1];
    options.connection.address = "127.0.0.1";
    options.connection.port = std::stoll(argv[2]);
    options.connection.base_path = "/dav/";
    if (argc == 8) options.connection.remote_directory = argv[7];
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
      auto task = check_async(std::move(options), std::string(argv[6]) == "failure", group, failure);
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

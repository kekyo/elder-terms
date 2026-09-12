#include "ftp-client.h"
#include "../file-transfer/file-transfer-engine.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace elder_terms_curl_ftp_stream_test {

static elder_terms::FtpTlsMode tls_mode = elder_terms::FtpTlsMode::none;
static std::string tls_name;
static std::string tls_certificate;
static std::string tls_key;

static void expect(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

static std::string read_line(int fd) {
  std::string text;
  char ch;
  for (;;) {
    const auto count = ::read(fd, &ch, 1);
    if (count < 0 && errno == EINTR) continue;
    expect(count > 0, "FTP server closed its event stream");
    if (ch == '\n') return text;
    text += ch;
  }
}

struct Server {
  std::filesystem::path directory;
  pid_t pid = -1;
  int events = -1;
  int commands = -1;
  unsigned port = 0;

  Server(const std::string &executable, const std::vector<std::string> &options) {
    std::array<char, 64> path{};
    std::string pattern = "/tmp/elder-terms-curl-client-XXXXXX";
    std::copy(pattern.begin(), pattern.end(), path.begin());
    expect(::mkdtemp(path.data()) != nullptr, "Test directory creation failed");
    directory = path.data();
    std::filesystem::create_directories(directory / "home/directory");
    std::filesystem::create_directories(directory / "home/space % # \" 日本;type=a");
    std::ofstream(directory / "home/data.txt") << "content";
    std::ofstream(directory / "home/denied") << "protected";
    std::ofstream(directory / "home/large.bin") << std::string(17, 'x');
    std::string payload(8 * 1024 * 1024, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(i % 251);
    std::ofstream(directory / "home/big.bin", std::ios::binary) << payload;
    std::ofstream(directory / "home/space % # \" 日本;type=a/file % # \" 日本;type=i") << "special";
    int output[2];
    int input[2];
    expect(::pipe2(output, O_CLOEXEC) == 0 && ::pipe2(input, O_CLOEXEC) == 0,
           "Test server pipes failed");
    std::vector<std::string> arguments{executable, directory.string(), "--trace"};
    arguments.insert(arguments.end(), options.begin(), options.end());
    if (!tls_name.empty()) {
      arguments.push_back("--tls=" + tls_name);
      arguments.push_back("--cert=" + tls_certificate);
      arguments.push_back("--key=" + tls_key);
    }
    std::vector<char *> pointers;
    for (auto &argument : arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    pid = ::fork();
    expect(pid >= 0, "Test server fork failed");
    if (pid == 0) {
      ::dup2(input[0], STDIN_FILENO);
      ::dup2(output[1], STDOUT_FILENO);
      ::execv(executable.c_str(), pointers.data());
      _exit(127);
    }
    ::close(input[0]);
    ::close(output[1]);
    events = output[0];
    commands = input[1];
    const auto ready = read_line(events);
    expect(ready.starts_with("READY "), "Test server did not announce readiness");
    port = std::stoul(ready.substr(6));
  }

  ~Server() {
    if (commands >= 0) ::close(commands);
    if (events >= 0) ::close(events);
    if (pid > 0) {
      ::kill(pid, SIGTERM);
      while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
};

static cardio::promise<void> wait_event_async(Server &server, const char *name) {
  co_await cardio::from_fd(server.events, cardio::fd_event::read);
  expect(read_line(server.events) == name, "Unexpected FTP synchronization event");
}

static void release(Server &server, bool cancel) {
  const std::string action = cancel ? "cancel\n" : "release\n";
  expect(::write(server.commands, action.data(), action.size()) ==
             static_cast<ssize_t>(action.size()), "FTP synchronization write failed");
}

static cardio::promise<std::string> read_all_async(
    elder_terms::RemoteFileReader &reader, cardio::cancellation cancellation) {
  std::array<std::byte, 4093> chunk;
  std::string result;
  for (;;) {
    const auto count = co_await reader.read_async(chunk, cancellation);
    if (count == 0) co_return result;
    result.append(reinterpret_cast<const char *>(chunk.data()), count);
  }
}

static cardio::promise<void> write_text_async(
    elder_terms::RemoteFileWriter &writer, const std::string &text,
    cardio::cancellation cancellation) {
  co_await writer.write_all_async(
      {reinterpret_cast<const std::byte *>(text.data()), text.size()}, cancellation);
}

static void expect_file(const std::filesystem::path &path, const std::string &expected) {
  std::ifstream file(path, std::ios::binary);
  expect(bool(file), "Expected output file is missing");
  std::array<char, 16384> buffer;
  std::size_t offset = 0;
  while (file) {
    file.read(buffer.data(), buffer.size());
    const auto count = static_cast<std::size_t>(file.gcount());
    expect(count <= expected.size() - offset &&
               std::string_view(buffer.data(), count) == std::string_view(expected).substr(offset, count),
           "File content differs from transferred bytes");
    offset += count;
  }
  expect(offset == expected.size(), "Transferred file has the wrong size");
}

static cardio::promise<void> direct_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client, Server &server) {
  std::string payload(2 * 1024 * 1024 + 17, '\0');
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<char>(index % 251);
  }
  const std::array<std::pair<std::string, std::string>, 3> files{{
      {"/home/output.bin", payload}, {"/home/empty.bin", {}},
      {"/home/space % # \" 日本;type=a/new % # \" 日本;type=i", std::string("special\r\ncontent\0tail", 21)}}};
  for (const auto &[path, content] : files) {
    auto writer = std::move(co_await client->open_write_async(path, content.size(), std::nullopt, {}));
    for (std::size_t offset = 0; offset < content.size(); offset += 17003) {
      const auto count = std::min<std::size_t>(17003, content.size() - offset);
      co_await writer->write_all_async(
          {reinterpret_cast<const std::byte *>(content.data() + offset), count}, {});
    }
    co_await writer->close_async({});
    co_await writer->close_async({});
    expect_file(server.directory / path.substr(1), content);
    auto reader = std::move(co_await client->open_read_async(path, {}));
    expect(co_await read_all_async(*reader, {}) == content,
           "Binary RETR must preserve every byte across backpressure and resumes");
    co_await reader->close_async({});
    co_await reader->close_async({});
  }
}

static cardio::promise<void> bounded_upload_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client, Server &server,
    bool cancel) {
  auto writer = std::move(co_await client->open_write_async("/home/bounded.bin", 64 * 1024 * 1024, std::nullopt, {}));
  co_await wait_event_async(server, "DATA_WAIT");
  const std::string payload(64 * 1024 * 1024, 'x');
  cardio::cancellation_source cancellation;
  auto writing = write_text_async(*writer, payload, cancellation.get_cancellation());
  auto queued = client->lstat_async("/home/bounded.bin", {});
  expect(!writing.is_ready() && !queued.is_ready(),
         "A stalled receiver must apply backpressure and retain the operation slot");
  if (cancel) (void)cancellation.cancel();
  release(server, cancel);
  bool canceled = false;
  try {
    co_await writing;
  } catch (const cardio::canceled_exception &) {
    canceled = true;
  }
  expect(canceled == cancel, "A blocked upload must propagate cancellation");
  if (cancel) {
    bool failed = false;
    try { (void)co_await queued; } catch (const std::runtime_error &) { failed = true; }
    expect(failed, "Canceled transfers must settle queued operations as failures");
  } else {
    co_await writer->close_async({});
    const auto attributes = co_await queued;
    expect(attributes && attributes->size == payload.size(),
           "Queued metadata must observe the complete upload");
    expect_file(server.directory / "home/bounded.bin", payload);
  }
}

static cardio::promise<void> held_final_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client, Server &server,
    bool upload, bool cancel) {
  auto writer = std::unique_ptr<elder_terms::RemoteFileWriter>{};
  auto reader = std::unique_ptr<elder_terms::RemoteFileReader>{};
  cardio::cancellation_source cancellation;
  cardio::promise<void> closing;
  cardio::promise<std::string> reading;
  if (upload) {
    writer = std::move(co_await client->open_write_async("/home/held.bin", 7, std::nullopt, {}));
    const std::string payload = "content";
    co_await write_text_async(*writer, payload, {});
    closing = writer->close_async(cancellation.get_cancellation());
  } else {
    reader = std::move(co_await client->open_read_async("/home/data.txt", {}));
    reading = read_all_async(*reader, cancellation.get_cancellation());
  }
  co_await wait_event_async(server, "FINAL_WAIT");
  auto queued = client->make_directory_async("/home/after-final", std::nullopt, {});
  expect(!(upload ? closing.is_ready() : reading.is_ready()) && !queued.is_ready(),
         "Transfer completion and following operations must await the final FTP response");
  if (cancel) (void)cancellation.cancel();
  release(server, cancel);
  bool canceled = false;
  try {
    if (upload) co_await closing;
    else expect(co_await reading == "content", "Delayed RETR returned wrong content");
  } catch (const cardio::canceled_exception &) {
    canceled = true;
  }
  expect(canceled == cancel, "Final-response cancellation was lost");
  bool failed = false;
  try { co_await queued; } catch (const std::runtime_error &) { failed = true; }
  expect(failed == cancel, "A final-response failure must settle following operations");
  expect(std::filesystem::exists(server.directory / "home/after-final") == !cancel,
         "A queued mutation ran despite transfer cancellation");
}

static cardio::promise<void> failed_transfer_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client, Server &server,
    const std::string &name) {
  if (name == "store-refused") {
    bool failed = false;
    try {
      auto writer = std::move(co_await client->open_write_async("/home/refused.bin", 0, std::nullopt, {}));
    } catch (const std::runtime_error &error) {
      failed = std::string_view(error.what()).find("550") != std::string_view::npos;
    }
    expect(failed && !std::filesystem::exists(server.directory / "home/refused.bin"),
           "STOR 550 must fail open without creating a file");
    co_return;
  }
  if (name.starts_with("upload")) {
    auto writer = std::move(co_await client->open_write_async("/home/failure.bin", 14, std::nullopt, {}));
    const std::string content = "complete input";
    co_await write_text_async(*writer, content, {});
    bool failed = false;
    try { co_await writer->close_async({}); } catch (const std::runtime_error &) { failed = true; }
    expect(failed, "STOR close must reject a failed or missing final response");
  } else {
    const bool large_size = name == "large-size";
    auto reader = std::move(co_await client->open_read_async(
        large_size ? "/home/large.bin" : "/home/big.bin", {}));
    auto reading = read_all_async(*reader, {});
    if (large_size) {
      co_await wait_event_async(server, "FINAL_WAIT");
      release(server, false);
    }
    bool failed = false;
    try { (void)co_await reading; } catch (const std::runtime_error &) { failed = true; }
    expect(failed, "RETR EOF must reject truncated data or a failed final response");
  }
}

static cardio::promise<void> abandoned_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client, bool stop) {
  auto reader = std::move(co_await client->open_read_async("/home/big.bin", {}));
  std::array<std::byte, 17> prefix;
  const auto count = co_await reader->read_async(prefix, {});
  expect(count > 0, "RETR must begin before abandonment");
  for (std::size_t index = 0; index < count; ++index) {
    expect(prefix[index] == static_cast<std::byte>(index % 251), "RETR prefix differs from the server file");
  }
  auto queued = client->lstat_async("/home/data.txt", {});
  if (stop) co_await elder_terms::stop_ftp_client_async(client);
  else reader.reset();
  bool failed = false;
  try { (void)co_await queued; } catch (const std::runtime_error &) { failed = true; }
  expect(failed, "Abandonment must settle operations queued behind the reader");
}

static cardio::promise<void> engine_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client, Server &server,
    bool fail_final) {
  const auto local = server.directory / "local";
  std::filesystem::create_directories(local / "source/bundle/nested");
  std::filesystem::create_directories(local / "received/bundle");
  std::ofstream(local / "source/bundle/report.txt") << "engine content";
  std::ofstream(local / "source/bundle/empty.bin");
  std::ofstream(local / "source/bundle/nested/data.txt") << "deep";
  std::ofstream(local / "source/payload.bin") << "uncommitted";
  std::ofstream(local / "received/bundle/report.txt") << "old download";
  std::filesystem::create_directories(server.directory / "home/bundle");
  std::ofstream(server.directory / "home/bundle/report.txt") << "old upload";
  unsigned conflicts = 0;
  std::string failure_message;
  elder_terms::FileTransferCallbacks callbacks;
  callbacks.conflict = [&conflicts](const elder_terms::FileTransferConflict &,
                                     cardio::cancellation) {
    ++conflicts;
    return cardio::resolved(elder_terms::FileTransferConflictAction::overwrite);
  };
  callbacks.failure = [&failure_message](const elder_terms::FileTransferFailure &failure,
                                          cardio::cancellation) {
    failure_message = failure.message;
    return cardio::resolved(elder_terms::FileTransferFailureAction::abort);
  };
  elder_terms::FileTransferRequest sending;
  sending.direction = elder_terms::FileTransferDirection::send;
  sending.source_paths = {(local / (fail_final ? "source/payload.bin" : "source/bundle")).string()};
  sending.destination_directory = "/home";
  sending.callbacks = callbacks;
  bool failed = false;
  try {
    co_await elder_terms::run_file_transfer_async(client, std::move(sending), {});
  } catch (const std::runtime_error &) {
    failed = true;
  }
  if (fail_final) {
    expect(failed && failure_message.find("451") != std::string::npos &&
               !std::filesystem::exists(server.directory / "home/payload.bin"),
           "A failed final STOR response must never commit the temporary file");
    co_return;
  }
  expect(!failed && conflicts == 1, "Recursive upload must apply the conflict decision once");
  expect_file(server.directory / "home/bundle/report.txt", "engine content");
  expect_file(server.directory / "home/bundle/empty.bin", "");
  expect_file(server.directory / "home/bundle/nested/data.txt", "deep");
  conflicts = 0;
  elder_terms::FileTransferRequest receiving;
  receiving.direction = elder_terms::FileTransferDirection::receive;
  receiving.source_paths = {"/home/bundle"};
  receiving.destination_directory = (local / "received").string();
  receiving.callbacks = callbacks;
  co_await elder_terms::run_file_transfer_async(client, std::move(receiving), {});
  expect(conflicts == 1, "Recursive download must apply the conflict decision once");
  expect_file(local / "received/bundle/report.txt", "engine content");
  expect_file(local / "received/bundle/empty.bin", "");
  expect_file(local / "received/bundle/nested/data.txt", "deep");
  for (const auto &entry : std::filesystem::recursive_directory_iterator(server.directory)) {
    expect(entry.path().filename().string().find(".elder-terms-part-") == std::string::npos,
           "Successful transfers must leave no temporary files");
  }
}

static cardio::promise<void> scenario_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client, Server &server,
    const std::string &name) {
  if (name == "passive" || name == "active" || name.starts_with("legacy-")) {
    co_await direct_async(client, server);
  } else if (name == "backpressure" || name == "cancel-backpressure") {
    co_await bounded_upload_async(client, server, name == "cancel-backpressure");
  } else if (name.starts_with("held-") || name.starts_with("cancel-final-")) {
    co_await held_final_async(client, server, name.ends_with("upload"), name.starts_with("cancel-"));
  } else if (name == "abandon" || name == "stop-reader") {
    co_await abandoned_async(client, name == "stop-reader");
  } else if (name == "engine" || name == "engine-failure") {
    co_await engine_async(client, server, name == "engine-failure");
  } else {
    co_await failed_transfer_async(client, server, name);
  }
}

static cardio::promise<void> run_async(
    Server &server, const std::string &name, cardio::dispatcher_group_glib &group,
    std::exception_ptr &failure) {
  std::shared_ptr<elder_terms::RemoteFileClient> client;
  try {
    cardio::cancellation_source cancellation;
    auto opening = elder_terms::open_ftp_client_async({
        .connection = {.address = "127.0.0.1", .port = server.port,
                       .username = "alice", .data_connection_mode =
                           name == "active" || name == "legacy-active"
                               ? elder_terms::FtpDataConnectionMode::active
                               : elder_terms::FtpDataConnectionMode::passive,
                       .local_directory = {}, .remote_directory = {},
                       .tls_mode = tls_mode, .ca_file = tls_certificate},
        .password = "secret"}, cancellation.get_cancellation());
    if (name == "cancel-control-tls") {
      if (opening.is_ready()) client = co_await opening;
      co_await wait_event_async(server, "TLS_WAIT");
      (void)cancellation.cancel();
      release(server, true);
      bool canceled = false;
      try { client = co_await opening; } catch (const cardio::canceled_exception &) { canceled = true; }
      expect(canceled, "TLS authentication must observe cancellation");
      group.shutdown();
      co_return;
    }
    client = co_await opening;
    if (name == "cancel-data-tls" || name == "stop-data-tls") {
      auto listing = client->load_directory_async("/home", cancellation.get_cancellation());
      co_await wait_event_async(server, "TLS_WAIT");
      expect(!listing.is_ready(), "Unfinished TLS handshake must not complete listing");
      if (name == "stop-data-tls") co_await elder_terms::stop_ftp_client_async(client);
      else (void)cancellation.cancel();
      release(server, true);
      bool failed = false;
      try { (void)co_await listing; } catch (const std::exception &) { failed = true; }
      expect(failed, "TLS data handshake must settle on stop or cancellation");
    } else co_await scenario_async(client, server, name);
  } catch (...) {
    failure = std::current_exception();
    ::close(server.commands);
    server.commands = -1;
  }
  if (client) co_await elder_terms::stop_ftp_client_async(client);
  group.shutdown();
}

static void run_case(const std::string &executable, const std::string &name) {
  std::vector<std::string> options;
  if (name.starts_with("legacy-")) options.push_back("--legacy-data");
  if (name == "backpressure" || name == "cancel-backpressure") options.push_back("--hold-upload");
  if (name.starts_with("held-") || name.starts_with("cancel-final-") || name == "large-size") {
    options.push_back("--hold-final");
  }
  if (name == "upload-error" || name == "download-error" || name == "engine-failure") {
    options.push_back("--final-error");
  }
  if (name.ends_with("disconnect")) options.push_back("--final-disconnect");
  if (name == "truncated") options.push_back("--truncated-download");
  if (name == "large-size") options.push_back("--large-size");
  if (name == "store-refused") options.push_back("--reject-store");
  if (name == "cancel-control-tls") options.push_back("--hold-control-tls");
  if (name == "cancel-data-tls" || name == "stop-data-tls") options.push_back("--hold-data-tls");
  Server server(executable, options);
  std::exception_ptr failure;
  {
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib_auto dispatcher(group);
    auto task = run_async(server, name, group, failure);
    dispatcher.park();
  }
  if (failure) std::rethrow_exception(failure);
}

} // namespace elder_terms_curl_ftp_stream_test

int main(int argc, char **argv) {
  try {
    using namespace elder_terms_curl_ftp_stream_test;
    if (argc == 6) {
      tls_name = argv[3];
      tls_mode = tls_name == "implicit" ? elder_terms::FtpTlsMode::implicit_tls : elder_terms::FtpTlsMode::explicit_tls;
      tls_certificate = argv[4];
      tls_key = argv[5];
    }
    expect(argc == 3 || argc == 6, "Expected the FTP test server executable path and case name");
    std::cout << "curl-ftp-stream-test: " << argv[2] << std::endl;
    run_case(argv[1], argv[2]);
    std::cout << "curl-ftp-stream-test: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "curl-ftp-stream-test: FAIL: " << error.what() << '\n';
    return 1;
  }
}

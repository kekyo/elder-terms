#include "ftp-client.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace elder_terms_ftp_client_lifetime_test {

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
    std::filesystem::create_directories(directory / "home");
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

static std::size_t resource_count(const char *directory) {
  std::size_t result = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    (void)entry;
    ++result;
  }
  return result;
}

static std::string describe_descriptors() {
  std::string result;
  for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
    std::error_code error;
    const auto target = std::filesystem::read_symlink(entry.path(), error);
    result += entry.path().filename().string() + " -> " +
        (error ? error.message() : target.string()) + '\n';
  }
  return result;
}

static cardio::promise<void> session_async(Server &server, const std::string &mode) {
  std::shared_ptr<elder_terms::RemoteFileClient> client;
  std::exception_ptr failure;
  try {
    try {
      client = co_await elder_terms::open_ftp_client_async({
          .connection = {.address = "127.0.0.1", .port = server.port,
                         .username = "alice",
                         .data_connection_mode = elder_terms::FtpDataConnectionMode::passive,
                         .local_directory = {}, .remote_directory = {},
                       .tls_mode = tls_mode, .ca_file = tls_certificate},
          .password = "secret"}, {});
    } catch (const std::runtime_error &error) {
      expect(mode == "login" && std::string(error.what()).find("530") != std::string::npos,
             "Repeated authentication failures must retain their FTP response");
      co_return;
    }
    expect(mode != "login", "Rejected authentication unexpectedly succeeded");
    auto writer = std::move(co_await client->open_write_async("/home/repeated.bin", std::nullopt, {}));
    const std::string payload("repeated binary\0payload", 23);
    co_await write_text_async(*writer, payload, {});
    cardio::cancellation_source cancellation;
    auto closing = writer->close_async(cancellation.get_cancellation());
    if (mode == "cancel") {
      co_await wait_event_async(server, "FINAL_WAIT");
      (void)cancellation.cancel();
      release(server, true);
    }
    bool canceled = false;
    try {
      co_await closing;
    } catch (const cardio::canceled_exception &) {
      canceled = true;
    }
    expect(canceled == (mode == "cancel"), "Repeated transfer completion has the wrong outcome");
    writer.reset();
    if (mode == "normal") {
      auto reader = std::move(co_await client->open_read_async("/home/repeated.bin", {}));
      expect(co_await read_all_async(*reader, {}) == payload,
             "Repeated connections must preserve transferred bytes");
      co_await reader->close_async({});
    }
  } catch (...) {
    failure = std::current_exception();
    if (server.commands >= 0) ::close(std::exchange(server.commands, -1));
  }
  if (client) co_await elder_terms::stop_ftp_client_async(client);
  if (failure) std::rethrow_exception(failure);
}

static cardio::promise<void> repeat_async(
    Server &server, const std::string &mode, cardio::dispatcher_group_glib &group,
    std::exception_ptr &failure) {
  try {
    // Warm up the dispatcher, resolver, and library before observing retention.
    // A returned start_new promise may precede the worker's final exit
    // instructions, so allow a bounded native-thread tail instead of sleeping.
    for (unsigned iteration = 0; iteration < 3; ++iteration) co_await session_async(server, mode);
    const auto descriptors = resource_count("/proc/self/fd");
    const auto descriptor_details = describe_descriptors();
    const auto threads = resource_count("/proc/self/task");
    for (unsigned iteration = 0; iteration < 24; ++iteration) {
      co_await session_async(server, mode);
      const auto current_descriptors = resource_count("/proc/self/fd");
      const auto current_threads = resource_count("/proc/self/task");
      std::cout << "resources " << iteration << " fd=" << current_descriptors
                << " threads=" << current_threads << std::endl;
      if (current_descriptors > descriptors) {
        std::cerr << "Baseline descriptors (" << descriptors << "):\n"
                  << descriptor_details << "Current descriptors ("
                  << current_descriptors << "):\n" << describe_descriptors();
      }
      expect(current_descriptors <= descriptors, "Stopped FTP sessions retain file descriptors");
      expect(current_threads <= threads + 2, "Stopped FTP sessions accumulate worker threads");
    }
  } catch (...) {
    failure = std::current_exception();
  }
  group.shutdown();
}

} // namespace elder_terms_ftp_client_lifetime_test

int main(int argc, char **argv) {
  using namespace elder_terms_ftp_client_lifetime_test;
  try {
    if (argc == 6) {
      tls_name = argv[3];
      tls_mode = tls_name == "implicit" ? elder_terms::FtpTlsMode::implicit_tls : elder_terms::FtpTlsMode::explicit_tls;
      tls_certificate = argv[4];
      tls_key = argv[5];
    }
    expect(argc == 3 || argc == 6, "Expected FTP server executable and lifetime scenario");
    const std::string mode = argv[2];
    expect(mode == "normal" || mode == "login" || mode == "cancel", "Unknown lifetime scenario");
    std::vector<std::string> options;
    if (mode == "login") options.push_back("--reject-login");
    if (mode == "cancel") options.push_back("--hold-final");
    Server server(argv[1], options);
    std::exception_ptr failure;
    // A private context makes the driver's sources independent of process
    // defaults and gives resource checkers a complete context lifetime.
    const auto context = std::unique_ptr<GMainContext, decltype(&g_main_context_unref)>(
        g_main_context_new(), g_main_context_unref);
    {
      cardio::dispatcher_group_glib group(context.get());
      cardio::dispatcher_host_glib_auto dispatcher(group);
      auto task = repeat_async(server, mode, group, failure);
      dispatcher.park();
    }
    if (failure) std::rethrow_exception(failure);
    std::cout << "ftp-client-lifetime-test: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "ftp-client-lifetime-test: FAIL: " << error.what() << '\n';
    return 1;
  }
}

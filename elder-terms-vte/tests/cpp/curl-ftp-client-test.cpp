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
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace elder_terms_curl_ftp_client_test {

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
    std::ofstream(directory / "home/large.bin");
    std::filesystem::resize_file(directory / "home/large.bin", 4294967313ULL);
    std::ofstream(directory / "home/space % # \" 日本;type=a/file % # \" 日本;type=i") << "special";
    int output[2];
    int input[2];
    expect(::pipe2(output, O_CLOEXEC) == 0 && ::pipe2(input, O_CLOEXEC) == 0,
           "Test server pipes failed");
    std::vector<std::string> arguments{executable, directory.string(), "--trace"};
    arguments.insert(arguments.end(), options.begin(), options.end());
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

static cardio::promise<void> browse_async(
    Server &server, bool active, bool ipv6, bool facts) {
  const auto cancellation = cardio::cancellation{};
  auto client = co_await elder_terms::open_ftp_client_async({
      .connection = {.address = ipv6 ? "::1" : "127.0.0.1", .port = server.port,
                     .username = "alice", .data_connection_mode = active
                         ? elder_terms::FtpDataConnectionMode::active
                         : elder_terms::FtpDataConnectionMode::passive,
                     .local_directory = {}, .remote_directory = {}},
      .password = "secret"}, cancellation);
  std::exception_ptr failure;
  try {
    const auto capabilities = client->capabilities();
    expect(!capabilities.symbolic_links && !capabilities.permissions &&
               !capabilities.access_time && !capabilities.modification_time,
           "FTP must not claim unsupported metadata updates");
    expect(client->try_begin_transfer(), "First transfer slot must be available");
    expect(!client->try_begin_transfer(), "Bulk transfers must be exclusive");
    client->end_transfer();
    expect(client->try_begin_transfer(), "Released transfer slot must be reusable");
    client->end_transfer();

    const auto initial = co_await client->load_directory_async("", cancellation);
    expect(initial.canonical_path == "/home" && initial.entries.size() == 5,
           "Initial listing must use the login directory");
    expect(initial.entries[0].name == "data.txt" && initial.entries[0].size == 7 &&
               initial.entries[0].type == elder_terms::RemoteFileType::regular,
           "Listing must expose file attributes and sort entries");
    expect(initial.entries[2].name == "directory" &&
               initial.entries[2].type == elder_terms::RemoteFileType::directory,
           "Listing must identify directories");
    expect(initial.entries[3].name == "large.bin" &&
               initial.entries[3].size == 4294967313ULL,
           "Listing must preserve file sizes above 32 bits");
    if (facts) {
      expect(initial.entries[0].modification_time_unix_seconds == 1704164645,
             "MLSD must preserve the UTC modification timestamp");
      const auto root = co_await client->lstat_async("/", cancellation);
      expect(root && root->type == elder_terms::RemoteFileType::directory,
             "MLST must inspect the server root");
    }
    const auto alias = co_await client->load_directory_async("/alias", cancellation);
    expect(alias.canonical_path == "/home", "PWD must return the server's canonical path");
    const auto relative = co_await client->load_directory_async("directory", cancellation);
    expect(relative.canonical_path == "/home/directory" && relative.entries.empty(),
           "Relative paths must be resolved from the current directory");
    const auto special = co_await client->load_directory_async("../space % # \" 日本;type=a", cancellation);
    expect(special.canonical_path == "/home/space % # \" 日本;type=a" &&
               special.entries.size() == 1 && special.entries[0].name == "file % # \" 日本;type=i",
           "FTP URL encoding must preserve special path characters");
    const auto special_file = co_await client->lstat_async(special.entries[0].path, cancellation);
    expect(special_file && special_file->size == 7, "Special paths must work for attributes");
    co_await client->make_directory_async("new folder", std::nullopt, cancellation);
    expect(std::filesystem::is_directory(server.directory / special.canonical_path.substr(1) / "new folder"),
           "MKD must change the server filesystem");
    co_await client->remove_directory_async("new folder", cancellation);
    expect(!std::filesystem::exists(server.directory / special.canonical_path.substr(1) / "new folder"),
           "RMD must change the server filesystem");
    co_await client->rename_async(special.entries[0].path, "/home/renamed.txt", cancellation);
    expect(std::filesystem::exists(server.directory / "home/renamed.txt") &&
               !std::filesystem::exists(server.directory / special.entries[0].path.substr(1)),
           "RNFR and RNTO must move the file");
    co_await client->remove_file_async("/home/renamed.txt", cancellation);
    expect(!(co_await client->lstat_async("/home/renamed.txt", cancellation)),
           "Deleted files must be absent from subsequent attributes");

    bool rejected = false;
    try {
      co_await client->rename_async("/home/data.txt", "/home/denied", cancellation);
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    expect(rejected && std::filesystem::exists(server.directory / "home/data.txt"),
           "RNTO refusal must not be reported as success");
    rejected = false;
    try {
      co_await client->remove_file_async("/home/denied", cancellation);
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    expect(rejected, "DELE permission denial must remain observable");
    rejected = false;
    try {
      (void)co_await client->lstat_async("/home/denied/child", cancellation);
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    expect(rejected, "A parent-directory refusal must not mean a nonexistent child");
    if (facts) {
      rejected = false;
      try {
        (void)co_await client->lstat_async("/home/denied", cancellation);
      } catch (const std::runtime_error &) {
        rejected = true;
      }
      expect(rejected, "An MLST refusal for a listed item must not mean nonexistent");
    }
    rejected = false;
    try {
      co_await client->make_directory_async("/home/injected\r\nDELE data.txt", std::nullopt, cancellation);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    expect(rejected, "Control characters must be rejected before sending commands");
    co_await client->make_directory_async("after-refusal", std::nullopt, cancellation);
    expect(std::filesystem::is_directory(
               server.directory / special.canonical_path.substr(1) / "after-refusal"),
           "Metadata lookups and reconnections must preserve the relative-path base");
    co_await client->remove_directory_async("after-refusal", cancellation);
    const auto restored = co_await client->load_directory_async("/home", cancellation);
    expect(restored.canonical_path == "/home" && restored.entries.size() == 5,
           "Ordinary operation refusals must leave the client usable");
  } catch (...) {
    failure = std::current_exception();
  }
  co_await elder_terms::stop_ftp_client_async(client);
  if (failure) std::rethrow_exception(failure);
}

static cardio::promise<void> run_async(
    Server &server, bool active, bool ipv6, bool facts,
    cardio::dispatcher_group_glib &group, std::exception_ptr &failure) {
  try {
    co_await browse_async(server, active, ipv6, facts);
  } catch (...) {
    failure = std::current_exception();
  }
  group.shutdown();
}

static void run_case(const std::string &executable,
                      std::vector<std::string> options,
                      bool active, bool ipv6, bool facts) {
  Server server(executable, options);
  std::exception_ptr failure;
  {
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib_auto dispatcher(group);
    auto task = run_async(server, active, ipv6, facts, group, failure);
    dispatcher.park();
  }
  if (failure) std::rethrow_exception(failure);
}

enum class FailureCase { login, empty_user, unsafe_password, denied_listing, temporary_listing, foreign_active };

static cardio::promise<void> failure_case_async(
    Server &server, FailureCase test_case,
    cardio::dispatcher_group_glib &group, std::exception_ptr &failure) {
  std::shared_ptr<elder_terms::RemoteFileClient> client;
  try {
    bool rejected = false;
    bool authenticated = false;
    try {
      client = co_await elder_terms::open_ftp_client_async({
          .connection = {.address = "127.0.0.1", .port = server.port,
                         .username = test_case == FailureCase::empty_user ? "" : "alice",
                         .data_connection_mode = test_case == FailureCase::foreign_active
                             ? elder_terms::FtpDataConnectionMode::active
                             : elder_terms::FtpDataConnectionMode::passive,
                         .local_directory = {}, .remote_directory = {}},
          .password = test_case == FailureCase::unsafe_password ? "secret\r\nDELE data.txt" : "secret"},
          {});
      authenticated = true;
      if (test_case == FailureCase::denied_listing || test_case == FailureCase::temporary_listing ||
          test_case == FailureCase::foreign_active) {
        (void)co_await client->load_directory_async("/home", {});
      }
    } catch (const std::invalid_argument &) {
      expect(test_case == FailureCase::empty_user || test_case == FailureCase::unsafe_password,
             "Unexpected input-validation error");
      rejected = true;
    } catch (const std::runtime_error &error) {
      if (test_case == FailureCase::foreign_active) {
        expect(authenticated, "Foreign-peer rejection must happen after authentication");
      } else {
        const auto expected = test_case == FailureCase::login ? "530" :
            test_case == FailureCase::temporary_listing ? "450" : "550";
        expect(std::string(error.what()).find(expected) != std::string::npos,
               "FTP failures must retain the server response code");
      }
      rejected = true;
    }
    expect(rejected, "FTP refusal must not silently succeed or fall back");
  } catch (...) {
    failure = std::current_exception();
  }
  if (client) co_await elder_terms::stop_ftp_client_async(client);
  group.shutdown();
}

static void run_failure_case(const std::string &executable, FailureCase test_case) {
  std::vector<std::string> options;
  if (test_case == FailureCase::login) options.push_back("--reject-login");
  if (test_case == FailureCase::denied_listing) options.push_back("--mlsd-denied");
  if (test_case == FailureCase::temporary_listing) options.push_back("--mlsd-temporary");
  if (test_case == FailureCase::foreign_active) options.push_back("--foreign-active");
  Server server(executable, options);
  std::exception_ptr failure;
  {
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib_auto dispatcher(group);
    auto task = failure_case_async(server, test_case, group, failure);
    dispatcher.park();
  }
  if (failure) std::rethrow_exception(failure);
}

static cardio::promise<void> fifo_case_async(
    Server &server, cardio::dispatcher_group_glib &group,
    std::exception_ptr &failure) {
  std::shared_ptr<elder_terms::RemoteFileClient> client;
  try {
    client = co_await elder_terms::open_ftp_client_async({
        .connection = {.address = "127.0.0.1", .port = server.port,
                       .username = "alice", .data_connection_mode = elder_terms::FtpDataConnectionMode::passive,
                       .local_directory = {}, .remote_directory = {}},
        .password = "secret"}, {});
    auto listing = client->load_directory_async("/home", {});
    for (;;) {
      co_await cardio::from_fd(server.events, cardio::fd_event::read);
      if (read_line(server.events) == "LIST_WAIT") break;
    }
    auto mutation = client->make_directory_async("/home/after", std::nullopt, {});
    cardio::cancellation_source cancellation;
    auto canceled = client->make_directory_async("/home/not-created", std::nullopt,
                                                  cancellation.get_cancellation());
    (void)cancellation.cancel();
    bool observed = false;
    try {
      co_await canceled;
    } catch (const cardio::canceled_exception &) {
      observed = true;
    }
    expect(observed, "Queued requests must cancel while the first request is held");
    expect(::write(server.commands, "release\n", 8) == 8, "Listing release failed");
    const auto snapshot = co_await listing;
    expect(snapshot.entries.size() == 5, "Queued mutations must follow the complete listing");
    co_await mutation;
    expect(std::filesystem::is_directory(server.directory / "home/after") &&
               !std::filesystem::exists(server.directory / "home/not-created"),
           "FIFO must preserve accepted requests and remove only canceled requests");
  } catch (...) {
    failure = std::current_exception();
    ::close(server.commands);
    server.commands = -1;
  }
  if (client) co_await elder_terms::stop_ftp_client_async(client);
  group.shutdown();
}

static void run_fifo_case(const std::string &executable) {
  Server server(executable, {"--hold-first-list"});
  std::exception_ptr failure;
  {
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib_auto dispatcher(group);
    auto task = fifo_case_async(server, group, failure);
    dispatcher.park();
  }
  if (failure) std::rethrow_exception(failure);
}

} // namespace elder_terms_curl_ftp_client_test

int main(int argc, char **argv) {
  using namespace elder_terms_curl_ftp_client_test;
  try {
    expect(argc == 3, "Expected the FTP test server executable path and case name");
    const std::string name = argv[2];
    std::cout << "curl-ftp-client-test: " << name << std::endl;
    if (name == "passive") run_case(argv[1], {}, false, false, true);
    else if (name == "active") run_case(argv[1], {}, true, false, true);
    else if (name == "legacy-passive") run_case(argv[1], {"--legacy-data"}, false, false, true);
    else if (name == "legacy-active") run_case(argv[1], {"--legacy-data"}, true, false, true);
    else if (name == "ipv6-passive") run_case(argv[1], {"--ipv6"}, false, true, true);
    else if (name == "ipv6-active") run_case(argv[1], {"--ipv6"}, true, true, true);
    else if (name == "unix") run_case(argv[1], {"--unix"}, false, false, false);
    else if (name == "dos") run_case(argv[1], {"--dos"}, false, false, false);
    else if (name == "mlsd-unavailable") run_case(argv[1], {"--mlsd-unavailable"}, false, false, false);
    else if (name == "login") run_failure_case(argv[1], FailureCase::login);
    else if (name == "empty-user") run_failure_case(argv[1], FailureCase::empty_user);
    else if (name == "unsafe-password") run_failure_case(argv[1], FailureCase::unsafe_password);
    else if (name == "denied-listing") run_failure_case(argv[1], FailureCase::denied_listing);
    else if (name == "temporary-listing") run_failure_case(argv[1], FailureCase::temporary_listing);
    else if (name == "foreign-active") run_failure_case(argv[1], FailureCase::foreign_active);
    else if (name == "fifo") run_fifo_case(argv[1]);
    else throw std::runtime_error("Unknown FTP client case: " + name);
    std::cout << "curl-ftp-client-test: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "curl-ftp-client-test: FAIL: " << error.what() << '\n';
    return 1;
  }
}

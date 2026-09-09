#include "ftp-client.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

// Test executable only: the ordinary public factory must depend on curl.
extern "C" CURLcode __wrap_curl_global_init(long) {
  return CURLE_FAILED_INIT;
}

namespace elder_terms_ftp_backend_removal_test {

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> check_async(unsigned port,
    cardio::dispatcher_group_glib &group, int &result) {
  try {
    auto client = co_await elder_terms::open_ftp_client_async({
        .connection = {.address = "127.0.0.1", .port = port, .username = "alice",
            .data_connection_mode = elder_terms::FtpDataConnectionMode::passive,
            .local_directory = {}, .remote_directory = "/home"},
        .password = "secret"}, {});
    std::cerr << "FTP authenticated despite failed libcurl initialization\n";
    result = 1;
  } catch (const std::exception &error) {
    result = std::string(error.what()) == curl_easy_strerror(CURLE_FAILED_INIT) ? 0 : 1;
    if (result != 0) std::cerr << "Unexpected initialization error: " << error.what() << '\n';
  }
  group.shutdown();
}

static int wait_child(pid_t pid) {
  int status;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) continue;
    throw std::runtime_error("Test child could not be reaped");
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

} // namespace elder_terms_ftp_backend_removal_test

int main(int argc, char **argv) {
  using namespace elder_terms_ftp_backend_removal_test;
  pid_t server = -1;
  std::string directory;
  int events = -1;
  int result = 1;
  try {
    expect(argc == 2, "Expected the FTP server executable path");
    directory = "/tmp/elder-terms-ftp-removal-XXXXXX";
    expect(::mkdtemp(directory.data()) != nullptr, "Temporary directory creation failed");
    std::filesystem::create_directories(directory + "/home");
    int output[2];
    expect(::pipe2(output, O_CLOEXEC) == 0, "Test event pipe failed");
    server = ::fork();
    expect(server >= 0, "Server fork failed");
    if (server == 0) {
      ::dup2(output[1], STDOUT_FILENO);
      ::execl(argv[1], argv[1], directory.c_str(), static_cast<char *>(nullptr));
      ::_exit(127);
    }
    ::close(output[1]);
    events = output[0];
    std::string ready;
    char ch;
    while (true) {
      const auto count = ::read(events, &ch, 1);
      if (count < 0 && errno == EINTR) continue;
      expect(count > 0, "Server did not announce readiness");
      if (ch == '\n') break;
      ready += ch;
    }
    expect(ready.starts_with("READY "), "Invalid FTP server readiness event");
    const unsigned port = std::stoul(ready.substr(6));
    const auto client = ::fork();
    expect(client >= 0, "Client fork failed");
    if (client == 0) {
      cardio::dispatcher_group_glib group;
      cardio::dispatcher_host_glib dispatcher(group);
      auto task = check_async(port, group, result);
      dispatcher.park();
      // This isolated process only proves the initialization dependency. Its
      // owner reaps it and the server even when the legacy factory succeeds.
      // Normal asynchronous cleanup is covered by the permanent client tests.
      ::_exit(result);
    }
    result = wait_child(client);
  } catch (const std::exception &error) {
    std::cerr << "FTP backend removal test: " << error.what() << '\n';
  }
  if (events >= 0) ::close(events);
  if (server > 0) {
    ::kill(server, SIGTERM);
    (void)wait_child(server);
  }
  if (!directory.empty()) std::filesystem::remove_all(directory);
  return result;
}

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
#include <memory>
#include <stdexcept>
#include <string>

static curl_version_info_data reported_version{};

/**
 * Reports fixture capabilities to the application, without changing libcurl.
 * @param age Requested information age, unused by this fixed fixture.
 * @returns Runtime information owned by this test process.
 */
extern "C" curl_version_info_data *__wrap_curl_version_info(CURLversion age) {
  (void)age;
  return &reported_version;
}

namespace elder_terms_ftp_runtime_capabilities_test {

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> verify_async(
    unsigned port, const std::string &expected_error, elder_terms::FtpTlsMode tls_mode,
    cardio::dispatcher_group_glib &group, int &result) {
  std::shared_ptr<elder_terms::RemoteFileClient> client;
  try {
    elder_terms::FtpConnectionSettings connection{.address = "127.0.0.1", .port = port, .username = "alice",
            .data_connection_mode = elder_terms::FtpDataConnectionMode::passive,
            .local_directory = {}, .remote_directory = "/home",
            .tls_mode = tls_mode};
    if (expected_error == "require OpenSSL") {
      auto store = elder_terms::create_settings_store(elder_terms::ftp_connection_setting_definitions());
      auto *ini = g_key_file_new();
      g_key_file_set_string(ini, "ftp", "tls_mode", "explicit");
      g_key_file_set_string(ini, "ftp", "tls_compatibility", "openssl_legacy");
      std::vector<std::string> warnings;
      elder_terms::load_settings_store_from_key_file(&store, ini, &warnings);
      g_key_file_unref(ini);
      connection = elder_terms::ftp_connection_settings(store);
      connection.address = "127.0.0.1";
      connection.username = "alice";
      connection.port = port;
    }
    client = co_await elder_terms::open_ftp_client_async({.connection = std::move(connection), .password = "secret"}, {});
    expect(expected_error.empty(), "An unsupported FTP runtime was accepted");
    const auto snapshot = co_await client->load_directory_async("/home", {});
    expect(snapshot.canonical_path == "/home" && snapshot.entries.empty(),
           "The supported runtime must authenticate and list the real server");
    result = 0;
  } catch (const std::exception &error) {
    if (!expected_error.empty() &&
        std::string(error.what()).find(expected_error) != std::string::npos) {
      result = 0;
    } else {
      std::cerr << "Unexpected capability result: " << error.what() << '\n';
      result = 1;
    }
  }
  if (client) {
    try {
      co_await elder_terms::stop_ftp_client_async(client);
    } catch (const std::exception &error) {
      std::cerr << "FTP cleanup failed: " << error.what() << '\n';
      result = 1;
    }
  }
  group.shutdown();
}

} // namespace elder_terms_ftp_runtime_capabilities_test

int main(int argc, char **argv) {
  using namespace elder_terms_ftp_runtime_capabilities_test;
  pid_t server = -1;
  std::string directory;
  int events = -1;
  int result = 1;
  try {
    expect(argc == 3, "Expected the FTP server executable and capability scenario");
    const std::string scenario = argv[2];
    expect(scenario == "supported" || scenario == "no-dns" ||
               scenario == "no-ftp" || scenario == "old-version" || scenario == "no-tls" || scenario == "no-ftps" || scenario == "non-openssl",
           "Unknown runtime capability scenario");
    static const char *const ftp_protocols[] = {"ftp", nullptr};
    static const char *const other_protocols[] = {"http", nullptr};
    static const char *const supported_features[] = {"AsynchDNS", "Largefile", "threadsafe", nullptr};
    static const char *const tls_features[] = {"AsynchDNS", "SSL", nullptr};
    static const char *const other_features[] = {"Largefile", "threadsafe", nullptr};
    // Capabilities are supplied through the documented feature-name array.
    // Other fields retain their zero-initialized values unless needed here.
    reported_version.age = scenario == "old-version" ? CURLVERSION_TENTH : CURLVERSION_ELEVENTH;
    reported_version.version = scenario == "old-version" ? "7.86.0" : "7.88.1";
    reported_version.version_num = scenario == "old-version" ? 0x075600 : 0x075801;
    reported_version.host = "FTP test runtime";
    reported_version.protocols = scenario == "no-ftp" ? other_protocols : ftp_protocols;
    reported_version.feature_names = scenario == "no-dns" ? other_features : (scenario == "no-ftps" || scenario == "non-openssl") ? tls_features : supported_features;
    if (scenario == "non-openssl") reported_version.ssl_version = "GnuTLS/3.7.9";
    const std::string expected_error = scenario == "no-dns" ? "asynchronous DNS"
        : scenario == "no-ftp" ? "without FTP support"
        : scenario == "old-version" ? "7.88.1 or newer"
        : scenario == "no-tls" ? "TLS support"
        : scenario == "no-ftps" ? "FTPS support"
        : scenario == "non-openssl" ? "require OpenSSL" : "";

    directory = "/tmp/elder-terms-ftp-capabilities-XXXXXX";
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
    for (;;) {
      const auto count = ::read(events, &ch, 1);
      if (count < 0 && errno == EINTR) continue;
      expect(count > 0, "Server did not announce readiness");
      if (ch == '\n') break;
      ready += ch;
    }
    expect(ready.starts_with("READY "), "Invalid FTP server readiness event");
    const unsigned port = std::stoul(ready.substr(6));
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib_auto dispatcher(group);
    auto task = verify_async(port, expected_error,
        scenario == "no-tls" ? elder_terms::FtpTlsMode::explicit_tls :
        scenario == "no-ftps" ? elder_terms::FtpTlsMode::implicit_tls : elder_terms::FtpTlsMode::none, group, result);
    dispatcher.park();
  } catch (const std::exception &error) {
    std::cerr << "FTP runtime capabilities test: " << error.what() << '\n';
  }
  if (events >= 0) ::close(events);
  if (server > 0) {
    ::kill(server, SIGTERM);
    while (::waitpid(server, nullptr, 0) < 0 && errno == EINTR) {}
  }
  if (!directory.empty()) std::filesystem::remove_all(directory);
  if (result == 0) std::cout << "ftp-runtime-capabilities-test: PASS\n";
  return result;
}

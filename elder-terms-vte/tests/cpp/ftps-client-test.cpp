#include "../../src/ftp/ftp-client.h"

#include <array>
#include <curl/curl.h>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <iostream>
#include <stdexcept>

namespace elder_terms_ftps_test {

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static std::size_t socket_count() {
  std::size_t count = 0;
  for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
    std::error_code error;
    if (std::filesystem::read_symlink(entry.path(), error).string().starts_with("socket:")) ++count;
  }
  return count;
}

static cardio::promise<void> run_async(
    elder_terms::FtpConnectionSettings connection, std::string scenario,
    cardio::dispatcher_group_glib &group, std::exception_ptr &failure) {
  std::shared_ptr<elder_terms::RemoteFileClient> client;
  std::shared_ptr<elder_terms::SshSocksProxy> proxy;
  const bool approve = scenario.starts_with("approve");
  const bool expect_success = scenario == "success" || approve;
  const auto caller = std::this_thread::get_id();
  const auto root = connection.local_directory;
  unsigned confirmations = 0;
  bool succeeded = false;
  const auto initial_sockets = socket_count();
  try {
    if (const auto *port = std::getenv("ELDER_TERMS_TEST_PROXY_PORT")) {
      elder_terms::TerminalSessionCallbacks callbacks{};
      callbacks.ssh_prompt = [](const auto &prompt, cardio::cancellation) -> cardio::promise<elder_terms::SshUserPromptResponse> {
        co_return elder_terms::SshUserPromptResponse{.accepted = true, .text = prompt.initial_text};
      };
      elder_terms::SshProxySettings route{.enabled = true,
          .endpoint = {.address = "127.0.0.1", .port = std::stoll(port), .username = "gateway-test", .identity_file = {}}};
      auto connecting = elder_terms::open_ssh_socks_proxy_async(route, connection.address, callbacks,
          {.known_hosts_file = std::getenv("ELDER_TERMS_TEST_PROXY_KNOWN_HOSTS"), .config_file = {}}, {});
      proxy = std::move(co_await connecting);
      auto active = connection;
      active.data_connection_mode = elder_terms::FtpDataConnectionMode::active;
      bool rejected = false;
      try {
        auto invalid = co_await elder_terms::open_ftp_client_async(
            {.connection = active, .password = "secret", .proxy = proxy}, {});
        co_await elder_terms::stop_ftp_client_async(invalid);
      } catch (const std::invalid_argument &) { rejected = true; }
      expect(rejected, "The FTP backend must reject active mode with SSH proxy before making requests");
    }
    auto opening = elder_terms::open_ftp_client_async({.connection = std::move(connection), .password = "secret",
         .confirm_certificate = [&](const elder_terms::TlsCertificateFailure &failure, cardio::cancellation cancellation) -> cardio::promise<bool> {
           expect(std::this_thread::get_id() == caller, "Confirmation must run on the caller dispatcher");
           cancellation.throw_if_cancellation_requested();
           expect(failure.sha256.size() == 95 && !failure.subject.empty() && !failure.issuer.empty() &&
                  !failure.reason.empty() && !failure.not_before.empty() && !failure.not_after.empty(), "Confirmation must contain copied certificate details");
           ++confirmations;
           std::cout << "CONFIRM " << (failure.identity_slot == elder_terms::ftp_data_certificate_identity ? "data" : "control") << " " << failure.validation_code << " " << failure.sha256 << std::endl;
           co_return approve;
         }, .proxy = proxy}, {});
    client = co_await opening;
    if (scenario == "approve-data") {
      bool failed = false;
      try { auto writer = std::move(co_await client->open_write_async("/home/probe", 3, std::nullopt, {})); }
      catch (const std::exception &error) { failed = true; std::cout << "DATA FAILURE OBSERVED " << error.what() << std::endl; }
      expect(failed, "Approving a data certificate must not automatically replay STOR");
      expect(confirmations == 1, "A distinct data certificate must be confirmed once");
      expect(std::filesystem::file_size(root + "/home/probe") == 0, "The rejected data handshake must not write file contents");
      auto retry = std::move(co_await client->open_write_async("/home/probe", 3, std::nullopt, {}));
      const std::array<std::byte, 3> value{std::byte(0),std::byte(255),std::byte(42)};
      co_await retry->write_all_async(value, {});
      co_await retry->close_async({});
      retry.reset();
      expect(std::filesystem::file_size(root + "/home/probe") == value.size(), "An explicit retry must use the approved data certificate");
      co_await client->remove_file_async("/home/probe", {});
    }
    const auto listing = co_await client->load_directory_async("/home", {});
    expect(listing.canonical_path == "/home", "FTPS must list the login directory");
    co_await client->make_directory_async("/home/roundtrip", std::nullopt, {});
    const auto nested = co_await client->load_directory_async("/home/roundtrip", {});
    expect(nested.canonical_path == "/home/roundtrip", "FTPS must preserve the selected remote directory");
    std::array<std::byte, 65537> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = std::byte(i % 251);
    auto writer = std::move(co_await client->open_write_async("file", bytes.size(), std::nullopt, {}));
    co_await writer->write_all_async(bytes, {});
    co_await writer->close_async({});
    writer.reset();
    co_await client->rename_async("file", "renamed", {});
    auto reader = std::move(co_await client->open_read_async("renamed", {}));
    std::array<std::byte, 4096> buffer{};
    std::size_t offset = 0;
    for (;;) {
      const auto count = co_await reader->read_async(buffer, {});
      if (count == 0) break;
      expect(offset + count <= bytes.size(), "FTPS download grew unexpectedly");
      for (std::size_t i = 0; i < count; ++i)
        expect(buffer[i] == bytes[offset + i], "FTPS must preserve binary contents");
      offset += count;
    }
    if (offset != bytes.size()) std::cerr << "Downloaded " << offset << " of " << bytes.size()
        << ", server file " << std::filesystem::file_size(root + "/home/roundtrip/renamed") << std::endl;
    expect(offset == bytes.size(), "FTPS must not truncate downloads");
    co_await reader->close_async({});
    reader.reset();
    co_await client->remove_file_async("/home/roundtrip/renamed", {});
    co_await client->remove_directory_async("/home/roundtrip", {});
    expect(!(co_await client->lstat_async("/home/roundtrip", {})), "FTPS deletion must take effect");
    if (proxy) {
      // open_write waits for the actual data connection to request upload bytes.
      auto interrupted = std::move(co_await client->open_write_async("/home/interrupted", 1024 * 1024, std::nullopt, {}));
      co_await elder_terms::stop_ftp_client_async(client);
      bool stopped = false;
      try { co_await interrupted->write_all_async(bytes, {}); }
      catch (const std::exception &) { stopped = true; }
      expect(stopped, "Stopping FTP must retire a pending upload through SSH");
    }
    succeeded = true;
  } catch (const std::exception &error) {
    std::cout << "RESULT ERROR " << error.what() << std::endl;
    if (expect_success) failure = std::current_exception();
  }
  if (succeeded != expect_success && !failure)
    failure = std::make_exception_ptr(std::runtime_error("Unexpected FTPS connection result"));
  if (client) co_await elder_terms::stop_ftp_client_async(client);
  co_await elder_terms::stop_ssh_socks_proxy_async(proxy);
  if (proxy) {
    expect(!std::filesystem::exists(elder_terms::ssh_socks_proxy_socket_path(proxy)), "Stopping FTP must remove its private proxy endpoint");
    expect(socket_count() == initial_sockets, "Stopping FTP must release its control, data and SSH sockets");
  }
  group.shutdown();
}

} // namespace elder_terms_ftps_test

int main(int argc, char **argv) {
  using namespace elder_terms_ftps_test;
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--runtime-version") {
      std::cout << curl_version_info(CURLVERSION_NOW)->version_num << std::endl;
      return 0;
    }
    expect(argc == 3 || argc == 5, "Expected INI path/result pairs");
    for (int argument = 1; argument < argc; argument += 2) {
    auto store = elder_terms::create_settings_store(elder_terms::ftp_connection_setting_definitions());
    auto *ini = g_key_file_new();
    expect(g_key_file_load_from_file(ini, argv[argument], G_KEY_FILE_NONE, nullptr), "Cannot load test INI");
    std::vector<std::string> warnings;
    elder_terms::load_settings_store_from_key_file(&store, ini, &warnings);
    g_key_file_unref(ini);
    std::exception_ptr failure;
    {
      cardio::dispatcher_group_glib group;
      cardio::dispatcher_host_glib_auto dispatcher(group);
      auto task = run_async(elder_terms::ftp_connection_settings(store),
          std::string(argv[argument + 1]), group, failure);
      dispatcher.park();
    }
    if (failure) std::rethrow_exception(failure);
    }
    std::cout << "FTPS PASS" << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FTPS FAIL: " << error.what() << std::endl;
    return 1;
  }
}

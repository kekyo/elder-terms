#include "../../src/ftp/ftp-client.h"

#include <array>
#include <exception>
#include <filesystem>
#include <thread>
#include <iostream>
#include <stdexcept>

namespace elder_terms_ftps_test {

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> run_async(
    elder_terms::FtpConnectionSettings connection, std::string scenario,
    cardio::dispatcher_group_glib &group, std::exception_ptr &failure) {
  std::shared_ptr<elder_terms::RemoteFileClient> client;
  const bool approve = scenario.starts_with("approve");
  const bool expect_success = scenario == "success" || approve;
  const auto caller = std::this_thread::get_id();
  const auto root = connection.local_directory;
  unsigned confirmations = 0;
  bool succeeded = false;
  try {
    client = co_await elder_terms::open_ftp_client_async(
        {.connection = std::move(connection), .password = "secret",
         .confirm_certificate = [&](const elder_terms::FtpCertificateFailure &failure, cardio::cancellation cancellation) -> cardio::promise<bool> {
           expect(std::this_thread::get_id() == caller, "Confirmation must run on the caller dispatcher");
           cancellation.throw_if_cancellation_requested();
           expect(failure.sha256.size() == 95 && !failure.subject.empty() && !failure.issuer.empty() &&
                  !failure.reason.empty() && !failure.not_before.empty() && !failure.not_after.empty(), "Confirmation must contain copied certificate details");
           ++confirmations;
           std::cout << "CONFIRM " << (failure.channel == elder_terms::FtpTlsChannel::data ? "data" : "control") << " " << failure.validation_code << " " << failure.sha256 << std::endl;
           co_return approve;
         }}, {});
    if (scenario == "approve-data") {
      bool failed = false;
      try { auto writer = std::move(co_await client->open_write_async("/home/probe", std::nullopt, {})); }
      catch (const std::exception &error) { failed = true; std::cout << "DATA FAILURE OBSERVED " << error.what() << std::endl; }
      expect(failed, "Approving a data certificate must not automatically replay STOR");
      expect(confirmations == 1, "A distinct data certificate must be confirmed once");
      expect(std::filesystem::file_size(root + "/home/probe") == 0, "The rejected data handshake must not write file contents");
      auto retry = std::move(co_await client->open_write_async("/home/probe", std::nullopt, {}));
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
    auto writer = std::move(co_await client->open_write_async("file", std::nullopt, {}));
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
    expect(offset == bytes.size(), "FTPS must not truncate downloads");
    co_await reader->close_async({});
    reader.reset();
    co_await client->remove_file_async("/home/roundtrip/renamed", {});
    co_await client->remove_directory_async("/home/roundtrip", {});
    expect(!(co_await client->lstat_async("/home/roundtrip", {})), "FTPS deletion must take effect");
    succeeded = true;
  } catch (const std::exception &error) {
    std::cout << "RESULT ERROR " << error.what() << std::endl;
    if (expect_success) failure = std::current_exception();
  }
  if (succeeded != expect_success && !failure)
    failure = std::make_exception_ptr(std::runtime_error("Unexpected FTPS connection result"));
  if (client) co_await elder_terms::stop_ftp_client_async(client);
  group.shutdown();
}

} // namespace elder_terms_ftps_test

int main(int argc, char **argv) {
  using namespace elder_terms_ftps_test;
  try {
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

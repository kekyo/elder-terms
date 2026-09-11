#include "../../src/ftp/ftp-client.h"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace elder_terms_ftps_test {

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> run_async(
    elder_terms::FtpConnectionSettings connection, bool expect_success,
    cardio::dispatcher_group_glib &group, std::exception_ptr &failure) {
  std::shared_ptr<elder_terms::RemoteFileClient> client;
  bool succeeded = false;
  try {
    client = co_await elder_terms::open_ftp_client_async(
        {.connection = std::move(connection), .password = "secret"}, {});
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
          std::string_view(argv[argument + 1]) == "success", group, failure);
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

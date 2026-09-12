#include "webdav-client.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
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

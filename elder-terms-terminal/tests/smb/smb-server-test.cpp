// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/smb/smb-client.h"
#include "../../src/smb/smb-ssh-proxy.h"
#include "../../src/file-transfer/file-transfer-engine.h"
#include "../../src/file-transfer/activity-file-client.h"
#include <array>
#include <algorithm>
#include <iostream>

static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

struct Fixture {
  std::shared_ptr<elder_terms::SmbSshProxy> route;
  std::shared_ptr<elder_terms::RemoteFileClient> server;
};

static cardio::promise<void> exercise(Fixture &fixture, int argc, char **argv) {
  using namespace elder_terms;
  require(argc == 5, "Usage: smb-server-test host share user port");
  SmbClientOpenOptions options;
  options.connection.address = argv[1];
  options.connection.share = argv[2];
  options.connection.username = argv[3];
  options.connection.port = std::stoi(argv[4]);
  options.password = g_getenv("ELDER_SMB_PASSWORD");
  int prompts = 0;
  if (g_getenv("ELDER_SMB_PROXY_PORT")) {
    SshProxySettings proxy;
    proxy.enabled = true;
    proxy.endpoint.address = "127.0.0.1";
    proxy.endpoint.port = std::stoi(g_getenv("ELDER_SMB_PROXY_PORT"));
    proxy.endpoint.username = "gateway-user";
    TerminalSessionCallbacks callbacks{};
    callbacks.ssh_prompt = [&](const SshUserPrompt &prompt, cardio::cancellation) -> cardio::promise<SshUserPromptResponse> {
      ++prompts;
      co_return SshUserPromptResponse{.accepted = true, .text = prompt.initial_text};
    };
    fixture.route = co_await open_smb_ssh_proxy_async(proxy, "proxy-test.invalid", options.connection.port,
        callbacks, {.known_hosts_file = g_getenv("ELDER_SMB_PROXY_KNOWN_HOSTS"), .config_file = {}}, {});
    options.connection.address = "127.0.0.1";
    options.connection.port = smb_ssh_proxy_port(fixture.route);
  }
  auto server = co_await open_smb_server_async(options, {});
  fixture.server = server;
  auto client = create_activity_file_client(server, [](ActivityIndicatorId) {}, {});
  const std::string share = "/" + std::string(argv[2]);
  require(!client->path_capabilities("/").create_children && !client->path_capabilities("/").modify &&
      client->path_capabilities(share).create_children && !client->path_capabilities(share).modify &&
      client->path_capabilities(share + "/file").modify,
      "Activity wrappers must preserve root and share mutation restrictions");
  if (g_getenv("ELDER_SMB_DENY_ENUMERATION")) {
    bool denied = false;
    try { (void)co_await client->load_directory_async("/", {}); }
    catch (const RemoteFileError &error) { denied = !error.connection_lost; }
    require(denied, "Enumeration denial must be reported without disabling known shares");
  } else {
    const auto root = co_await client->load_directory_async("/", {});
    require(root.canonical_path == "/" && std::any_of(root.entries.begin(), root.entries.end(), [&](const auto &item) {
      return item.path == share && item.type == RemoteFileType::directory;
    }), "Server root must contain named shares, not their files");
  }
  const auto files = co_await client->load_directory_async(share, {});
  require(files.canonical_path == share, "Share listing lost the server path prefix");
  for (const auto &item : files.entries) require(item.path.starts_with(share + "/"), "Child path escaped its share");
  if (std::string(argv[2]) == "readonly") {
    bool denied = false;
    try { co_await client->make_directory_async(share + "/must-not-create", {}, {}); }
    catch (const RemoteFileError &error) { denied = !error.connection_lost; }
    require(denied, "Read-only access must remain a local share error");
  } else {
    const auto content = std::string(131079, 'x');
    const auto temporary = share + "/server-test.tmp";
    const auto destination = share + "/server-test.bin";
    auto writer = std::move(co_await client->open_write_async(temporary, content.size(), {}, {}));
    co_await writer->write_all_async(std::as_bytes(std::span(content).first(65536)), {});
    (void)co_await client->load_directory_async("/empty", {});
    co_await writer->write_all_async(std::as_bytes(std::span(content).subspan(65536)), {});
    co_await writer->close_async({});
    co_await client->commit_upload_async(temporary, destination, false, {});
    auto reader = std::move(co_await client->open_read_async(destination, {}));
    (void)co_await client->load_directory_async("/日本語 space", {});
    std::array<std::byte, 65536> bytes{};
    std::string actual;
    for (;;) {
      const auto count = co_await reader->read_async(bytes, {});
      if (!count) break;
      actual.append(reinterpret_cast<const char *>(bytes.data()), count);
    }
    co_await reader->close_async({});
    require(actual == content, "Browsing another share changed the open file session");
    bool cross_share = false;
    try { co_await client->rename_async(destination, "/empty/changed.bin", {}); }
    catch (const RemoteFileError &error) { cross_share = !error.connection_lost; }
    require(cross_share && (co_await client->lstat_async(destination, {})).has_value(), "Cross-share rename moved or removed its source");
    co_await client->remove_file_async(destination, {});
    // The engine must reject a share before descending into its contents.
    auto protected_writer = std::move(co_await client->open_write_async("/empty/protected.txt", 4, {}, {}));
    const std::string sentinel = "keep";
    co_await protected_writer->write_all_async(std::as_bytes(std::span(sentinel)), {});
    co_await protected_writer->close_async({});
    bool protected_share = false;
    try { co_await delete_file_transfer_items_async(client, FileTransferEndpoint::remote, {"/empty"}, {}); }
    catch (const RemoteFileError &error) { protected_share = !error.connection_lost; }
    require(protected_share && (co_await client->lstat_async("/empty/protected.txt", {})).has_value(),
        "Deleting a protected share removed its contents before rejection");
    co_await client->remove_file_async("/empty/protected.txt", {});
  }
  bool denied_share = false;
  try { (void)co_await client->load_directory_async("/denied", {}); }
  catch (const RemoteFileError &error) { denied_share = !error.connection_lost; }
  require(denied_share, "An inaccessible share must not disconnect the server browser");
  (void)co_await client->load_directory_async(share, {});
  require(!fixture.route || prompts == 2, "Multiple shares must authenticate the SSH gateway only once");
  co_await stop_smb_client_async(server);
  bool stopped = false;
  try { (void)co_await client->load_directory_async(share, {}); }
  catch (const RemoteFileError &error) { stopped = error.connection_lost; }
  require(stopped, "Server shutdown left a share usable");
  std::cout << "PASS SMB client server shares/paths/independent streams/permissions/delete/cleanup\n";
}

static cardio::promise<void> run(cardio::dispatcher_group_glib &group, int argc, char **argv, std::exception_ptr &failure) {
  Fixture fixture;
  try { co_await exercise(fixture, argc, argv); } catch (...) { failure = std::current_exception(); }
  co_await elder_terms::stop_smb_client_async(fixture.server);
  co_await elder_terms::stop_smb_ssh_proxy_async(fixture.route);
  group.shutdown();
}

int main(int argc, char **argv) {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  std::exception_ptr failure;
  auto task = run(group, argc, argv, failure);
  dispatcher.park();
  if (!failure) return 0;
  try { std::rethrow_exception(failure); } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  return 1;
}

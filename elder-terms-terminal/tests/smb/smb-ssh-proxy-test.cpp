// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/smb/smb-client.h"
#include "../../src/smb/smb-ssh-proxy.h"
#include <array>
#include <iostream>

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> exercise_async(int argc, char **argv) {
  using namespace elder_terms;
  require(argc == 5, "Usage: smb-ssh-proxy-test host share user port");
  const std::string mode = g_getenv("ELDER_SMB_PROXY_MODE") ? g_getenv("ELDER_SMB_PROXY_MODE") : "transfer";
  SshProxySettings proxy;
  proxy.enabled = true;
  proxy.endpoint.address = "127.0.0.1";
  proxy.endpoint.port = std::stoi(g_getenv("ELDER_SMB_PROXY_PORT"));
  proxy.endpoint.username = "gateway-user";
  int prompts = 0;
  TerminalSessionCallbacks callbacks{};
  callbacks.ssh_prompt = [&](const SshUserPrompt &prompt, cardio::cancellation)
      -> cardio::promise<SshUserPromptResponse> {
    require(prompt.title.starts_with("SSH proxy"), "Gateway prompt was not identified");
    ++prompts;
    co_return SshUserPromptResponse{.accepted = true, .text = prompt.initial_text};
  };
  cardio::cancellation_source canceled;
  (void)canceled.cancel();
  bool was_canceled = false;
  try { (void)co_await open_smb_ssh_proxy_async(proxy, "proxy-test.invalid", std::stoi(argv[4]),
      callbacks, {}, canceled.get_cancellation()); }
  catch (const cardio::canceled_exception &) { was_canceled = true; }
  require(was_canceled && prompts == 0, "Canceled route started SSH authentication");
  auto opening = open_smb_ssh_proxy_async(proxy, "proxy-test.invalid", std::stoi(argv[4]),
      callbacks, {.known_hosts_file = g_getenv("ELDER_SMB_PROXY_KNOWN_HOSTS"), .config_file = {}}, {});
  auto route = std::move(co_await opening);
  require(route && smb_ssh_proxy_port(route), "Enabled SMB SSH proxy must open a route");
  require(prompts == 2, "Gateway user name and host key must be confirmed once");
  if (mode == "pending") {
    co_await stop_smb_ssh_proxy_async(route);
    co_await stop_smb_ssh_proxy_async(route);
    std::cout << "PASS SMB client SSH proxy cancellation before SMB connection\n";
    co_return;
  }
  std::shared_ptr<RemoteFileClient> client;
  std::exception_ptr failure;
  try {
    SmbClientOpenOptions options;
    options.connection.address = "127.0.0.1";
    options.connection.port = smb_ssh_proxy_port(route);
    options.connection.share = argv[2];
    options.connection.username = argv[3];
    options.connection.require_encryption = std::string(argv[2]) == "encrypted" || mode == "encrypted";
    options.password = g_getenv("ELDER_SMB_PASSWORD");
    client = co_await open_smb_client_async(std::move(options), {});
    const auto listing = co_await client->load_directory_async("/", {});
    require(listing.canonical_path == "/", "Proxy share root differs");
    if (std::string(argv[2]) == "readonly" || mode == "readonly") {
      bool refused = false;
      try { co_await client->make_directory_async("/proxy-readonly", {}, {}); }
      catch (const RemoteFileError &error) { refused = !error.connection_lost; }
      require(refused, "Proxy bypassed read-only share permissions");
    } else {
      auto *uuid = g_uuid_string_random();
      const std::string directory = "/proxy-" + std::string(uuid);
      g_free(uuid);
      co_await client->make_directory_async(directory, {}, {});
      const std::string original = directory + "/日本語 space.tmp";
      const std::string renamed = directory + "/renamed.bin";
      std::string content(1024 * 1024 + 13, '\0');
      for (std::size_t index = 0; index < content.size(); ++index)
        content[index] = static_cast<char>((index * 31 + 7) % 256);
      auto writer = std::move(co_await client->open_write_async(original, content.size(), {}, {}));
      co_await writer->write_all_async(std::as_bytes(std::span(content)), {});
      co_await writer->close_async({});
      co_await client->commit_upload_async(original, renamed, false, {});
      auto reader = std::move(co_await client->open_read_async(renamed, {}));
      std::array<std::byte, 65536> buffer{};
      std::string actual;
      for (;;) {
        const auto count = co_await reader->read_async(buffer, {});
        if (!count) break;
        actual.append(reinterpret_cast<const char *>(buffer.data()), count);
      }
      co_await reader->close_async({});
      require(actual == content, "Proxy round-trip bytes differ");
      co_await client->remove_file_async(renamed, {});
      co_await client->remove_directory_async(directory, {});
    }
    // Closing the SSH route must invalidate the established SMB session.
    co_await stop_smb_ssh_proxy_async(route);
    bool disconnected = false;
    try { (void)co_await client->load_directory_async("/", {}); }
    catch (const RemoteFileError &error) { disconnected = error.connection_lost; }
    require(disconnected, "Closed SSH route accepted an SMB operation");
  } catch (...) { failure = std::current_exception(); }
  co_await stop_smb_client_async(client);
  co_await stop_smb_ssh_proxy_async(route);
  co_await stop_smb_ssh_proxy_async(route);
  if (failure) std::rethrow_exception(failure);
  proxy.enabled = false;
  require(!co_await open_smb_ssh_proxy_async(proxy, "", 0, {}, {}, {}), "Disabled proxy opened a route");
  proxy.validation_errors.push_back("Invalid proxy setting");
  bool rejected = false;
  try { (void)co_await open_smb_ssh_proxy_async(proxy, "", 0, {}, {}, {}); }
  catch (const std::invalid_argument &) { rejected = true; }
  require(rejected, "Invalid proxy settings were ignored");
  std::cout << "PASS SMB client SSH proxy listing/round-trip/commit/permissions/disconnect/cleanup\n";
}

static cardio::promise<void> run_async(cardio::dispatcher_group_glib &group,
    int argc, char **argv, std::exception_ptr &failure) {
  try { co_await exercise_async(argc, argv); }
  catch (...) { failure = std::current_exception(); }
  group.shutdown();
}

int main(int argc, char **argv) {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  std::exception_ptr failure;
  auto pending = run_async(group, argc, argv, failure);
  dispatcher.park();
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
    return 1;
  }
  return 0;
}

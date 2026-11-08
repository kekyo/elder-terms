// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/nfs/nfs-ssh-proxy.h"
#include <array>
#include <iostream>

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> exercise(char **argv) {
  using namespace elder_terms;
  const int version = std::stoi(argv[1]);
  const bool fixed = std::string_view(argv[2]) == "fixed";
  SshProxySettings settings{.enabled = true,
      .endpoint = {.address = "127.0.0.1", .port = std::stoi(g_getenv("ELDER_NFS_PROXY_PORT")),
          .username = "gateway-user", .identity_file = {}}};
  int prompts = 0;
  TerminalSessionCallbacks callbacks{};
  callbacks.ssh_prompt = [&](const SshUserPrompt &prompt, cardio::cancellation) -> cardio::promise<SshUserPromptResponse> {
    require(prompt.title.starts_with("SSH proxy"), "Identify gateway prompts");
    ++prompts;
    co_return SshUserPromptResponse{.accepted = true, .text = prompt.initial_text};
  };
  auto proxy = co_await open_ssh_tcp_proxy_async(settings, callbacks,
      {.known_hosts_file = g_getenv("ELDER_NFS_PROXY_KNOWN_HOSTS"), .config_file = {}}, {});
  NfsClientOpenOptions options;
  options.address = "proxy-test.invalid";
  options.version = version;
  options.filename_encoding = "CP932";
  options.uid = 1000;
  options.gid = 1000;
  if (fixed) { options.nfs_port = 22049; options.mount_port = 22048; }
  options.require_reserved_port = true;
  bool rejected = false;
  try { (void)co_await forward_nfs_ssh_ports_async(proxy, options, {}); }
  catch (const RemoteFileError &error) { rejected = std::string_view(error.what()).find("reserved") != std::string_view::npos; }
  require(rejected, "Reject a reserved-port requirement before creating NFS routes");
  options.require_reserved_port = false;
  cardio::cancellation_source canceled;
  (void)canceled.cancel();
  rejected = false;
  try { (void)co_await forward_nfs_ssh_ports_async(proxy, options, canceled.get_cancellation()); }
  catch (const cardio::canceled_exception &) { rejected = true; }
  require(rejected, "Canceled NFS setup must not open forwarding channels");
  if (std::string_view(argv[2]) == "invalid-mapping") {
    rejected = false;
    try { (void)co_await forward_nfs_ssh_ports_async(proxy, options, {}); }
    catch (const RemoteFileError &error) { rejected = std::string_view(error.what()).find("no valid TCP port") != std::string_view::npos; }
    require(rejected, "Invalid service ports must not produce a forwarding endpoint");
    co_await stop_ssh_tcp_proxy_async(proxy);
    std::cout << "PASS invalid RPC port mapping\n";
    co_return;
  }
  options = co_await forward_nfs_ssh_ports_async(proxy, options, {});
  require(options.address == "127.0.0.1" && options.nfs_port > 0 && (version == 4 || options.mount_port > 0),
      "Every NFS connection must use an explicit forwarded endpoint");
  if (std::string_view(argv[2]) == "ipv6-mapping") {
    require(prompts == 2, "RPC discovery must share gateway authentication");
    co_await stop_ssh_tcp_proxy_async(proxy);
    std::cout << "PASS IPv6-only RPC port mapping\n";
    co_return;
  }
  auto client = co_await open_nfs_server_async(options, {});
  const std::string prefix = version == 4 ? "" : "/srv";
  const auto root = nfs_export_identity(client, prefix + "/public");
  const auto readonly = nfs_export_identity(client, prefix + "/readonly");
  const auto entries = co_await client->load_directory_async(root, {});
  require(entries.entries.size() >= 8, "Discover exports and list the public directory through SSH");
  require((co_await client->load_directory_async(readonly, {})).canonical_path == readonly,
      "Browse a second export through the same gateway");
  rejected = false;
  try { co_await client->make_directory_async(client->child_path(readonly, "proxy-denied"), {}, {}); }
  catch (const RemoteFileError &) { rejected = true; }
  require(rejected, "SSH must preserve NFS read-only permissions");
  const auto directory = client->child_path(root, "proxy-日本語");
  co_await client->make_directory_async(directory, {}, {});
  const auto temporary = client->child_path(directory, "pending.bin");
  const auto uploaded = client->child_path(directory, "uploaded.bin");
  const std::string content(65539, 'p');
  auto writer = std::move(co_await client->open_write_async(temporary, content.size(), {}, {}));
  co_await writer->write_all_async(std::as_bytes(std::span(content)), {});
  co_await writer->close_async({});
  co_await client->commit_upload_async(temporary, uploaded, false, {});
  auto reader = std::move(co_await client->open_read_async(uploaded, {}));
  std::array<std::byte, 4096> buffer{};
  std::string received;
  for (;;) {
    const auto count = co_await reader->read_async(buffer, {});
    if (!count) break;
    received.append(reinterpret_cast<const char *>(buffer.data()), count);
  }
  co_await reader->close_async({});
  require(received == content, "NFS over SSH must preserve transferred bytes");
  co_await client->remove_file_async(uploaded, {});
  co_await client->remove_directory_async(directory, {});
  require(!(co_await client->lstat_async(directory, {})), "NFS deletion must reach the server through SSH");
  require(prompts == 2, "Discovery and multiple exports must authenticate to SSH only once");
  co_await stop_ssh_tcp_proxy_async(proxy);
  rejected = false;
  try { (void)co_await client->load_directory_async(root, {}); }
  catch (const RemoteFileError &error) { rejected = error.connection_lost; }
  require(rejected, "A closed proxy must not fall back to direct NFS");
  co_await stop_nfs_client_async(client);
  co_await stop_ssh_tcp_proxy_async(proxy);
  std::cout << "PASS NFS SSH proxy " << version << ' ' << argv[2] << " discovery, files, permissions and shutdown\n";
}

static cardio::promise<void> run(cardio::dispatcher_group_glib &group, char **argv, std::exception_ptr &failure) {
  try { co_await exercise(argv); } catch (...) { failure = std::current_exception(); }
  group.shutdown();
}

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  std::exception_ptr failure;
  auto pending = run(group, argv, failure);
  dispatcher.park();
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
    return 1;
  }
}

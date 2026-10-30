// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/smb/smb-ssh-proxy.h"
#include "../../src/smb/smb-client.h"
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-share-enum.h>
#include <poll.h>
#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>

static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

struct Response {
  bool done = false;
  int status = 0;
  std::string error;
  unsigned callbacks = 0;
  std::function<void(smb2_context *, int, void *)> consume;
  std::exception_ptr failure;
};

static void completed(smb2_context *context, int status, void *data, void *opaque) {
  auto &response = *static_cast<Response *>(opaque);
  response.status = status;
  response.error = smb2_get_error(context);
  ++response.callbacks;
  try { if (response.consume) response.consume(context, status, data); }
  catch (...) { response.failure = std::current_exception(); }
  response.done = true;
}

static cardio::promise<Response> execute(smb2_context *&context,
    std::function<int(smb2_command_cb, void *)> start,
    std::function<void(smb2_context *, int, void *)> consume,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  Response response;
  response.consume = std::move(consume);
  const int started = start(completed, &response);
  require(started == 0, "Cannot start SMB probe request");
  auto timeout = cardio::cancellations::timeout(10000);
  auto stop = cardio::cancellations::any(cancellation, timeout.get_cancellation());
  try {
    while (!response.done) {
      const int wanted = smb2_which_events(context);
      auto events = cardio::fd_event::none;
      if (wanted & POLLIN) events |= cardio::fd_event::read;
      if (wanted & POLLOUT) events |= cardio::fd_event::write;
      const auto ready = co_await cardio::from_fd(smb2_get_fd(context), events, stop.get_cancellation());
      int occurred = 0;
      if ((ready & cardio::fd_event::read) != cardio::fd_event::none) occurred |= POLLIN;
      if ((ready & cardio::fd_event::write) != cardio::fd_event::none) occurred |= POLLOUT;
      if ((ready & cardio::fd_event::error) != cardio::fd_event::none) occurred |= POLLERR;
      if ((ready & cardio::fd_event::hangup) != cardio::fd_event::none) occurred |= POLLHUP;
      if (smb2_service(context, occurred) < 0 && !response.done) throw std::runtime_error(smb2_get_error(context));
    }
  } catch (...) {
    timeout.cancel();
    stop.cancel();
    smb2_destroy_context(context);
    context = nullptr;
    require(response.callbacks == 1, "Pending enumeration callback did not retire exactly once");
    throw;
  }
  timeout.cancel();
  stop.cancel();
  if (response.failure) std::rethrow_exception(response.failure);
  co_return response;
}

static cardio::promise<void> probe(int argc, char **argv) {
  using namespace elder_terms;
  require(argc == 5, "Usage: smb-share-probe host expected-share user port");
  std::string host = argv[1];
  int port = std::stoi(argv[4]);
  std::shared_ptr<SmbSshProxy> route;
  if (g_getenv("ELDER_SMB_PROXY_PORT")) {
    SshProxySettings settings;
    settings.enabled = true;
    settings.endpoint.address = "127.0.0.1";
    settings.endpoint.port = std::stoi(g_getenv("ELDER_SMB_PROXY_PORT"));
    settings.endpoint.username = "gateway-user";
    TerminalSessionCallbacks callbacks{};
    callbacks.ssh_prompt = [](const SshUserPrompt &prompt, cardio::cancellation) -> cardio::promise<SshUserPromptResponse> {
      co_return SshUserPromptResponse{.accepted = true, .text = prompt.initial_text};
    };
    route = co_await open_smb_ssh_proxy_async(settings, "proxy-test.invalid", port, callbacks,
        {.known_hosts_file = g_getenv("ELDER_SMB_PROXY_KNOWN_HOSTS"), .config_file = {}}, {});
    host = "127.0.0.1";
    port = smb_ssh_proxy_port(route);
  }
  auto *context = smb2_init_context();
  require(context, "No SMB context");
  std::exception_ptr failure;
  try {
    smb2_set_user(context, argv[3]);
    smb2_set_password(context, g_getenv("ELDER_SMB_PASSWORD"));
    auto *url = smb2_parse_url(context, "smb://localhost/IPC$?sec=ntlmssp");
    require(url, "NTLM selection failed");
    smb2_destroy_url(url);
    smb2_set_sign(context, 1);
    smb2_set_security_mode(context, SMB2_NEGOTIATE_SIGNING_ENABLED | SMB2_NEGOTIATE_SIGNING_REQUIRED);
    const auto endpoint = host + ":" + std::to_string(port);
    const auto connected = co_await execute(context, [&](auto cb, auto data) {
      return smb2_connect_share_async(context, endpoint.c_str(), "IPC$", argv[3], cb, data);
    }, {}, {});
    if (g_getenv("ELDER_SMB_DENY_ENUMERATION")) {
      require(connected.status != 0, "IPC enumeration was not denied");
      SmbClientOpenOptions options;
      options.connection.address = argv[1];
      options.connection.port = std::stoi(argv[4]);

      options.connection.username = argv[3];
      options.password = g_getenv("ELDER_SMB_PASSWORD");
      auto client = co_await open_smb_client_async(options, argv[2], {});
      const auto files = co_await client->load_directory_async("/", {});
      require(!files.entries.empty(), "Known share could not be browsed after IPC denial");
      co_await stop_smb_client_async(client);
      smb2_destroy_context(context);
      context = nullptr;
      co_await stop_smb_ssh_proxy_async(route);
      std::cout << "PASS SMB client enumeration denial with direct known-share browsing\n";
      co_return;
    }
    if (connected.status != 0) throw std::runtime_error(connected.error);
    std::vector<std::string> shares;
    const auto consume = [&](smb2_context *smb, int status, void *data) {
      if (!data) return;
      auto *reply = static_cast<smb2_share_enum_reply *>(data);
      auto release = [smb](smb2_share_enum_reply *value) { smb2_free_data(smb, value); };
      const std::unique_ptr<smb2_share_enum_reply, decltype(release)> owned(reply, release);
      require(status == 0 && reply->entries_read == reply->total_entries, "Enumeration failed or returned a partial list");
      require(reply->level == SMB2_SHARE_INFO_1, "Unexpected enumeration level");
      for (uint32_t index = 0; index < reply->entries_read; ++index) {
        const auto &share = reply->share_info.info_1[index];
        if ((share.type & 3U) == SMB2_SHARE_TYPE_DISKTREE) shares.emplace_back(share.netname);
      }
    };
    const auto enumeration = co_await execute(context, [&](auto cb, auto data) {
      return smb2_share_enum_async(context, SMB2_SHARE_INFO_1, cb, data);
    }, consume, {});
    if (enumeration.status != 0) throw std::runtime_error("Share enumeration status=" + std::to_string(enumeration.status) + ": " + enumeration.error);
    require(std::find(shares.begin(), shares.end(), argv[2]) != shares.end(), "Expected disk share is absent");
    if (const auto *expected = g_getenv("ELDER_SMB_EXPECT_SHARES")) {
      std::istringstream names(expected);
      for (std::string name; std::getline(names, name);)
        require(std::find(shares.begin(), shares.end(), name) != shares.end(), "Expected fixture share is absent");
    }
    require(std::find(shares.begin(), shares.end(), "IPC$") == shares.end(), "IPC share leaked into disk list");
    // Start another real request and cancel its descriptor wait before it replies.
    cardio::cancellation_source cancellation;
    auto pending = execute(context, [&](auto cb, auto data) {
      return smb2_share_enum_async(context, SMB2_SHARE_INFO_1, cb, data);
    }, consume, cancellation.get_cancellation());
    cancellation.cancel();
    bool canceled = false;
    try { (void)co_await pending; } catch (const cardio::canceled_exception &) { canceled = true; }
    require(canceled && context == nullptr, "Enumeration cancellation kept callbacks alive");
    std::cout << "PASS SMB client share enumeration disks=" << shares.size() << " cancellation/cleanup\n";
  } catch (...) { failure = std::current_exception(); }
  if (context) smb2_destroy_context(context);
  co_await stop_smb_ssh_proxy_async(route);
  if (failure) std::rethrow_exception(failure);
}

static cardio::promise<void> run(cardio::dispatcher_group_glib &group, int argc, char **argv, std::exception_ptr &failure) {
  try { co_await probe(argc, argv); } catch (...) { failure = std::current_exception(); }
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

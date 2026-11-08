// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/
#include "http-client.h"
#include "../../src/file-transfer/file-transfer-engine.h"
#include <elder-terms/settings.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

static void expect(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
static cardio::promise<void> run_async(char **args, cardio::dispatcher_group_glib &group, std::exception_ptr &failure) {
  using namespace elder_terms;
  std::shared_ptr<RemoteFileClient> client;
  std::shared_ptr<SshSocksProxy> proxy;
  try {
    auto store = create_default_settings({}, "HTTP integration");
    expect(set_explicit_setting_value(&store, http_setting_key("base_url"), std::string(args[1])), "Valid base URL required");
    const auto settings = http_connection_settings(store);
    HttpClientOpenOptions options;
    options.base_url = settings.base_url;
    options.transport.scheme = settings.scheme;
    options.transport.address = settings.address;
    options.transport.port = settings.port;
    options.transport.ca_file = args[3];
    options.transport.prompt_certificate = std::string(args[4]) != "reject";
    options.transport.authentication = std::string(args[6]) == "none" ? HttpAuthentication::none : HttpAuthentication::automatic;
    options.transport.username = "alice";
    options.password = args[6];
    unsigned confirmations = 0;
    options.confirm_certificate = [&confirmations, args](const TlsCertificateFailure &certificate, cardio::cancellation) -> cardio::promise<bool> {
      ++confirmations;
      expect(!certificate.sha256.empty(), "Certificate decision must identify the peer");
      co_return std::string(args[4]) == "allow";
    };
    if (const auto *port = std::getenv("ELDER_TERMS_TEST_PROXY_PORT")) {
      TerminalSessionCallbacks callbacks{};
      callbacks.ssh_prompt = [](const auto &prompt, cardio::cancellation) -> cardio::promise<SshUserPromptResponse> {
        co_return SshUserPromptResponse{.accepted = true, .text = prompt.initial_text};
      };
      SshProxySettings route{.enabled = true,
          .endpoint = {.address = "127.0.0.1", .port = std::stoll(port), .username = "gateway-test", .identity_file = {}}};
      auto opening = open_ssh_socks_proxy_async(route, settings.address, callbacks,
          {.known_hosts_file = std::getenv("ELDER_TERMS_TEST_PROXY_KNOWN_HOSTS"), .config_file = {}}, {});
      proxy = std::move(co_await opening);
      options.proxy = proxy;
    }
    bool failed = false;
    try { client = co_await open_http_client_async(options, {}); }
    catch (const std::exception &error) { std::cout << error.what() << '\n'; failed = true; }
    expect(failed == (std::string(args[5]) == "failure"), "TLS/authentication result must match the configured policy");
    if (!failed) {
      FileTransferRequest request;
      request.direction = FileTransferDirection::receive;
      request.source_paths = {settings.base_url + "tree/"};
      request.destination_directory = args[2];
      const auto result = co_await run_file_transfer_async(client, request, {});
      expect(result.outcome == FileTransferOutcome::completed, "Selected subtree must complete");
      std::ifstream file(std::filesystem::path(args[2]) / "tree" / "file.txt");
      const std::string content{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
      expect(content == "HTTP integration bytes\n", "The downloaded bytes must match the independently served file");
    }
    const auto expected = std::string(args[4]) == "reject" ? 0U : 1U;
    expect(confirmations == expected, "Certificate exceptions must be reused only within this connection");
  } catch (...) { failure = std::current_exception(); }
  if (client) co_await stop_http_client_async(client);
  co_await stop_ssh_socks_proxy_async(proxy);
  group.shutdown();
}
int main(int argc, char **argv) {
  try {
    expect(argc == 7, "Expected URL, destination, CA, certificate policy, outcome and password");
    std::exception_ptr failure;
    {
      cardio::dispatcher_group_glib group;
      cardio::dispatcher_host_glib_auto dispatcher(group);
      auto pending = run_async(argv, group, failure);
      dispatcher.park();
    }
    if (failure) std::rethrow_exception(failure);
    std::cout << "HTTP integration: PASS\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

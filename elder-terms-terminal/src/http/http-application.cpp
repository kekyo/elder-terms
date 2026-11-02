// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "http-application.h"
#include "http-client.h"
#include "../file-transfer/file-transfer-window.h"
#include "../file-transfer/file-transfer-paths.h"
#include "../file-transfer/file-transfer-certificate-prompt.h"
#include <elder-terms/connection-desktop-session.h>

#include <iostream>
#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

struct HttpApplication {
  cardio::dispatcher_group_glib *dispatcher_group;
  HttpConnectionSettings connection;
  SshProxySettings proxy;
  std::shared_ptr<SshSocksProxy> route;
  std::shared_ptr<RemoteFileClient> client;
  std::shared_ptr<FileTransferWindow> window;
  cardio::cancellation_source stopping;
  std::optional<cardio::promise<void>> startup;
  std::optional<cardio::promise<void>> shutdown;
  bool shutting_down = false;
};

static cardio::promise<void> finish_application_async(HttpApplication *state) {
  try {
    if (state->startup) co_await *state->startup;
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  auto closing = close_file_transfer_window_async(state->window);
  try {
    if (state->client) co_await stop_http_client_async(state->client);
    co_await stop_ssh_socks_proxy_async(state->route);
    co_await closing;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  shutdown_connection_desktop_session();
  state->dispatcher_group->shutdown();
}

static void stop_application(HttpApplication *state) {
  if (state->shutting_down) return;
  state->shutting_down = true;
  (void)state->stopping.cancel();
  state->shutdown.emplace(finish_application_async(state));
}

static cardio::promise<bool> confirm_http_certificate_async(
    HttpApplication *state, const TlsCertificateFailure &failure,
    cardio::cancellation cancellation) {
  auto pending = confirm_file_transfer_certificate_async(
      state->window, failure, _("HTTPS certificate validation failed"),
      _("HTTPS connection"), cancellation);
  const bool accepted = co_await pending;
  if (!accepted || cancellation.is_cancellation_requested() || state->shutting_down) {
    stop_application(state);
    co_return false;
  }
  mark_file_transfer_certificate_exception(state->window);
  co_return true;
}

static cardio::promise<void> start_application_async(HttpApplication *state) {
  std::string failure;
  const auto cancellation = state->stopping.get_cancellation();
  try {
    if (!state->proxy.validation_errors.empty()) throw std::invalid_argument(state->proxy.validation_errors.front());
    HttpClientOpenOptions options{
        .transport = {.scheme = state->connection.scheme, .address = state->connection.address,
            .port = state->connection.port, .authentication = state->connection.authentication,
            .username = state->connection.username, .ca_file = state->connection.ca_file,
            .prompt_certificate = state->connection.prompt_certificate,
            .connect_timeout_seconds = state->connection.connect_timeout_seconds,
            .idle_timeout_seconds = state->connection.idle_timeout_seconds,
            .validation_errors = state->connection.validation_errors},
        .base_url = state->connection.base_url, .start_url = state->connection.start_url,
        .limits = {}, .password = {},
        .confirm_certificate = [state](const TlsCertificateFailure &failure, cardio::cancellation cancellation) {
          return confirm_http_certificate_async(state, failure, cancellation);
        }, .proxy = {}};
    TerminalSessionCallbacks gateway_callbacks{};
    gateway_callbacks.ssh_prompt = [state](const SshUserPrompt &prompt, cardio::cancellation signal) {
      return prompt_file_transfer_ssh_async(state->window, prompt, signal);
    };
    auto opening_proxy = open_ssh_socks_proxy_async(state->proxy, options.transport.address,
        std::move(gateway_callbacks), {.known_hosts_file = {}, .config_file = {}}, cancellation);
    state->route = std::move(co_await opening_proxy);
    options.proxy = state->route;
    if (options.transport.authentication != HttpAuthentication::none) {
      std::string username = options.transport.username;
      if (username.empty() && g_get_user_name()) username = g_get_user_name();
      bool missing = false;
      while (true) {
        auto *formatted = g_strdup_printf(_("User name for %s:"), options.transport.address.c_str());
        std::string message(formatted);
        g_free(formatted);
        if (missing) message += "\n\n" + std::string(_("User name must not be empty."));
        InlinePromptRequest request{
            .title = _("HTTP authentication"), .message = std::move(message),
            .accept_label = _("Connect"), .cancel_label = _("Cancel"),
            .initial_text = username, .input_label = _("User name"),
            .input_required = true, .echo = true, .cancel_visible = true};
        auto response = co_await prompt_file_transfer_window_async(state->window, std::move(request), cancellation);
        if (!response.accepted) { stop_application(state); co_return; }
        username = std::move(response.text);
        if (username.find_first_not_of(" \t\r\n") != std::string::npos) {
          break;
        }
        missing = true;
      }
      InlinePromptRequest password_request{
          .title = _("HTTP authentication"), .message = _("Password:"),
          .accept_label = _("Connect"), .cancel_label = _("Cancel"),
          .input_required = true, .echo = false, .cancel_visible = true};
      auto password_response = co_await prompt_file_transfer_window_async(
          state->window, std::move(password_request), cancellation);
      if (!password_response.accepted) { stop_application(state); co_return; }
      options.transport.username = std::move(username);
      options.password = std::move(password_response.text);
    }
    state->client = co_await open_http_client_async(std::move(options), cancellation);
    attach_file_transfer_window_client(state->window, state->client);
    co_return;
  } catch (const cardio::canceled_exception &) {
    stop_application(state);
    co_return;
  } catch (const std::exception &error) { failure = error.what(); }
  co_await stop_ssh_socks_proxy_async(state->route);
  if (!state->shutting_down) {
    std::cerr << failure << '\n';
    co_await show_file_transfer_window_connection_error_async(
        state->window, _("Failed to start HTTP"), std::move(failure), cancellation);
  }
}

static cardio::promise<void> reconnect_application_async(
    HttpApplication *state, SettingsStore settings) {
  if (state->startup) co_await *state->startup;
  if (state->client) co_await stop_http_client_async(state->client);
  co_await stop_ssh_socks_proxy_async(state->route);
  state->route.reset();
  state->client.reset();
  if (state->shutting_down) co_return;
  state->connection = http_connection_settings(settings);
  state->proxy = ssh_proxy_connection_settings(settings);
  co_await start_application_async(state);
}

int run_http_application(const SettingsLoadResult &settings,
    std::optional<std::filesystem::path> config_path) {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  HttpApplication state;
  state.dispatcher_group = &group;
  state.connection = http_connection_settings(settings.store);
  state.proxy = ssh_proxy_connection_settings(settings.store);
  state.window = create_file_transfer_window({
      .connection_name = general_connection_name(settings.store),
      .protocol_name = state.connection.scheme == "https" ? "HTTPS" : "HTTP",
      .local_directory = resolve_file_transfer_local_directory(settings.store, state.connection.local_directory),
      .remote_directory = state.connection.start_url, .remote_file_hash = {},
      .colors = general_color_settings(settings.store),
      .closed = [&state] { stop_application(&state); },
      .settings = settings.store, .config_path = std::move(config_path),
      .reconnect = [&state](SettingsStore settings) {
        return reconnect_application_async(&state, std::move(settings));
      }});
  show_file_transfer_window(state.window);
  state.startup.emplace(start_application_async(&state));
  dispatcher.park();
  return 0;
}

} // namespace elder_terms

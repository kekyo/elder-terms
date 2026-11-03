// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "smb-application.h"
#include "smb-client.h"
#include "smb-ssh-proxy.h"
#include "../file-transfer/file-transfer-window.h"
#include "../file-transfer/file-transfer-paths.h"
#include <elder-terms/connection-desktop-session.h>

#include <iostream>
#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

struct SmbApplication {
  cardio::dispatcher_group_glib *dispatcher_group;
  SmbConnectionSettings connection;
  SshProxySettings proxy;
  std::shared_ptr<SmbSshProxy> route;
  std::string ssh_known_hosts_file;
  std::shared_ptr<RemoteFileClient> client;
  std::shared_ptr<FileTransferWindow> window;
  cardio::cancellation_source stopping;
  std::optional<cardio::promise<void>> startup;
  std::optional<cardio::promise<void>> shutdown;
  bool shutting_down = false;
};

static cardio::promise<void> finish_application_async(SmbApplication *state) {
  try {
    if (state->startup) co_await *state->startup;
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  auto closing = close_file_transfer_window_async(state->window);
  try {
    if (state->client) co_await stop_smb_client_async(state->client);
    co_await stop_smb_ssh_proxy_async(state->route);
    co_await closing;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  shutdown_connection_desktop_session();
  state->dispatcher_group->shutdown();
}

static void stop_application(SmbApplication *state) {
  if (state->shutting_down) return;
  state->shutting_down = true;
  (void)state->stopping.cancel();
  state->shutdown.emplace(finish_application_async(state));
}

static cardio::promise<void> start_application_async(SmbApplication *state) {
  std::string failure;
  const auto cancellation = state->stopping.get_cancellation();
  try {
    if (!state->connection.validation_errors.empty())
      throw std::invalid_argument(state->connection.validation_errors.front());
    SmbClientOpenOptions options{.connection = state->connection, .password = {}};
    options.disconnected = [window = std::weak_ptr<FileTransferWindow>(state->window)] {
      if (auto current = window.lock()) set_file_transfer_window_connection_available(current, false);
    };
    TerminalSessionCallbacks gateway_callbacks{};
    gateway_callbacks.ssh_prompt = [state](const SshUserPrompt &prompt, cardio::cancellation signal) {
      return prompt_file_transfer_ssh_async(state->window, prompt, signal);
    };
    auto opening_proxy = open_smb_ssh_proxy_async(state->proxy, state->connection.address,
        static_cast<std::uint16_t>(state->connection.port), std::move(gateway_callbacks),
        {.known_hosts_file = state->ssh_known_hosts_file, .config_file = {}}, cancellation);
    state->route = std::move(co_await opening_proxy);
    {
      std::string username = options.connection.username;
      if (username.empty() && g_get_user_name()) username = g_get_user_name();
      bool missing = false;
      while (true) {
        auto *formatted = g_strdup_printf(_("User name for %s:"), options.connection.address.c_str());
        std::string message(formatted);
        g_free(formatted);
        if (missing) message += "\n\n" + std::string(_("User name must not be empty."));
        InlinePromptRequest request{
            .title = _("SMB authentication"), .message = std::move(message),
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
          .title = _("SMB authentication"), .message = _("Password:"),
          .accept_label = _("Connect"), .cancel_label = _("Cancel"),
          .input_required = true, .echo = false, .cancel_visible = true};
      auto password_response = co_await prompt_file_transfer_window_async(
          state->window, std::move(password_request), cancellation);
      if (!password_response.accepted) { stop_application(state); co_return; }
      options.connection.username = std::move(username);
      options.password = std::move(password_response.text);
    }
    if (state->route) {
      // Keep the saved endpoint and authentication prompts addressed to the
      // real server; all libsmb2 share connections use the local relay.
      options.connection.address = "127.0.0.1";
      options.connection.port = smb_ssh_proxy_port(state->route);
    }
    state->client = co_await open_smb_server_async(std::move(options), cancellation);
    attach_file_transfer_window_client(state->window, state->client);
    co_return;
  } catch (const cardio::canceled_exception &) {
    stop_application(state);
    co_return;
  } catch (const std::exception &error) { failure = error.what(); }
  co_await stop_smb_ssh_proxy_async(state->route);
  if (!state->shutting_down) {
    std::cerr << failure << '\n';
    co_await show_file_transfer_window_connection_error_async(
        state->window, _("Failed to start SMB"), std::move(failure), cancellation);
  }
}

static cardio::promise<void> reconnect_application_async(
    SmbApplication *state, SettingsStore settings) {
  if (state->startup) co_await *state->startup;
  if (state->client) co_await stop_smb_client_async(state->client);
  co_await stop_smb_ssh_proxy_async(state->route);
  state->route.reset();
  state->client.reset();
  if (state->shutting_down) co_return;
  state->connection = smb_connection_settings(settings);
  state->proxy = ssh_proxy_connection_settings(settings);
  co_await start_application_async(state);
}

int run_smb_application(const SettingsLoadResult &settings,
    std::optional<std::filesystem::path> config_path, std::string ssh_known_hosts_file) {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  SmbApplication state;
  state.dispatcher_group = &group;
  state.connection = smb_connection_settings(settings.store);
  state.proxy = ssh_proxy_connection_settings(settings.store);
  state.ssh_known_hosts_file = std::move(ssh_known_hosts_file);
  state.window = create_file_transfer_window({
      .connection_name = general_connection_name(settings.store),
      .protocol_name = "SMB",
      .local_directory = resolve_file_transfer_local_directory(settings.store, state.connection.local_directory),
      .remote_directory = state.connection.remote_directory, .remote_file_hash = {},
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

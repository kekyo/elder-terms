#include "webdav-application.h"
#include "webdav-client.h"
#include "../file-transfer/file-transfer-window.h"
#include "../file-transfer/file-transfer-paths.h"
#include "../file-transfer/file-transfer-certificate-prompt.h"

#include <iostream>
#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

struct WebdavApplication {
  cardio::dispatcher_group_glib *dispatcher_group;
  WebdavConnectionSettings connection;
  std::shared_ptr<RemoteFileClient> client;
  std::shared_ptr<FileTransferWindow> window;
  cardio::cancellation_source stopping;
  std::optional<cardio::promise<void>> startup;
  std::optional<cardio::promise<void>> shutdown;
  bool shutting_down = false;
};

static cardio::promise<void> finish_application_async(WebdavApplication *state) {
  try {
    if (state->startup) co_await *state->startup;
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  auto closing = close_file_transfer_window_async(state->window);
  try {
    if (state->client) co_await stop_webdav_client_async(state->client);
    co_await closing;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  state->dispatcher_group->shutdown();
}

static void stop_application(WebdavApplication *state) {
  if (state->shutting_down) return;
  state->shutting_down = true;
  (void)state->stopping.cancel();
  state->shutdown.emplace(finish_application_async(state));
}

static cardio::promise<bool> confirm_webdav_certificate_async(
    WebdavApplication *state, const TlsCertificateFailure &failure,
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

static cardio::promise<void> start_application_async(WebdavApplication *state) {
  std::string failure;
  const auto cancellation = state->stopping.get_cancellation();
  try {
    WebdavClientOpenOptions options{
        .connection = state->connection, .password = {},
        .confirm_certificate = [state](const TlsCertificateFailure &failure, cardio::cancellation cancellation) {
          return confirm_webdav_certificate_async(state, failure, cancellation);
        }};
    if (options.connection.authentication != WebdavAuthentication::none) {
      std::string username = options.connection.username;
      if (username.empty() && g_get_user_name()) username = g_get_user_name();
      bool missing = false;
      while (true) {
        auto *formatted = g_strdup_printf(_("Enter the user name and password for %s."), options.connection.address.c_str());
        std::string message(formatted);
        g_free(formatted);
        if (missing) message += "\n\n" + std::string(_("User name must not be empty."));
        InlinePromptRequest request{
            .title = _("WebDAV authentication"), .message = std::move(message),
            .accept_label = _("Connect"), .cancel_label = _("Cancel"),
            .initial_text = username, .input_label = _("User name"),
            .input_required = true, .echo = true,
            .secondary_input_label = _("Password:"), .secondary_input_required = true,
            .secondary_echo = false, .cancel_visible = true};
        auto response = co_await prompt_file_transfer_window_async(state->window, std::move(request), cancellation);
        if (!response.accepted) { stop_application(state); co_return; }
        username = std::move(response.text);
        if (username.find_first_not_of(" \t\r\n") != std::string::npos) {
          options.connection.username = std::move(username);
          options.password = std::move(response.secondary_text);
          break;
        }
        missing = true;
      }
    }
    state->client = co_await open_webdav_client_async(std::move(options), cancellation);
    attach_file_transfer_window_client(state->window, state->client);
    co_return;
  } catch (const cardio::canceled_exception &) {
    stop_application(state);
    co_return;
  } catch (const std::exception &error) { failure = error.what(); }
  if (!state->shutting_down) {
    std::cerr << failure << '\n';
    co_await show_file_transfer_window_connection_error_async(
        state->window, _("Failed to start WebDAV"), std::move(failure), cancellation);
  }
}

int run_webdav_application(const SettingsLoadResult &settings,
    std::optional<std::filesystem::path> config_path) {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  WebdavApplication state;
  state.dispatcher_group = &group;
  state.connection = webdav_connection_settings(settings.store);
  state.window = create_file_transfer_window({
      .connection_name = general_connection_name(settings.store),
      .protocol_name = "WebDAV",
      .local_directory = resolve_file_transfer_local_directory(settings.store, state.connection.local_directory),
      .remote_directory = state.connection.remote_directory, .remote_file_hash = {},
      .colors = general_color_settings(settings.store),
      .closed = [&state] { stop_application(&state); },
      .settings = settings.store, .config_path = std::move(config_path)});
  show_file_transfer_window(state.window);
  state.startup.emplace(start_application_async(&state));
  dispatcher.park();
  return 0;
}

} // namespace elder_terms

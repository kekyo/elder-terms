#include <clocale>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <gtk/gtk.h>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

#include <cardio.h>
#include <elder-terms/localization.h>
#include <elder-terms/settings/application-settings.h>
#include <elder-terms/settings.h>

#include "../file-transfer/file-transfer-paths.h"
#include "../file-transfer/file-transfer-window.h"
#include "../launch-options.h"
#include "../sftp/sftp-fixture-client.h"
#include "ftp-client.h"

struct FtpApplicationState {
  cardio::dispatcher_group_glib *dispatcher_group = nullptr;
  bool fixture = false;
  elder_terms::SettingsStore settings;
  elder_terms::FtpConnectionSettings connection;
  std::shared_ptr<elder_terms::RemoteFileClient> client;
  std::shared_ptr<elder_terms::FileTransferWindow> window;
  cardio::cancellation_source stop_source;
  std::optional<cardio::promise<void>> startup_task;
  bool shutting_down = false;
};

static std::string format_message(const char *format,
                                  const std::string &first,
                                  const std::string &second) {
  gchar *value = g_strdup_printf(format, first.c_str(), second.c_str());
  const std::string result = value == nullptr ? std::string() : value;
  g_free(value);
  return result;
}

static void stop_ftp_application(FtpApplicationState *state) {
  if (state == nullptr || state->shutting_down) {
    return;
  }
  state->shutting_down = true;
  (void)state->stop_source.cancel();
  state->dispatcher_group->shutdown();
}

static void create_ftp_application_window(FtpApplicationState *state) {
  state->window = elder_terms::create_file_transfer_window(
      {
          .connection_name =
              elder_terms::general_connection_name(state->settings),
          .protocol_name = "FTP",
          .local_directory =
              elder_terms::resolve_file_transfer_local_directory(
                  state->settings, state->connection.local_directory),
          .remote_directory = state->connection.remote_directory,
          .colors = elder_terms::general_color_settings(state->settings),
          .closed =
              [state]() {
                stop_ftp_application(state);
              },
      });
  elder_terms::show_file_transfer_window(state->window);
}

static cardio::promise<std::optional<std::string>>
prompt_ftp_password_async(FtpApplicationState *state,
                          cardio::cancellation cancellation) {
  elder_terms::InlinePromptResponse response =
      co_await elder_terms::prompt_file_transfer_window_async(
          state->window,
          {
              .title = _("FTP authentication"),
              .message = format_message(
                  _("Enter the password for %s on %s."),
                  state->connection.username, state->connection.address),
              .accept_label = _("Connect"),
              .cancel_label = _("Cancel"),
              .input_required = true,
              .echo = false,
              .cancel_visible = true,
          },
          std::move(cancellation));
  if (!response.accepted) {
    co_return std::nullopt;
  }
  co_return std::move(response.text);
}

static cardio::promise<void>
start_ftp_application_async(FtpApplicationState *state) {
  std::string failure;
  try {
    const cardio::cancellation cancellation =
        state->stop_source.get_cancellation();
    std::string password;
    if (!state->connection.username.empty()) {
      std::optional<std::string> prompted =
          co_await prompt_ftp_password_async(state, cancellation);
      if (!prompted.has_value()) {
        stop_ftp_application(state);
        co_return;
      }
      password = std::move(*prompted);
    }
    if (state->fixture) {
      state->client = elder_terms::create_sftp_fixture_client(false);
    } else {
      state->client = co_await elder_terms::open_ftp_client_async(
          {
              .connection = state->connection,
              .password = std::move(password),
          },
          cancellation);
    }
    elder_terms::attach_file_transfer_window_client(
        state->window, state->client);
    co_return;
  } catch (const cardio::canceled_exception &) {
    stop_ftp_application(state);
    co_return;
  } catch (const std::exception &exception) {
    failure = exception.what();
  }

  if (!state->shutting_down) {
    std::cerr << failure << '\n';
    co_await elder_terms::show_file_transfer_window_connection_error_async(
        state->window, _("Failed to start FTP"), std::move(failure),
        state->stop_source.get_cancellation());
  }
}

int main(int argc, char **argv) {
  const elder_terms::ApplicationUiLanguage ui_language =
      elder_terms::load_application_ui_language_preference(
          elder_terms::default_global_config_path());
  const elder_terms::LocalizationInitializationResult localization =
      elder_terms::initialize_localization(ui_language);
  for (const std::string &warning : localization.warnings) {
    std::cerr << warning << '\n';
  }
  gtk_disable_setlocale();
  elder_terms::LaunchOptions launch_options =
      elder_terms::parse_launch_options(&argc, argv);
  gtk_init(&argc, &argv);

  const elder_terms::SettingsLoadResult settings_result =
      elder_terms::load_settings(
          {
              .config_path = launch_options.config_path,
              .startup_config_path = launch_options.startup_config_path,
              .global_config_path =
                  elder_terms::default_global_config_path(),
          },
          1.0);
  for (const std::string &warning : settings_result.warnings) {
    std::cerr << warning << '\n';
  }
  if (!elder_terms::general_settings_select_ftp_connection(
          settings_result.store)) {
    std::cerr << "Error: configured connection type is not FTP\n";
    return 1;
  }

  cardio::dispatcher_group_glib dispatcher_group;
  cardio::dispatcher_host_glib dispatcher(dispatcher_group);
  FtpApplicationState state;
  state.dispatcher_group = &dispatcher_group;
  state.fixture = launch_options.test.fixture;
  state.settings = settings_result.store;
  state.connection =
      elder_terms::ftp_connection_settings(settings_result.store);

  create_ftp_application_window(&state);
  if (state.fixture && state.connection.username.empty()) {
    state.client = elder_terms::create_sftp_fixture_client(false);
    elder_terms::attach_file_transfer_window_client(
        state.window, state.client);
  } else {
    state.startup_task.emplace(start_ftp_application_async(&state));
  }

  dispatcher.park();
  (void)state.stop_source.cancel();
  state.window.reset();
  state.client.reset();
  state.startup_task.reset();
  return 0;
}

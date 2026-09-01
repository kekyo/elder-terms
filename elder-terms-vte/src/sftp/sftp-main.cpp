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
#include "../terminal-session-callbacks.h"
#include "../terminal-sessions/ssh-session/authenticated-ssh-transport.h"
#include "sftp-client.h"
#include "sftp-fixture-client.h"

struct SftpApplicationState {
  cardio::dispatcher_group_glib *dispatcher_group = nullptr;
  elder_terms::LaunchOptions launch_options;
  elder_terms::SettingsStore settings;
  elder_terms::SftpConnectionSettings connection;
  std::shared_ptr<elder_terms::AuthenticatedSshTransport> transport;
  std::shared_ptr<elder_terms::RemoteFileClient> client;
  std::shared_ptr<elder_terms::FileTransferWindow> window;
  cardio::cancellation_source stop_source;
  std::optional<cardio::promise<void>> startup_task;
  bool shutting_down = false;
};

static void stop_sftp_application(SftpApplicationState *state) {
  if (state == nullptr || state->shutting_down) {
    return;
  }
  state->shutting_down = true;
  (void)state->stop_source.cancel();
  state->dispatcher_group->shutdown();
}

static void create_sftp_application_window(SftpApplicationState *state) {
  state->window = elder_terms::create_file_transfer_window(
      {
          .connection_name =
              elder_terms::general_connection_name(state->settings),
          .protocol_name = "SFTP",
          .local_directory =
              elder_terms::resolve_file_transfer_local_directory(
                  state->settings, state->connection.local_directory),
          .remote_directory = state->connection.remote_directory,
          .colors = elder_terms::general_color_settings(state->settings),
          .closed =
              [state]() {
                stop_sftp_application(state);
              },
      });
  elder_terms::show_file_transfer_window(state->window);
}

static cardio::promise<elder_terms::SshUserPromptResponse>
prompt_sftp_authentication_async(
    SftpApplicationState *state,
    const elder_terms::SshUserPrompt &prompt,
    cardio::cancellation cancellation) {
  elder_terms::InlinePromptResponse response =
      co_await elder_terms::prompt_file_transfer_window_async(
          state->window,
          {
              .title = prompt.title.empty() ? _("SSH") : prompt.title,
              .message = prompt.message,
              .accept_label =
                  prompt.kind == elder_terms::SshUserPromptKind::host_key
                      ? _("Accept")
                      : _("OK"),
              .cancel_label = _("Cancel"),
              .input_required = prompt.input_required,
              .echo = prompt.echo,
              .cancel_visible = true,
          },
          std::move(cancellation));
  if (!response.accepted) {
    (void)state->stop_source.cancel();
  }
  co_return elder_terms::SshUserPromptResponse{
      .accepted = response.accepted,
      .text = std::move(response.text),
  };
}

static cardio::promise<void>
start_sftp_application_async(SftpApplicationState *state) {
  std::string failure;
  try {
    const cardio::cancellation cancellation =
        state->stop_source.get_cancellation();
    elder_terms::TerminalSessionCallbacks callbacks{
        .ended = {},
        .activity = {},
        .indicator_state = {},
        .connection_phase = {},
        .failure = {},
        .output = {},
        .zmodem_auto_start = {},
        .ssh_prompt =
            [state](const elder_terms::SshUserPrompt &prompt,
                    cardio::cancellation prompt_cancellation) {
              return prompt_sftp_authentication_async(
                  state, prompt, std::move(prompt_cancellation));
            },
    };
    state->transport =
        co_await elder_terms::AuthenticatedSshTransport::connect_async(
            state->connection.endpoint, callbacks,
            {
                .known_hosts_file =
                    state->launch_options.test.ssh_known_hosts_file,
            },
            cancellation);
    state->client = co_await elder_terms::open_sftp_client_async(
        state->transport, cancellation);
    elder_terms::attach_file_transfer_window_client(
        state->window, state->client);
    co_return;
  } catch (const cardio::canceled_exception &) {
    stop_sftp_application(state);
    co_return;
  } catch (const std::exception &exception) {
    failure = exception.what();
  }

  if (!state->shutting_down) {
    std::cerr << failure << '\n';
    co_await elder_terms::show_file_transfer_window_connection_error_async(
        state->window, _("Failed to start SFTP"), std::move(failure),
        state->stop_source.get_cancellation());
  }
}

static elder_terms::SshUserPrompt fixture_sftp_prompt(
    const std::string &fixture) {
  if (fixture == "host-key") {
    return {
        .kind = elder_terms::SshUserPromptKind::host_key,
        .title = _("SSH Host Key"),
        .message = _("Accept the fixture SSH host key?"),
        .input_required = false,
        .echo = false,
    };
  }
  return {
      .kind = elder_terms::SshUserPromptKind::password,
      .title = _("SSH Authentication"),
      .message = _("Password:"),
      .input_required = true,
      .echo = false,
  };
}

static cardio::promise<void>
start_sftp_fixture_async(SftpApplicationState *state) {
  const cardio::cancellation cancellation =
      state->stop_source.get_cancellation();
  if (state->launch_options.test.ssh_prompt.has_value()) {
    const elder_terms::SshUserPromptResponse response =
        co_await prompt_sftp_authentication_async(
            state,
            fixture_sftp_prompt(*state->launch_options.test.ssh_prompt),
            cancellation);
    if (!response.accepted) {
      stop_sftp_application(state);
      co_return;
    }
  }
  cancellation.throw_if_cancellation_requested();
  state->client = elder_terms::create_sftp_fixture_client(
      state->launch_options.test.sftp_pause_transfer);
  elder_terms::attach_file_transfer_window_client(
      state->window, state->client);
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
              .global_config_path = elder_terms::default_global_config_path(),
          },
          1.0);
  for (const std::string &warning : settings_result.warnings) {
    std::cerr << warning << '\n';
  }
  if (!elder_terms::general_settings_select_sftp_connection(
          settings_result.store)) {
    std::cerr << "Error: configured connection type is not SFTP\n";
    return 1;
  }

  cardio::dispatcher_group_glib dispatcher_group;
  cardio::dispatcher_host_glib dispatcher(dispatcher_group);
  SftpApplicationState state;
  state.dispatcher_group = &dispatcher_group;
  state.launch_options = std::move(launch_options);
  state.settings = settings_result.store;
  state.connection =
      elder_terms::sftp_connection_settings(settings_result.store);

  create_sftp_application_window(&state);
  if (state.launch_options.test.fixture) {
    if (state.launch_options.test.ssh_prompt.has_value()) {
      state.startup_task.emplace(start_sftp_fixture_async(&state));
    } else {
      state.client = elder_terms::create_sftp_fixture_client(
          state.launch_options.test.sftp_pause_transfer);
      elder_terms::attach_file_transfer_window_client(
          state.window, state.client);
    }
  } else {
    state.startup_task.emplace(start_sftp_application_async(&state));
  }

  dispatcher.park();
  (void)state.stop_source.cancel();
  state.window.reset();
  state.client.reset();
  state.transport.reset();
  state.startup_task.reset();
  return 0;
}

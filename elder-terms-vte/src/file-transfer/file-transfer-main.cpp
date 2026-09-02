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

#include "../ftp/ftp-client.h"
#include "../launch-options.h"
#include "../sftp/sftp-client.h"
#include "../sftp/sftp-fixture-client.h"
#include "../terminal-session-callbacks.h"
#include "../terminal-sessions/ssh-session/authenticated-ssh-transport.h"
#include "file-transfer-paths.h"
#include "file-transfer-window.h"

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

struct FtpRuntimeCredentials {
  std::string username;
  std::string password;
};

static std::string format_message(const char *format,
                                  const std::string &value) {
  gchar *formatted = g_strdup_printf(format, value.c_str());
  const std::string result =
      formatted == nullptr ? std::string() : formatted;
  g_free(formatted);
  return result;
}

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
                      : prompt.kind ==
                                elder_terms::SshUserPromptKind::username
                            ? _("Connect")
                            : _("OK"),
              .cancel_label = _("Cancel"),
              .initial_text = prompt.initial_text,
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
                .config_file = {},
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
        .initial_text = {},
        .input_required = false,
        .echo = false,
    };
  }
  if (fixture == "username") {
    return {
        .kind = elder_terms::SshUserPromptKind::username,
        .title = _("SSH Authentication"),
        .message = _("User name for fixture.example:"),
        .initial_text = "configured-user",
        .input_required = true,
        .echo = true,
    };
  }
  return {
      .kind = elder_terms::SshUserPromptKind::password,
      .title = _("SSH Authentication"),
      .message = _("Password:"),
      .initial_text = {},
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

static int run_sftp_application(
    const elder_terms::SettingsLoadResult &settings_result,
    elder_terms::LaunchOptions launch_options) {
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

static std::string initial_ftp_username(
    const elder_terms::FtpConnectionSettings &connection) {
  if (!connection.username.empty()) {
    return connection.username;
  }
  const char *current_username = g_get_user_name();
  return current_username == nullptr ? std::string()
                                     : std::string(current_username);
}

static std::string ftp_authentication_message(
    const elder_terms::FtpConnectionSettings &connection,
    bool username_missing) {
  std::string message = format_message(
      _("Enter the user name and password for %s."), connection.address);
  message += "\n\n";
  message += _("To log in anonymously, enter anonymous as the user name.");
  if (username_missing) {
    message += "\n\n";
    message += _("User name must not be empty.");
  }
  return message;
}

static cardio::promise<std::optional<FtpRuntimeCredentials>>
prompt_ftp_credentials_async(FtpApplicationState *state,
                             cardio::cancellation cancellation) {
  std::string username = initial_ftp_username(state->connection);
  bool username_missing = false;
  while (true) {
    elder_terms::InlinePromptResponse response =
        co_await elder_terms::prompt_file_transfer_window_async(
            state->window,
            {
                .title = _("FTP authentication"),
                .message = ftp_authentication_message(
                    state->connection, username_missing),
                .accept_label = _("Connect"),
                .cancel_label = _("Cancel"),
                .initial_text = username,
                .input_label = _("User name"),
                .input_required = true,
                .echo = true,
                .secondary_input_label = _("Password:"),
                .secondary_input_required = true,
                .secondary_echo = false,
                .cancel_visible = true,
            },
            cancellation);
    if (!response.accepted) {
      co_return std::nullopt;
    }
    username = std::move(response.text);
    if (username.find_first_not_of(" \t\r\n") != std::string::npos) {
      co_return FtpRuntimeCredentials{
          .username = std::move(username),
          .password = std::move(response.secondary_text),
      };
    }
    username_missing = true;
  }
}

static cardio::promise<void>
start_ftp_application_async(FtpApplicationState *state) {
  std::string failure;
  try {
    const cardio::cancellation cancellation =
        state->stop_source.get_cancellation();
    std::optional<FtpRuntimeCredentials> credentials =
        co_await prompt_ftp_credentials_async(state, cancellation);
    if (!credentials.has_value()) {
      stop_ftp_application(state);
      co_return;
    }
    if (state->fixture) {
      state->client = elder_terms::create_sftp_fixture_client(false);
    } else {
      elder_terms::FtpConnectionSettings connection = state->connection;
      connection.username = std::move(credentials->username);
      state->client = co_await elder_terms::open_ftp_client_async(
          {
              .connection = std::move(connection),
              .password = std::move(credentials->password),
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

static int run_ftp_application(
    const elder_terms::SettingsLoadResult &settings_result,
    bool fixture) {
  cardio::dispatcher_group_glib dispatcher_group;
  cardio::dispatcher_host_glib dispatcher(dispatcher_group);
  FtpApplicationState state;
  state.dispatcher_group = &dispatcher_group;
  state.fixture = fixture;
  state.settings = settings_result.store;
  state.connection =
      elder_terms::ftp_connection_settings(settings_result.store);

  create_ftp_application_window(&state);
  state.startup_task.emplace(start_ftp_application_async(&state));

  dispatcher.park();
  (void)state.stop_source.cancel();
  state.window.reset();
  state.client.reset();
  state.startup_task.reset();
  return 0;
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

  const elder_terms::ConnectionKind kind =
      elder_terms::general_connection_kind(settings_result.store);
  if (kind == elder_terms::ConnectionKind::sftp) {
    return run_sftp_application(settings_result, std::move(launch_options));
  }
  if (kind == elder_terms::ConnectionKind::ftp) {
    return run_ftp_application(settings_result,
                               launch_options.test.fixture);
  }
  std::cerr << "Error: configured connection type is not SFTP or FTP\n";
  return 1;
}

#include <clocale>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
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
#include "file-hash.h"
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
  cardio::primitives::manually_conditional fixture_hash_gate{false};
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
  std::optional<cardio::promise<void>> shutdown_task;
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

static cardio::promise<elder_terms::FileHashes>
calculate_sftp_file_hashes_async(
    SftpApplicationState *state, std::string path,
    cardio::cancellation cancellation) {
  if (!state->launch_options.test.fixture) {
    if (state->transport == nullptr) {
      throw std::runtime_error("SSH transport is not connected");
    }
    co_return co_await elder_terms::calculate_ssh_file_hashes_async(
        state->transport, std::move(path), std::move(cancellation));
  }

  if (state->launch_options.test.sftp_pause_transfer) {
    co_await state->fixture_hash_gate.wait(cancellation);
  }
  cancellation.throw_if_cancellation_requested();
  if (path != "/remote/readme.txt") {
    throw std::runtime_error("Fixture remote file is unavailable");
  }
  co_return elder_terms::FileHashes{
      .md5 = "f840a57e7c2e27409f9a22366c97aa38",
      .sha1 = "4e9891113f487a51fda4942050b67e650e6db5eb",
      .sha256 =
          "d79bda8bec3b76fa69692436f1d8ce37a168df014f925dd1fc18c58a550d05a9",
  };
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
          .remote_file_hash =
              [state](std::string path,
                      cardio::cancellation cancellation) {
                return calculate_sftp_file_hashes_async(
                    state, std::move(path), std::move(cancellation));
              },
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
  elder_terms::InlinePromptRequest request{
      .title = prompt.title.empty() ? _("SSH") : prompt.title,
      .message = prompt.message,
      .monospace_message = prompt.monospace_message,
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
      .accept_visible = prompt.accept_visible,
      .alternative_label = _("Reset and Connect"),
      .alternative_visible = prompt.host_key_reset_available,
  };
  auto pending = elder_terms::prompt_file_transfer_window_async(
      state->window, std::move(request), std::move(cancellation));
  elder_terms::InlinePromptResponse response = co_await pending;
  if (!response.accepted && !response.alternative) {
    (void)state->stop_source.cancel();
  }
  co_return elder_terms::SshUserPromptResponse{
      .accepted = response.accepted,
      .text = std::move(response.text),
      .reset_host_key = response.alternative,
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
    elder_terms::AuthenticatedSshTransportOptions options{
        .known_hosts_file =
            state->launch_options.test.ssh_known_hosts_file,
        .config_file = {},
    };
    auto connecting = elder_terms::AuthenticatedSshTransport::connect_async(
        state->connection.endpoint, callbacks, options, cancellation);
    state->transport = co_await connecting;
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
  if (fixture == "changed-host-key") {
    return {
        .kind = elder_terms::SshUserPromptKind::host_key,
        .title = _("SSH Host Key Changed"),
        .message =
            _("The SSH host key for fixture.example:2222 has changed.\n"
              "Verify the fingerprint before resetting the saved key.\n"
              "Equivalent command:\n"
              "ssh-keygen -R '[fixture.example]:2222' -f "
              "'/home/test/.ssh/known_hosts'"),
        .monospace_message =
            "+--[ED25519 256]--+\n"
            "|         .oo..   |\n"
            "|        . .oo    |\n"
            "|        oo*o     |\n"
            "|       .+*=o     |\n"
            "| o .   .S*+..    |\n"
            "|= o ... oooo     |\n"
            "|o. oEo = ..o     |\n"
            "|   oo.B Oo. .    |\n"
            "|    . .@o+.      |\n"
            "+----[SHA256]-----+",
        .initial_text = {},
        .input_required = false,
        .echo = false,
        .accept_visible = false,
        .host_key_reset_available = true,
    };
  }
  if (fixture == "host-key") {
    return {
        .kind = elder_terms::SshUserPromptKind::host_key,
        .title = _("SSH Host Key"),
        .message = _("Accept the fixture SSH host key?"),
        .monospace_message =
            "+--[ED25519 256]--+\n"
            "|         .oo..   |\n"
            "|        . .oo    |\n"
            "|        oo*o     |\n"
            "|       .+*=o     |\n"
            "| o .   .S*+..    |\n"
            "|= o ... oooo     |\n"
            "|o. oEo = ..o     |\n"
            "|   oo.B Oo. .    |\n"
            "|    . .@o+.      |\n"
            "+----[SHA256]-----+",
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
    const auto prompt = fixture_sftp_prompt(*state->launch_options.test.ssh_prompt);
    const elder_terms::SshUserPromptResponse response =
        co_await prompt_sftp_authentication_async(state, prompt, cancellation);
    if (!response.accepted && !response.reset_host_key) {
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

// Keep the dispatcher alive until authentication, callbacks and the dedicated
// curl worker have finished. This task is owned by the application state.
static cardio::promise<void> finish_ftp_application_async(FtpApplicationState *state) {
  try {
    if (state->startup_task.has_value()) co_await *state->startup_task;
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
  }
  auto closing = elder_terms::close_file_transfer_window_async(state->window);
  try {
    if (state->client && !state->fixture) {
      co_await elder_terms::stop_ftp_client_async(state->client);
    }
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
  }
  try {
    co_await closing;
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
  }
  state->dispatcher_group->shutdown();
}

static void stop_ftp_application(FtpApplicationState *state) {
  if (state == nullptr || state->shutting_down) {
    return;
  }
  state->shutting_down = true;
  (void)state->stop_source.cancel();
  state->shutdown_task.emplace(finish_ftp_application_async(state));
}

static void create_ftp_application_window(FtpApplicationState *state) {
  state->window = elder_terms::create_file_transfer_window(
      {
          .connection_name =
              elder_terms::general_connection_name(state->settings),
          .protocol_name = state->connection.tls_mode == elder_terms::FtpTlsMode::none ? "FTP" : "FTPS",
          .local_directory =
              elder_terms::resolve_file_transfer_local_directory(
                  state->settings, state->connection.local_directory),
          .remote_directory = state->connection.remote_directory,
          .remote_file_hash = {},
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
    elder_terms::InlinePromptRequest request{
        .title = state->connection.tls_mode == elder_terms::FtpTlsMode::none
            ? _("FTP authentication") : _("FTPS authentication"),
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
    };
    auto pending = elder_terms::prompt_file_transfer_window_async(
        state->window, std::move(request), cancellation);
    elder_terms::InlinePromptResponse response = co_await pending;
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

static std::string certificate_display_text(const std::string &text) {
  if (text.empty()) return _("Unavailable");
  auto *valid = g_utf8_make_valid(text.data(), text.size());
  std::string result;
  unsigned count = 0;
  for (const char *cursor = valid; *cursor; cursor = g_utf8_next_char(cursor), ++count) {
    if (count == 256) { result += "…"; break; }
    if (count && count % 48 == 0) result += '\n';
    result.append(cursor, g_utf8_next_char(cursor) - cursor);
  }
  g_free(valid);
  return result;
}

static cardio::promise<bool> confirm_ftp_certificate_async(
    FtpApplicationState *state, const elder_terms::FtpCertificateFailure &failure,
    cardio::cancellation cancellation) {
  std::string message = _("The server certificate could not be validated.");
  message += "\n\n" + certificate_display_text(failure.address) + ":" + std::to_string(failure.port);
  message += "\n";
  message += failure.channel == elder_terms::FtpTlsChannel::control ? _("Control connection") : _("Data connection");
  message += "\n" + certificate_display_text(failure.reason);
  message += "\n\n";
  message += _("Allow only this certificate and validation failure for this connection? Exceptions are not saved.");
  std::string details = std::string(_("Subject:")) + " " + certificate_display_text(failure.subject);
  details += "\n" + std::string(_("Issuer:")) + " " + certificate_display_text(failure.issuer);
  details += "\n" + std::string(_("Valid from:")) + " " + certificate_display_text(failure.not_before);
  details += "\n" + std::string(_("Valid until:")) + " " + certificate_display_text(failure.not_after);
  details += "\nSHA-256:\n" + failure.sha256.substr(0, 48) + "\n" + failure.sha256.substr(48);
  message += "\n\n" + details;
  elder_terms::InlinePromptRequest request{
      .title = _("FTPS certificate validation failed"), .message = std::move(message),
      .accept_label = _("Allow for this connection"),
      .cancel_label = _("Abort connection"), .input_required = false, .echo = false,
      .cancel_visible = true, .default_cancel = true};
  auto pending = elder_terms::confirm_file_transfer_window_async(
      state->window, std::move(request), cancellation);
  const auto response = co_await pending;
  if (!response.accepted || cancellation.is_cancellation_requested() || state->shutting_down) {
    stop_ftp_application(state);
    co_return false;
  }
  elder_terms::mark_file_transfer_certificate_exception(state->window);
  co_return true;
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
      elder_terms::FtpClientOpenOptions options{
          .connection = std::move(connection),
          .password = std::move(credentials->password),
          .confirm_certificate = [state](const elder_terms::FtpCertificateFailure &failure, cardio::cancellation cancellation) {
            return confirm_ftp_certificate_async(state, failure, cancellation);
          },
      };
      auto opening = elder_terms::open_ftp_client_async(
          std::move(options), cancellation);
      state->client = co_await opening;
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
        state->window, state->connection.tls_mode == elder_terms::FtpTlsMode::none
            ? _("Failed to start FTP") : _("Failed to start FTPS"), std::move(failure),
        state->stop_source.get_cancellation());
  }
}

static int run_ftp_application(
    const elder_terms::SettingsLoadResult &settings_result,
    bool fixture) {
  cardio::dispatcher_group_glib dispatcher_group;
  // Worker completions must wake the GLib context even before it starts waiting.
  cardio::dispatcher_host_glib_auto dispatcher(dispatcher_group);
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
  state.shutdown_task.reset();
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

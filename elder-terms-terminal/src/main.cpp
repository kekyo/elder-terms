// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/modal-dialog.h>
#include <elder-terms/tracked-window.h>
#include <algorithm>
#include <clocale>
#include <cstdarg>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtk/gtk.h>
#include <type_traits>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

#include <cardio.h>
#include <gestament/gtk.h>

#include <elder-terms/application-icon.h>
#include <elder-terms/connection-desktop-session.h>
#include <elder-terms/connection-icon-image.h>
#include <elder-terms/localization.h>
#include <elder-terms/settings/application-settings.h>
#include <elder-terms/settings.h>
#include <elder-terms/settings-widget.h>

#include "launch-options.h"
#include "main-window.h"
#include "file-transfer/file-hash.h"
#include "file-transfer/file-transfer-paths.h"
#include "file-transfer/file-transfer-window.h"
#include "sftp/sftp-client.h"
#include "sftp/sftp-fixture-client.h"
#include "terminal-bell-player.h"
#include "terminal-bell.h"
#include "terminal-hyperlink-action.h"
#include "terminal-presentation.h"
#include "terminal-renderer-status.h"
#include "terminal-runtime.h"
#include "terminal-transports.h"
#include "terminal-log.h"
#include "terminal-text-send.h"
#include "terminal-transfer-runner.h"
#include "terminal-sessions/ssh-session/authenticated-ssh-transport.h"

static constexpr char terminal_application_id[] =
    "net.kekyo.elder-terms-terminal";

struct ApplicationState {
  elder_terms::MainWindow *main_window = nullptr;
  elder_terms::TerminalRuntime *runtime = nullptr;
  elder_terms::TerminalPresentation *presentation = nullptr;
  elder_terms::TerminalConnectionProfile active_profile;
  std::optional<elder_terms::TerminalTransportRequest> transport;
  std::uint64_t closed_generation = 0;
  bool closing = false;
  bool connect_when_ready = false;
  bool audible_bell = true;
  std::string terminal_title;
  elder_terms::TerminalLogState *log_state = nullptr;
  elder_terms::TerminalBellPlayer *bell_player = nullptr;
  elder_terms::TerminalBellState *bell_state = nullptr;
  cardio::dispatcher_group_glib *dispatcher_group = nullptr;
  std::optional<cardio::promise<void>> shutdown_task;
  std::optional<cardio::promise<void>> ssh_prompt_fixture_task;
  std::optional<cardio::promise<void>> sftp_open_task;
  std::optional<cardio::promise<void>> sftp_close_task;
  std::optional<cardio::promise<void>> sftp_connection_check_task;
  std::optional<cardio::cancellation_source> sftp_cancel_source;
  std::shared_ptr<elder_terms::AuthenticatedSshTransport> sftp_transport;
  std::shared_ptr<elder_terms::RemoteFileClient> sftp_client;
  std::shared_ptr<elder_terms::FileTransferWindow> sftp_window;
  elder_terms::SettingsStore settings_store;
  std::optional<std::filesystem::path> config_path;
  elder_terms::TestOptions test_options;
  bool auto_close = true;
  elder_terms::TerminalSessionConnectionPhase connection_phase =
      elder_terms::TerminalSessionConnectionPhase::disconnected;
  bool connection_active = false;
  bool connection_failed = false;
  bool transfer_active = false;
  bool sftp_opening = false;
  bool sftp_connection_check_active = false;
  bool terminal_shutdown_complete = false;
  GtkWidget *window = nullptr;
  GtkWidget *settings_dialog = nullptr;
  GtkWidget *transfer_file_dialog = nullptr;
  guint settings_dialog_close_idle_id = 0;
  elder_terms::SettingsWidgetState *settings_widget = nullptr;
  GtkWidget *log_enabled_menu_item = nullptr;
};

struct TransferMenuAction {
  elder_terms::TerminalTransferProtocol protocol =
      elder_terms::TerminalTransferProtocol::zmodem;
  elder_terms::TerminalTransferDirection direction =
      elder_terms::TerminalTransferDirection::send;
  elder_terms::TerminalTransferOptions options;
};

struct TransferFileDialogRequest {
  std::function<void(std::vector<std::string>)> selected;
};

static void close_settings_dialog(ApplicationState *state);
static void close_transfer_file_dialog(ApplicationState *state);
static void schedule_settings_dialog_close(ApplicationState *state);
static void restore_terminal_focus(ApplicationState *state);
static void start_shared_sftp_connection_check(ApplicationState *state);
static void update_application_session_identity(ApplicationState *state);
static void update_application_terminal_presentation(
    ApplicationState *state);

static bool connect_application_terminal(ApplicationState *state);
static void apply_zmodem_auto_start(ApplicationState *state);

static void on_external_command_error_response(GtkDialog *dialog, gint,
                                               gpointer) {
  gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void show_external_command_error(ApplicationState *state,
                                        const char *warning_context,
                                        const char *dialog_title,
                                        const char *accessible_id,
                                        const std::string &detail) {
  std::cerr << "Warning: " << warning_context << " failed: " << detail << '\n';
  if (state == nullptr || state->window == nullptr) {
    return;
  }

  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(state->window),
      static_cast<GtkDialogFlags>(0),
      GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", dialog_title);
  gtk_window_set_title(GTK_WINDOW(dialog), dialog_title);
  gestament_gtk_assign_accessible_id(dialog, accessible_id);
  AtkObject *accessible = gtk_widget_get_accessible(dialog);
  if (accessible != nullptr) {
    atk_object_set_name(accessible, dialog_title);
  }
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
                                           detail.c_str());
  g_signal_connect(dialog, "response",
                   G_CALLBACK(on_external_command_error_response), nullptr);
  elder_terms::show_modal_dialog(dialog, GTK_WINDOW(state->window));
}

static bool spawn_external_command(ApplicationState *state,
                                   std::string command,
                                   std::vector<std::string> arguments,
                                   const char *warning_context,
                                   const char *dialog_title,
                                   const char *accessible_id) {
  if (command.find('\0') != std::string::npos ||
      std::any_of(arguments.begin(), arguments.end(),
                  [](const std::string &argument) {
                    return argument.find('\0') != std::string::npos;
                  })) {
    show_external_command_error(state, warning_context, dialog_title,
                                accessible_id,
                                "command contains a NUL byte");
    return false;
  }

  std::vector<gchar *> argv;
  argv.reserve(arguments.size() + 2);
  argv.push_back(command.data());
  for (std::string &argument : arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  GError *error = nullptr;
  if (!g_spawn_async(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH,
                     nullptr, nullptr, nullptr, &error)) {
    const std::string detail =
        error == nullptr || error->message == nullptr
            ? std::string("unknown spawn error")
            : std::string(error->message);
    g_clear_error(&error);
    show_external_command_error(state, warning_context, dialog_title,
                                accessible_id, detail);
    return false;
  }
  return true;
}

static std::string resolve_launcher_executable() {
  const char *configured = g_getenv("ELDER_TERMS_LAUNCHER_PATH");
  if (configured != nullptr && configured[0] != '\0') {
    return configured;
  }

  std::error_code read_error;
  const std::filesystem::path executable =
      std::filesystem::read_symlink("/proc/self/exe", read_error);
  if (!read_error) {
    const std::filesystem::path private_directory =
        executable.parent_path().parent_path();
    const std::filesystem::path candidates[] = {
        private_directory / "launcher" / "elder-terms",
        private_directory / "elder-terms" / "elder-terms",
    };
    for (const std::filesystem::path &candidate : candidates) {
      if (g_file_test(candidate.c_str(), G_FILE_TEST_IS_EXECUTABLE)) {
        return candidate.string();
      }
    }
  }

  char *found = g_find_program_in_path("elder-terms");
  if (found == nullptr) {
    return "elder-terms";
  }
  const std::string result = found;
  g_free(found);
  return result;
}

static void request_launcher_about_page(ApplicationState *state) {
  (void)spawn_external_command(
      state, resolve_launcher_executable(), {"--about"},
      "application dialog request", _("Failed to open elder-terms"),
      "application_dialog_request_error");
}

static void spawn_macro_command(ApplicationState *state, std::string command,
                                std::vector<std::string> arguments) {
  (void)spawn_external_command(
      state, std::move(command), std::move(arguments), "macro command",
      _("Macro command failed"), "macro_command_error_dialog");
}

static void apply_terminal_hyperlink_settings(ApplicationState *state) {
  elder_terms::terminal_runtime_set_hyperlink_rules(state->runtime,
      elder_terms::effective_hyperlink_action_rules(state->settings_store),
      [state](elder_terms::TerminalHyperlinkResolution resolution) {
        if (!resolution.action) {
          show_external_command_error(state, "hyperlink command", _("Hyperlink command failed"),
              "hyperlink_command_error_dialog", resolution.error);
          return;
        }
        spawn_external_command(state, resolution.action->command, resolution.action->arguments,
            "hyperlink command", _("Hyperlink command failed"), "hyperlink_command_error_dialog");
      });
}

static void apply_received_macros(ApplicationState *state) {
  elder_terms::terminal_runtime_set_macro_rules(state->runtime, state->settings_store.macro_rules,
      elder_terms::transfer_text_send_follow_return_code(state->settings_store),
      [state](std::string command, std::vector<std::string> arguments) {
        spawn_macro_command(state, std::move(command), std::move(arguments));
      }, [](std::string message) { std::cerr << "Warning: received macro: " << message << '\n'; });
}

static std::string format_translated_string(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  gchar *formatted = g_strdup_vprintf(format, arguments);
  va_end(arguments);
  const std::string result = formatted == nullptr ? std::string() : formatted;
  g_free(formatted);
  return result;
}

static void maybe_shutdown_application(ApplicationState *state) {
  if (state == nullptr || !state->terminal_shutdown_complete ||
      state->window != nullptr || state->sftp_window != nullptr ||
      state->sftp_opening) {
    return;
  }
  elder_terms::shutdown_connection_desktop_session();
  state->dispatcher_group->shutdown();
}

static cardio::promise<void>
run_ssh_prompt_fixture_async(ApplicationState *state,
                             const std::string &fixture) {
  elder_terms::SshUserPrompt prompt;
  if (fixture == "changed-host-key" ||
      fixture == "changed-global-host-key") {
    const bool global_key = fixture == "changed-global-host-key";
    prompt = {
        .kind = elder_terms::SshUserPromptKind::host_key,
        .title = _("SSH Host Key Changed"),
        .message =
            global_key
                ? _("The SSH host key for fixture.example:2222 has changed.\n"
                    "Verify the fingerprint before asking an administrator "
                    "to run:\n"
                    "sudo ssh-keygen -R '[fixture.example]:2222' -f "
                    "'/etc/ssh/ssh_known_hosts'")
                : _("The SSH host key for fixture.example:2222 has changed.\n"
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
        .host_key_reset_available = !global_key,
    };
  } else if (fixture == "host-key") {
    prompt = {
        .kind = elder_terms::SshUserPromptKind::host_key,
        .title = _("SSH Host Key"),
        .message = format_translated_string(
            _("Unknown SSH host key for %s:%d\n"
              "Key type: %s\n"
              "Fingerprint: %s\n"
              "Accept and save this host key?"),
            "fixture.example", 22, "ssh-ed25519",
            "SHA256:fixture-host-key"),
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
  } else if (fixture == "username") {
    prompt = {
        .kind = elder_terms::SshUserPromptKind::username,
        .title = _("SSH Authentication"),
        .message = _("User name for fixture.example:"),
        .initial_text = "configured-user",
        .input_required = true,
        .echo = true,
    };
  } else {
    prompt = {
        .kind = elder_terms::SshUserPromptKind::password,
        .title = _("SSH Authentication"),
        .message = _("Password:"),
        .initial_text = {},
        .input_required = true,
        .echo = false,
    };
  }

  const elder_terms::SshUserPromptResponse response =
      co_await elder_terms::prompt_main_window_ssh_async(
          state->main_window, prompt, {});
  if (response.accepted &&
      prompt.kind == elder_terms::SshUserPromptKind::password) {
    update_application_terminal_presentation(state);
  }
  elder_terms::set_main_window_status_text(
      state->main_window,
      response.reset_host_key
          ? _("SSH host key reset requested")
          : response.accepted ? _("SSH prompt accepted")
                              : _("SSH prompt cancelled"));
}

static gboolean destroy_terminal_window_idle(gpointer context) {
  auto *state = static_cast<ApplicationState *>(context);
  elder_terms::terminal_runtime_set_changed_callback(state->runtime, nullptr, nullptr);
  elder_terms::terminal_runtime_set_host_callbacks(state->runtime, {});
  elder_terms::destroy_terminal_presentation(state->presentation);
  state->presentation = nullptr;
  elder_terms::destroy_terminal_runtime(state->runtime);
  state->runtime = nullptr;
  state->main_window->runtime = nullptr;
  state->main_window->terminal = nullptr;
  state->terminal_shutdown_complete = true;
  if (state->window) gtk_widget_destroy(state->window);
  maybe_shutdown_application(state);
  return G_SOURCE_REMOVE;
}

static cardio::promise<void>
stop_application_async(ApplicationState *state) {
  co_await elder_terms::terminal_runtime_disconnect(state->runtime);
  co_await elder_terms::stop_terminal_log_async(state->log_state);
  // A close request may originate inside a runtime frame notification. Release
  // its callbacks and widgets only after that dispatch has returned.
  g_idle_add_full(G_PRIORITY_DEFAULT, destroy_terminal_window_idle, state, nullptr);
}

static cardio::promise<void> check_shared_sftp_connection_async(
    ApplicationState *state,
    std::shared_ptr<elder_terms::AuthenticatedSshTransport> transport,
    cardio::cancellation cancellation) {
  bool available = false;
  try {
    available =
        co_await transport->is_connected_async(std::move(cancellation));
  } catch (const cardio::canceled_exception &) {
    state->sftp_connection_check_active = false;
    co_return;
  } catch (const std::exception &error) {
    std::cerr << "Warning: failed to check shared SFTP connection: "
              << error.what() << '\n';
  }

  if (state->sftp_transport == transport &&
      state->sftp_window != nullptr) {
    elder_terms::set_file_transfer_window_connection_available(
        state->sftp_window, available);
  }
  state->sftp_connection_check_active = false;
}

static void start_shared_sftp_connection_check(
    ApplicationState *state) {
  if (state == nullptr || state->sftp_window == nullptr ||
      state->sftp_connection_check_active || state->sftp_opening) {
    return;
  }
  if (state->test_options.fixture) {
    elder_terms::set_file_transfer_window_connection_available(
        state->sftp_window,
        !state->test_options.shared_sftp_disconnected);
    return;
  }
  if (state->sftp_transport == nullptr ||
      !state->sftp_cancel_source.has_value()) {
    elder_terms::set_file_transfer_window_connection_available(
        state->sftp_window, false);
    return;
  }

  state->sftp_connection_check_active = true;
  state->sftp_connection_check_task.reset();
  state->sftp_connection_check_task.emplace(
      check_shared_sftp_connection_async(
          state, state->sftp_transport,
          state->sftp_cancel_source->get_cancellation()));
}

static void update_application_reconnect_presentation(ApplicationState *state) {
  if (state == nullptr || state->window == nullptr) {
    return;
  }
  const auto profile =
      elder_terms::terminal_connection_profile(state->settings_store);
  if (!profile.has_value()) {
    return;
  }
  elder_terms::set_main_window_reconnect_presentation(
      state->main_window,
      elder_terms::terminal_reconnect_presentation(
          profile->kind, state->auto_close, state->connection_phase,
          (elder_terms::terminal_runtime_connection_state(state->runtime).phase == elder_terms::TerminalConnectionPhase::closed && !state->closing),
          false));
}

static void on_reconnect_clicked(GtkButton *, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state == nullptr || state->window == nullptr || state->auto_close) {
    return;
  }
  (void)connect_application_terminal(state);
}

static void update_application_terminal_presentation(
    ApplicationState *state) {
  if (state == nullptr) {
    return;
  }

  update_application_reconnect_presentation(state);

  const bool terminal_interactive =
      state->connection_active && !state->transfer_active &&
      !elder_terms::has_modal_dialog(state->window);
  elder_terms::set_main_window_terminal_interactive(
      state->main_window, terminal_interactive);
  if (terminal_interactive) {
    elder_terms::focus_main_window_terminal_if_interactive(
        state->main_window);
  }
  elder_terms::set_main_window_transfer_button_sensitive(
      state->main_window,
      state->connection_active && !state->transfer_active &&
          !elder_terms::has_modal_dialog(state->window));
}

static void on_parent_sensitivity_changed(GObject *, GParamSpec *, gpointer data) {
  update_application_terminal_presentation(static_cast<ApplicationState *>(data));
}

static void update_application_session_identity(ApplicationState *state) {
  if (!state || !state->window) return;
  const auto title = state->terminal_title.empty() ? std::string("elder-terms") : state->terminal_title;
  elder_terms::set_main_window_title(state->main_window, title + " | " + state->active_profile.name);
  const auto detail = std::visit([](const auto &settings) -> std::string {
    using Settings = std::decay_t<decltype(settings)>;
    if constexpr (std::is_same_v<Settings, elder_terms::LocalShellConnectionSettings>) return _("local terminal");
    else if constexpr (std::is_same_v<Settings, elder_terms::TelnetConnectionSettings>)
      return settings.address.empty() ? _("telnet: (unknown)") : "telnet: " + settings.address + ":" + std::to_string(settings.port);
    else if constexpr (std::is_same_v<Settings, elder_terms::SshConnectionSettings>) {
      if (settings.endpoint.address.empty()) return _("ssh: (unknown)");
      return "ssh: " + (settings.endpoint.username.empty() ? std::string() : settings.endpoint.username + "@") +
          settings.endpoint.address + ":" + std::to_string(settings.endpoint.port);
    } else {
      if (settings.device.empty()) return _("serial: (unknown)");
      const char flow = settings.flow_control == elder_terms::SerialFlowControl::xon ? 'x' :
          settings.flow_control == elder_terms::SerialFlowControl::hard ? 'h' : 'n';
      return "serial: " + settings.device + ":" + std::to_string(settings.baudrate) + ":" +
          elder_terms::serial_parity_to_string(settings.parity) + std::to_string(settings.bits) +
          std::to_string(settings.stop_bit) + flow;
    }
  }, state->active_profile.settings);
  elder_terms::set_main_window_status_text(state->main_window, detail);
}

static void set_application_transfer_progress_visible(ApplicationState *state,
                                                      bool active) {
  if (state == nullptr) {
    return;
  }
  elder_terms::set_main_window_transfer_progress_visible(state->main_window,
                                                         active);
}

static void set_application_transfer_active(ApplicationState *state,
                                            bool active) {
  if (state == nullptr) {
    return;
  }

  state->transfer_active = active;
  update_application_terminal_presentation(state);
}

static void set_application_connection_phase(
    ApplicationState *state,
    elder_terms::TerminalSessionConnectionPhase phase) {
  if (state == nullptr) {
    return;
  }

  if (phase == elder_terms::TerminalSessionConnectionPhase::connecting) {
    state->connection_failed = false;
    apply_received_macros(state);
    update_application_session_identity(state);
  }
  const elder_terms::TerminalConnectionPresentation presentation =
      elder_terms::terminal_connection_presentation(phase);
  state->connection_phase = phase;
  state->connection_active = presentation.connection_active;
  elder_terms::set_main_window_connection_phase(state->main_window, phase);
  elder_terms::set_terminal_log_connection_active(
      state->log_state, presentation.connection_active);
  update_application_terminal_presentation(state);
  if (phase ==
      elder_terms::TerminalSessionConnectionPhase::disconnected) {
    start_shared_sftp_connection_check(state);
  }
}

static void set_application_indicator_state(
    ApplicationState *state, elder_terms::ActivityIndicatorId indicator,
    bool active) {
  if (state == nullptr) {
    return;
  }

  if (indicator == elder_terms::ActivityIndicatorId::conn) {
    set_application_connection_phase(
        state, active
                   ? elder_terms::TerminalSessionConnectionPhase::connected
                   : elder_terms::TerminalSessionConnectionPhase::disconnected);
    return;
  }

  elder_terms::set_main_window_indicator_state(state->main_window, indicator, active);
}

static void restore_terminal_focus(ApplicationState *state) {
  if (state == nullptr || state->window == nullptr ||
      state->main_window == nullptr) {
    return;
  }

  if (!gtk_widget_get_visible(state->window)) {
    return;
  }

  elder_terms::present_modal_dialog(state->window);
  if (!elder_terms::has_modal_dialog(state->window)) {
    elder_terms::focus_main_window_terminal_if_interactive(state->main_window);
  }
}

static gboolean on_main_window_focus_in(
    GtkWidget *, GdkEventFocus *, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state != nullptr && !elder_terms::has_modal_dialog(state->window)) {
    elder_terms::focus_main_window_terminal_if_interactive(
        state->main_window);
  }
  return GDK_EVENT_PROPAGATE;
}

static void on_main_window_destroy(GtkWidget *, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  state->window = nullptr;
  elder_terms::cancel_main_window_ssh_prompt(state->main_window);
  elder_terms::set_main_window_transfer_progress_visible(state->main_window,
                                                         false);
  elder_terms::deactivate_main_window_activity_indicators(state->main_window);
  close_settings_dialog(state);
  close_transfer_file_dialog(state);
  elder_terms::set_terminal_log_connection_active(state->log_state, false);
  if (!state->closing) {
    state->closing = true;
    state->shutdown_task.emplace(stop_application_async(state));
  }
  maybe_shutdown_application(state);
}

static gboolean on_main_window_delete(GtkWidget *, GdkEvent *, gpointer data) {
  auto *state = static_cast<ApplicationState *>(data);
  if (!state->closing) {
    state->closing = true;
    elder_terms::cancel_main_window_ssh_prompt(state->main_window);
    close_settings_dialog(state);
    close_transfer_file_dialog(state);
    state->shutdown_task.emplace(stop_application_async(state));
  }
  return TRUE;
}

static bool connect_application_terminal(ApplicationState *state) {
  if (state->closing) return false;
  const auto profile = elder_terms::terminal_connection_profile(state->settings_store);
  if (!profile) return false;
  state->active_profile = *profile;
  state->transport.reset();
  elder_terms::TerminalSessionCallbacks callbacks;
  callbacks.activity = [state](elder_terms::ActivityIndicatorId id) { elder_terms::note_main_window_activity(state->main_window, id); };
  callbacks.indicator_state = [state](elder_terms::ActivityIndicatorId id, bool active) {
    elder_terms::set_main_window_indicator_state(state->main_window, id, active);
  };
  callbacks.connection_phase = [state](elder_terms::TerminalSessionConnectionPhase phase) {
    if (!state->closing) set_application_connection_phase(state, phase);
  };
  callbacks.ssh_prompt = [state](const elder_terms::SshUserPrompt &prompt, cardio::cancellation cancellation) {
    return elder_terms::prompt_main_window_ssh_async(state->main_window, prompt, cancellation);
  };
  const elder_terms::AuthenticatedSshTransportOptions options{
      state->test_options.ssh_known_hosts_file, {}, profile->ssh_proxy};
  elder_terms::terminal_runtime_request_input_settings(state->runtime, profile->text_settings);
  set_application_connection_phase(state, elder_terms::TerminalSessionConnectionPhase::connecting);
  return std::visit([&](const auto &settings) {
    using Settings = std::decay_t<decltype(settings)>;
    if constexpr (std::is_same_v<Settings, elder_terms::LocalShellConnectionSettings>) {
      elder_terms::TerminalLocalCommand command;
      command.arguments = settings.argv;
      if (command.arguments.empty()) {
        const auto *shell = g_getenv("SHELL");
        command.arguments.push_back(shell && *shell ? shell : "/bin/sh");
      }
      command.encoding = profile->text_settings.encoding;
      command.activity = [state](bool outgoing) {
        elder_terms::note_main_window_activity(state->main_window,
            outgoing ? elder_terms::ActivityIndicatorId::sd : elder_terms::ActivityIndicatorId::rd);
      };
      return elder_terms::terminal_runtime_connect_local(state->runtime, std::move(command));
    } else {
      if constexpr (std::is_same_v<Settings, elder_terms::TelnetConnectionSettings>)
        state->transport = elder_terms::make_telnet_transport_request(settings, profile->text_settings.encoding, callbacks, options);
      else if constexpr (std::is_same_v<Settings, elder_terms::SshConnectionSettings>)
        state->transport = elder_terms::make_ssh_transport_request(settings, profile->text_settings.encoding, callbacks, options);
      else state->transport = elder_terms::make_serial_transport_request(settings, profile->text_settings.encoding, callbacks);
      return elder_terms::terminal_runtime_connect_transport(state->runtime, *state->transport);
    }
  }, profile->settings);
}

static void update_settings_renderer_status(ApplicationState *state) {
  if (!state->settings_widget || !state->runtime) return;
  const auto status = elder_terms::terminal_renderer_status(elder_terms::terminal_runtime_widget(state->runtime));
  elder_terms::settings_widget_set_runtime_status(state->settings_widget, status.text, status.tooltip);
  elder_terms::settings_widget_set_background_status(state->settings_widget,
      elder_terms::terminal_presentation_background_status(state->presentation));
}

static void application_terminal_changed(void *context) {
  auto *state = static_cast<ApplicationState *>(context);
  if (!state->window || state->closing) return;
  elder_terms::refresh_terminal_presentation(state->presentation);
  update_settings_renderer_status(state);
  if (state->test_options.fixture) return;
  if (state->connect_when_ready && elder_terms::terminal_runtime_is_settled(state->runtime)) {
    state->connect_when_ready = false;
    if (!connect_application_terminal(state))
      set_application_connection_phase(state, elder_terms::TerminalSessionConnectionPhase::disconnected);
  }
  const auto connection = elder_terms::terminal_runtime_connection_state(state->runtime);
  if (connection.phase == elder_terms::TerminalConnectionPhase::connected) {
    const auto phase = connection.carrier_connected ? elder_terms::TerminalSessionConnectionPhase::connected :
        elder_terms::TerminalSessionConnectionPhase::disconnected;
    if (phase != state->connection_phase) set_application_connection_phase(state, phase);
  } else if (connection.phase == elder_terms::TerminalConnectionPhase::starting) {
    if (state->connection_phase == elder_terms::TerminalSessionConnectionPhase::disconnected)
      set_application_connection_phase(state, elder_terms::TerminalSessionConnectionPhase::connecting);
  } else if (connection.phase == elder_terms::TerminalConnectionPhase::closed && connection.generation != state->closed_generation) {
    state->closed_generation = connection.generation;
    if (connection.operation_error || connection.spawn.state == BOXRIG_POSIX_PTY_SPAWN_FAILED) {
      state->connection_failed = state->active_profile.kind == elder_terms::TerminalConnectionKind::ssh;
      const auto message = connection.operation_error_message.empty() ?
          std::string(g_strerror(connection.operation_error ? connection.operation_error : connection.spawn.error_number)) :
          connection.operation_error_message;
      std::cerr << "Error: " << message << '\n';
      if (state->active_profile.kind == elder_terms::TerminalConnectionKind::ssh ||
          state->active_profile.kind == elder_terms::TerminalConnectionKind::telnet)
        elder_terms::set_main_window_connection_failure(state->main_window, message, state->active_profile.kind);
    }
    set_application_connection_phase(state, elder_terms::TerminalSessionConnectionPhase::disconnected);
    if (state->auto_close && !state->connection_failed && state->active_profile.kind != elder_terms::TerminalConnectionKind::serial)
      gtk_window_close(GTK_WINDOW(state->window));
  }
}

static void on_settings_dialog_destroy(GtkWidget *, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  elder_terms::cancel_terminal_presentation_background(state->presentation);
  if (state->settings_dialog_close_idle_id != 0) {
    g_source_remove(state->settings_dialog_close_idle_id);
    state->settings_dialog_close_idle_id = 0;
  }
  elder_terms::set_main_window_settings_dialog(
      state->main_window, nullptr, nullptr);
  if (state->settings_widget != nullptr) {
    elder_terms::destroy_settings_widget(state->settings_widget);
    state->settings_widget = nullptr;
  }
  state->settings_dialog = nullptr;

  restore_terminal_focus(state);
}

static void close_settings_dialog(ApplicationState *state) {
  if (state == nullptr) {
    return;
  }

  if (state->settings_dialog_close_idle_id != 0) {
    g_source_remove(state->settings_dialog_close_idle_id);
    state->settings_dialog_close_idle_id = 0;
  }

  if (state->settings_dialog == nullptr) {
    return;
  }

  gtk_widget_destroy(state->settings_dialog);
}

static gboolean close_settings_dialog_idle(gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state != nullptr) {
    state->settings_dialog_close_idle_id = 0;
  }
  close_settings_dialog(state);
  return G_SOURCE_REMOVE;
}

static void schedule_settings_dialog_close(ApplicationState *state) {
  if (state == nullptr || state->settings_dialog == nullptr ||
      state->settings_dialog_close_idle_id != 0) {
    return;
  }

  state->settings_dialog_close_idle_id =
      g_idle_add(close_settings_dialog_idle, state);
}

static bool apply_runtime_settings(ApplicationState *state,
    const elder_terms::SettingsStore &store, elder_terms::TerminalBackgroundImagePtr image) {
  const auto previous_image = elder_terms::terminal_presentation_background_image(state->presentation);
  if (!elder_terms::apply_terminal_presentation(state->presentation, store, std::move(image))) {
    if (!elder_terms::apply_terminal_presentation(state->presentation, state->settings_store, previous_image))
      std::cerr << "Failed to restore terminal presentation\n";
    return false;
  }
  state->settings_store = store;
  elder_terms::update_connection_desktop_session(state->settings_store);
  elder_terms::set_connection_window_icon(GTK_WINDOW(state->main_window->window),
      GTK_IMAGE(state->main_window->header_connection_icon),
      elder_terms::general_connection_icon(state->settings_store),
      state->main_window->compact_mode ? 18 : 24);
  elder_terms::apply_terminal_bell_settings(
      state->bell_state,
      elder_terms::terminal_bell_settings(state->settings_store));
  apply_received_macros(state);
  apply_terminal_hyperlink_settings(state);
  const elder_terms::GeneralColorSettings colors =
      elder_terms::general_color_settings(state->settings_store);
  elder_terms::set_main_window_colors(
      state->main_window, colors);
  elder_terms::set_main_window_indicator_color(
      state->main_window, elder_terms::general_indicator_color(state->settings_store),
      elder_terms::general_indicator_off_color(state->settings_store));
  if (state->sftp_window != nullptr) {
    elder_terms::set_file_transfer_window_connection_icon(
        state->sftp_window,
        elder_terms::general_connection_icon(state->settings_store));
    elder_terms::set_file_transfer_window_colors(
        state->sftp_window, colors);
    elder_terms::set_file_transfer_window_indicator_colors(
        state->sftp_window,
        elder_terms::general_indicator_color(state->settings_store),
        elder_terms::general_indicator_off_color(state->settings_store));
  }
  const elder_terms::TerminalLogSettings log_settings =
      elder_terms::terminal_log_settings(state->settings_store);
  elder_terms::apply_terminal_log_settings(
      state->log_state, log_settings);
  if (state->log_enabled_menu_item != nullptr &&
      (gtk_check_menu_item_get_active(
           GTK_CHECK_MENU_ITEM(state->log_enabled_menu_item)) != FALSE) !=
          log_settings.enabled) {
    gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(state->log_enabled_menu_item),
        log_settings.enabled ? TRUE : FALSE);
  }
  state->auto_close = elder_terms::general_auto_close(state->settings_store);
  const auto connection_profile =
      elder_terms::terminal_connection_profile(state->settings_store);
  if (!connection_profile.has_value()) {
    return true;
  }
  elder_terms::set_main_window_activity_indicator_connection_kind(
      state->main_window, connection_profile->kind);
  state->active_profile.name = connection_profile->name;
  state->active_profile.text_settings = connection_profile->text_settings;
  elder_terms::terminal_runtime_request_input_settings(state->runtime, connection_profile->text_settings);
  const auto connection = elder_terms::terminal_runtime_connection_state(state->runtime);
  if (connection.encoding != connection_profile->text_settings.encoding)
    elder_terms::terminal_runtime_request_encoding(state->runtime, connection_profile->text_settings.encoding);
  if (state->transport) state->transport->encoding = connection_profile->text_settings.encoding;
  if (const auto *serial = std::get_if<elder_terms::SerialConnectionSettings>(&connection_profile->settings)) {
    if (std::holds_alternative<elder_terms::SerialConnectionSettings>(state->active_profile.settings)) {
      try {
        if (state->transport && state->transport->configure_serial) state->transport->configure_serial(*serial);
        auto updated = *serial;
        const auto &current = std::get<elder_terms::SerialConnectionSettings>(state->active_profile.settings);
        updated.device = current.device;
        updated.device_match_mode = current.device_match_mode;
        updated.device_usb_serial = current.device_usb_serial;
        state->active_profile.settings = updated;
      } catch (const std::exception &error) { std::cerr << "Warning: serial settings: " << error.what() << '\n'; }
    }
  }
  apply_zmodem_auto_start(state);
  elder_terms::set_main_window_transfer_button_visible(state->main_window, true);
  update_application_terminal_presentation(state);
  update_application_session_identity(state);
  return true;
}

static bool save_runtime_settings(
  ApplicationState *state, const elder_terms::SettingsStore &store, elder_terms::TerminalBackgroundImagePtr image) {
  if (!state->config_path.has_value()) {
    return false;
  }

  const auto previous = state->settings_store;
  const auto previous_image = elder_terms::terminal_presentation_background_image(state->presentation);
  if (!apply_runtime_settings(state, store, std::move(image))) return false;

  const elder_terms::SettingsSaveResult save_result =
      elder_terms::save_settings(store, state->config_path.value());
  for (const std::string &warning : save_result.warnings) {
    std::cerr << warning << '\n';
  }
  if (!save_result.saved) {
    if (!apply_runtime_settings(state, previous, previous_image))
      std::cerr << "Failed to restore settings after save failure\n";
    return false;
  }

  return true;
}

static void request_runtime_settings(ApplicationState *state, const elder_terms::SettingsStore &store,
    elder_terms::SettingsWidgetAction action, bool save) {
  const auto current = action.current;
  elder_terms::prepare_terminal_presentation_background(state->presentation, store, current,
      [state, store, action = std::move(action), save](elder_terms::TerminalBackgroundImagePtr image, std::string error) {
        if (!error.empty()) { action.complete(false, std::move(error)); return; }
        const bool accepted = save ? save_runtime_settings(state, store, std::move(image)) :
            apply_runtime_settings(state, store, std::move(image));
        action.complete(accepted, accepted ? "" : save ? _("Failed to save connection") : _("Failed to apply terminal settings"));
        if (accepted && save) schedule_settings_dialog_close(state);
      });
}

static void update_runtime_terminal_display_settings(
    ApplicationState *state,
    elder_terms::TerminalDisplaySettings terminal_display_settings) {
  elder_terms::set_setting_value(
      &state->settings_store, elder_terms::terminal_width_setting_key(),
      elder_terms::SettingValue{
          static_cast<gint64>(terminal_display_settings.width)});
  elder_terms::set_setting_value(
      &state->settings_store, elder_terms::terminal_height_setting_key(),
      elder_terms::SettingValue{
          static_cast<gint64>(terminal_display_settings.height)});
  elder_terms::set_setting_value(
      &state->settings_store,
      elder_terms::terminal_scrollback_lines_setting_key(),
      elder_terms::SettingValue{
          static_cast<gint64>(terminal_display_settings.scrollback_lines)});
  elder_terms::set_setting_value(
      &state->settings_store, elder_terms::terminal_font_size_dip_setting_key(),
      elder_terms::SettingValue{terminal_display_settings.font_size_dip});

  if (state->settings_widget != nullptr) {
    elder_terms::update_settings_widget_store(
      state->settings_widget, state->settings_store);
  }
}

static void open_settings_dialog(ApplicationState *state) {
  if (state->settings_dialog != nullptr) {
    elder_terms::present_modal_dialog(state->settings_dialog);
    return;
  }

  GtkWidget *dialog = gtk_dialog_new();
  gestament_gtk_assign_accessible_id(dialog, "settings_dialog");
  gtk_window_set_title(GTK_WINDOW(dialog), _("Settings"));
  GtkWidget *header_bar = gtk_header_bar_new();
  gestament_gtk_assign_accessible_id(
      header_bar, "settings_dialog_header_bar");
  gtk_header_bar_set_title(GTK_HEADER_BAR(header_bar), _("Settings"));
  gtk_header_bar_set_show_close_button(
      GTK_HEADER_BAR(header_bar), TRUE);
  gtk_window_set_titlebar(GTK_WINDOW(dialog), header_bar);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 720, 495);

  elder_terms::SettingsWidgetCallbacks callbacks;
  callbacks.apply = [state](const elder_terms::SettingsStore &store, elder_terms::SettingsWidgetAction action) {
    request_runtime_settings(state, store, std::move(action), false);
  };
  if (state->config_path.has_value()) {
    callbacks.save = [state](const elder_terms::SettingsStore &store, elder_terms::SettingsWidgetAction action) {
      request_runtime_settings(state, store, std::move(action), true);
    };
  }
  callbacks.cancel = [state]() {
    elder_terms::cancel_terminal_presentation_background(state->presentation);
    schedule_settings_dialog_close(state);
  };
  callbacks.changed = [state] { elder_terms::cancel_terminal_presentation_background(state->presentation); };

  elder_terms::SettingsWidgetOptions options{
      .store = state->settings_store,
      .is_runtime = true,
      .callbacks = std::move(callbacks),
  };
  state->settings_widget =
      elder_terms::create_settings_widget(std::move(options));
  update_settings_renderer_status(state);
  GtkWidget *settings_root =
      elder_terms::settings_widget_root(state->settings_widget);
  gtk_container_add(
      GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
      settings_root);

  state->settings_dialog = dialog;
  elder_terms::set_main_window_settings_dialog(
      state->main_window, dialog, settings_root);
  g_signal_connect(dialog, "destroy",
                   G_CALLBACK(on_settings_dialog_destroy), state);

  elder_terms::show_modal_dialog(dialog, GTK_WINDOW(state->window));
}

static void on_settings_menu_item_activate(GtkMenuItem *, gpointer user_data) {
  open_settings_dialog(static_cast<ApplicationState *>(user_data));
}

static void on_about_menu_item_activate(GtkMenuItem *, gpointer user_data) {
  request_launcher_about_page(static_cast<ApplicationState *>(user_data));
}

static gboolean emit_transfer_dialog_current_folder_uri(gpointer data) {
  auto *chooser = GTK_FILE_CHOOSER(data);
  char *uri = gtk_file_chooser_get_current_folder_uri(chooser);
  std::cout << "ELDER_TERMS_TRANSFER_DIALOG_CURRENT_FOLDER_URI="
            << (uri == nullptr ? "" : uri) << '\n'
            << std::flush;
  if (uri != nullptr) {
    g_free(uri);
  }
  return G_SOURCE_REMOVE;
}

static void schedule_transfer_dialog_probe(GtkFileChooser *chooser) {
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                  emit_transfer_dialog_current_folder_uri,
                  g_object_ref(chooser), g_object_unref);
}

static void set_transfer_dialog_initial_folder(ApplicationState *state,
                                               GtkFileChooser *chooser) {
  const std::string base_path =
      elder_terms::transfer_base_path(state->settings_store);
  const std::string folder_uri =
      elder_terms::resolve_transfer_base_path_uri(base_path);
  if (folder_uri.empty()) {
    return;
  }

  if (!gtk_file_chooser_set_current_folder_uri(chooser, folder_uri.c_str())) {
    std::cerr << "Warning: failed to set transfer file dialog folder: "
              << folder_uri << '\n';
  }
}

static std::vector<std::string> selected_transfer_file_uris(
    GtkFileChooser *chooser) {
  std::vector<std::string> uris;
  GSList *files = gtk_file_chooser_get_files(chooser);
  for (GSList *item = files; item != nullptr; item = item->next) {
    GFile *file = G_FILE(item->data);
    char *uri = g_file_get_uri(file);
    if (uri != nullptr) {
      uris.emplace_back(uri);
      g_free(uri);
    }
    g_object_unref(file);
  }
  g_slist_free(files);
  return uris;
}

static void on_transfer_file_dialog_destroy(GtkWidget *dialog,
                                            gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state == nullptr || state->transfer_file_dialog != dialog) {
    return;
  }

  state->transfer_file_dialog = nullptr;
}

static void on_transfer_file_dialog_response(GtkDialog *dialog,
                                             gint response,
                                             gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  auto *request = static_cast<TransferFileDialogRequest *>(
      g_object_get_data(G_OBJECT(dialog),
                        "elder-terms-transfer-file-request"));
  std::function<void(std::vector<std::string>)> selected;
  if (request != nullptr) {
    selected = std::move(request->selected);
  }

  std::vector<std::string> uris;
  if (response == GTK_RESPONSE_ACCEPT) {
    uris = selected_transfer_file_uris(GTK_FILE_CHOOSER(dialog));
  }

  if (state != nullptr &&
      state->transfer_file_dialog == GTK_WIDGET(dialog)) {
    state->transfer_file_dialog = nullptr;
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));

  if (selected) {
    selected(std::move(uris));
    return;
  }
}

static void close_transfer_file_dialog(ApplicationState *state) {
  if (state != nullptr && state->transfer_file_dialog != nullptr) {
    gtk_widget_destroy(state->transfer_file_dialog);
  }
}

static void choose_transfer_files(
    ApplicationState *state, bool select_multiple,
    std::function<void(std::vector<std::string>)> selected) {
  if (state == nullptr || state->window == nullptr || !selected) {
    return;
  }
  if (!state->test_options.transfer_source_uris.empty()) {
    selected(state->test_options.transfer_source_uris);
    return;
  }
  if (state->transfer_file_dialog != nullptr) {
    elder_terms::present_modal_dialog(state->transfer_file_dialog);
    return;
  }

  GtkWidget *dialog = gtk_file_chooser_dialog_new(
      _("Select file"), GTK_WINDOW(state->window),
      GTK_FILE_CHOOSER_ACTION_OPEN, _("Cancel"), GTK_RESPONSE_CANCEL,
      _("Open"), GTK_RESPONSE_ACCEPT, nullptr);
  gestament_gtk_assign_accessible_id(dialog, "transfer_file_dialog");

  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

  GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
  gtk_file_chooser_set_local_only(chooser, FALSE);
  gtk_file_chooser_set_select_multiple(chooser, select_multiple);
  set_transfer_dialog_initial_folder(state, chooser);
  if (state->test_options.transfer_dialog_probe) {
    schedule_transfer_dialog_probe(chooser);
  }

  auto *request = new TransferFileDialogRequest{
      .selected = std::move(selected),
  };
  g_object_set_data_full(
      G_OBJECT(dialog), "elder-terms-transfer-file-request", request,
      [](gpointer data) {
        delete static_cast<TransferFileDialogRequest *>(data);
      });
  state->transfer_file_dialog = dialog;
  g_signal_connect(dialog, "response",
                   G_CALLBACK(on_transfer_file_dialog_response), state);
  g_signal_connect(dialog, "destroy",
                   G_CALLBACK(on_transfer_file_dialog_destroy), state);

  elder_terms::show_modal_dialog(dialog, GTK_WINDOW(state->window));
}

static elder_terms::TerminalTransferRequest make_transfer_request(ApplicationState *state,
                                   const TransferMenuAction &action,
                                   std::vector<std::string> source_uris) {
  elder_terms::TerminalTransferRequest request{
      .protocol = action.protocol,
      .direction = action.direction,
      .base_path = elder_terms::transfer_base_path(state->settings_store),
      .source_file_uris = std::move(source_uris),
      .options = action.options,
      .active =
          [state](bool active) {
            set_application_transfer_active(state, active);
            set_application_transfer_progress_visible(state, active);
          },
      .status =
          [state](const std::string &status) {
            elder_terms::set_main_window_status_text(state->main_window,
                                                     status);
          },
      .progress =
          [state](elder_terms::TerminalTransferProgress progress) {
            elder_terms::set_main_window_transfer_progress(state->main_window,
                                                           progress);
          },
      .finished =
          [state](bool) {
            restore_terminal_focus(state);
          },
  };

  return request;
}

static bool start_transfer_request(ApplicationState *state, const TransferMenuAction &action,
    std::vector<std::string> source_uris) {
  return elder_terms::terminal_runtime_start_transfer(state->runtime, make_transfer_request(state, action, std::move(source_uris)));
}

static void on_transfer_menu_item_activate(GtkMenuItem *item,
                                           gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  const auto *action = static_cast<const TransferMenuAction *>(
      g_object_get_data(G_OBJECT(item), "elder-terms-transfer-action"));
  if (state == nullptr || action == nullptr) {
    return;
  }

  std::vector<std::string> source_uris;
  if (action->direction == elder_terms::TerminalTransferDirection::send) {
    const TransferMenuAction selected_action = *action;
    choose_transfer_files(
        state,
        action->protocol != elder_terms::TerminalTransferProtocol::xmodem,
        [state, selected_action](std::vector<std::string> selected_uris) {
          if (selected_uris.empty()) return;
          if (!start_transfer_request(state, selected_action,
                                      std::move(selected_uris))) {
            elder_terms::set_main_window_status_text(
                state->main_window, _("Transfer unavailable"));
            update_application_terminal_presentation(state);
            restore_terminal_focus(state);
          }
        });
    return;
  }

  if (!start_transfer_request(state, *action, std::move(source_uris))) {
    elder_terms::set_main_window_status_text(state->main_window,
                                             _("Transfer unavailable"));
  }
}

struct AutoTransferSelection {
  cardio::primitives::conditional changed;
  bool ready = false;
  std::vector<std::string> uris;
};

static cardio::promise<std::optional<elder_terms::TerminalTransferRequest>> select_auto_transfer(
    ApplicationState *state, elder_terms::TerminalTransferDirection direction, cardio::cancellation cancellation) {
  TransferMenuAction action{elder_terms::TerminalTransferProtocol::zmodem, direction, {}};
  if (direction == elder_terms::TerminalTransferDirection::receive)
    co_return make_transfer_request(state, action, {});
  auto selection = std::make_shared<AutoTransferSelection>();
  choose_transfer_files(state, true, [selection](std::vector<std::string> uris) {
    selection->uris = std::move(uris);
    selection->ready = true;
    selection->changed.trigger();
  });
  try {
    while (!selection->ready) co_await selection->changed.wait(cancellation);
    cancellation.throw_if_cancellation_requested();
  } catch (const cardio::canceled_exception &) {
    close_transfer_file_dialog(state);
    co_return std::nullopt;
  }
  if (selection->uris.empty()) co_return std::nullopt;
  co_return make_transfer_request(state, action, std::move(selection->uris));
}

static void apply_zmodem_auto_start(ApplicationState *state) {
  elder_terms::TerminalZmodemAutoStart select;
  if (elder_terms::transfer_zmodem_autostart(state->settings_store))
    select = [state](elder_terms::TerminalTransferDirection direction, cardio::cancellation cancellation) {
      return select_auto_transfer(state, direction, cancellation);
    };
  elder_terms::terminal_runtime_set_zmodem_auto_start(state->runtime, std::move(select));
}

static elder_terms::TerminalTransferOptions make_xmodem_transfer_options(
    elder_terms::TerminalTransferXmodemPacketSize packet_size,
    elder_terms::TerminalTransferXmodemChecksumMode checksum_mode) {
  elder_terms::TerminalTransferOptions options;
  options.xmodem_packet_size = packet_size;
  options.xmodem_checksum_mode = checksum_mode;
  return options;
}

static elder_terms::TerminalTransferOptions make_ymodem_transfer_options(
    elder_terms::TerminalTransferYmodemVariant variant) {
  elder_terms::TerminalTransferOptions options;
  options.ymodem_variant = variant;
  return options;
}

static GtkWidget *create_transfer_menu_item(
    ApplicationState *state, const char *id, const char *label,
    elder_terms::TerminalTransferProtocol protocol,
    elder_terms::TerminalTransferDirection direction,
    elder_terms::TerminalTransferOptions options) {
  GtkWidget *item = gtk_menu_item_new_with_label(label);
  gestament_gtk_assign_accessible_id(item, id);
  auto *action = new TransferMenuAction{
      .protocol = protocol,
      .direction = direction,
      .options = options,
  };
  g_object_set_data_full(G_OBJECT(item), "elder-terms-transfer-action",
                         action,
                         [](gpointer data) {
                           delete static_cast<TransferMenuAction *>(data);
                         });
  g_signal_connect(item, "activate",
                   G_CALLBACK(on_transfer_menu_item_activate), state);
  return item;
}

static bool start_text_send_request(ApplicationState *state,
                                    elder_terms::TerminalTextSendSource source) {
  const std::optional<elder_terms::TerminalConnectionProfile> profile =
      elder_terms::terminal_connection_profile(state->settings_store);
  if (!profile.has_value()) {
    return false;
  }
  elder_terms::TerminalTextSendRequest request{
      .source = std::move(source),
      .text_settings = profile->text_settings,
      .follow_return_code =
          elder_terms::transfer_text_send_follow_return_code(
              state->settings_store),
      .bytes_per_second = static_cast<std::uint64_t>(
          elder_terms::transfer_text_send_bytes_per_second(
              state->settings_store)),
      .active =
          [state](bool active) {
            set_application_transfer_active(state, active);
            set_application_transfer_progress_visible(state, active);
          },
      .status =
          [state](const std::string &status) {
            elder_terms::set_main_window_status_text(state->main_window,
                                                     status);
          },
      .progress =
          [state](elder_terms::TerminalTransferProgress progress) {
            elder_terms::set_main_window_transfer_progress(state->main_window,
                                                           progress);
          },
      .finished = [state](bool succeeded) {
        if (succeeded) update_application_session_identity(state);
        else elder_terms::set_main_window_status_text(state->main_window, _("Text send failed"));
        restore_terminal_focus(state);
      },
  };
  return elder_terms::terminal_runtime_start_text_send(state->runtime,
                                                       std::move(request));
}

static bool can_paste_terminal_text(const ApplicationState *state) {
  return state != nullptr && state->window != nullptr &&
         state->connection_active && !state->transfer_active &&
         !elder_terms::has_modal_dialog(state->window);
}

static bool can_send_terminal_break(const ApplicationState *state) {
  return state != nullptr && state->window != nullptr &&
         state->connection_active && !state->transfer_active &&
         !elder_terms::has_modal_dialog(state->window) &&
         elder_terms::terminal_runtime_can_send_break(state->runtime);
}

static void request_terminal_break(ApplicationState *state) {
  if (state == nullptr) {
    return;
  }
  if (!can_send_terminal_break(state)) {
    elder_terms::set_main_window_status_text(state->main_window,
                                             _("BREAK unavailable"));
    restore_terminal_focus(state);
    return;
  }

  elder_terms::set_main_window_status_text(state->main_window,
                                           _("Sending BREAK"));
  const bool accepted = elder_terms::terminal_runtime_send_break(
      state->runtime,
      {
          .status =
              [state](std::string status) {
                elder_terms::set_main_window_status_text(
                    state->main_window, status);
              },
          .finished =
              [state](bool succeeded) {
                elder_terms::set_main_window_status_text(state->main_window,
                    succeeded ? _("BREAK sent") : _("BREAK failed"));
                restore_terminal_focus(state);
              },
      });
  if (!accepted) {
    elder_terms::set_main_window_status_text(state->main_window,
                                             _("BREAK unavailable"));
    restore_terminal_focus(state);
  }
}

static void on_text_send_menu_item_activate(GtkMenuItem *,
                                            gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state == nullptr) {
    return;
  }
  choose_transfer_files(
      state, false,
      [state](std::vector<std::string> selected_uris) {
        if (selected_uris.empty()) return;
        if (!start_text_send_request(
                state,
                elder_terms::TerminalTextSendFileSource{
                    .uri = std::move(selected_uris.front()),
                })) {
          elder_terms::set_main_window_status_text(
              state->main_window, _("Text send unavailable"));
          update_application_terminal_presentation(state);
          restore_terminal_focus(state);
        }
      });
}

static GtkWidget *create_text_send_menu_item(ApplicationState *state) {
  GtkWidget *item = gtk_menu_item_new_with_label(_("Text (Send)"));
  gestament_gtk_assign_accessible_id(item, "transfer_text_send_item");
  g_signal_connect(item, "activate",
                   G_CALLBACK(on_text_send_menu_item_activate), state);
  return item;
}

static cardio::promise<void> finish_shared_sftp_window_async(ApplicationState *state) {
  try {
    if (state->sftp_open_task) co_await *state->sftp_open_task;
    if (state->sftp_connection_check_task) co_await *state->sftp_connection_check_task;
    co_await elder_terms::close_file_transfer_window_async(state->sftp_window);
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  state->sftp_connection_check_active = false;
  state->sftp_window.reset();
  state->sftp_client.reset();
  state->sftp_transport.reset();
  state->sftp_cancel_source.reset();
  maybe_shutdown_application(state);
}

static void on_shared_sftp_window_closed(ApplicationState *state) {
  if (state == nullptr) return;
  if (state->sftp_cancel_source) (void)state->sftp_cancel_source->cancel();
  state->sftp_close_task.reset();
  state->sftp_close_task.emplace(finish_shared_sftp_window_async(state));
}

static cardio::promise<elder_terms::FileHashes>
calculate_shared_sftp_file_hashes_async(
    ApplicationState *state, std::string path,
    cardio::cancellation cancellation) {
  if (!state->test_options.fixture) {
    co_return co_await elder_terms::calculate_ssh_file_hashes_async(
        state->sftp_transport, std::move(path),
        std::move(cancellation));
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

static cardio::promise<void> reconnect_shared_sftp_window_async(
    ApplicationState *state, elder_terms::SettingsStore settings) {
  if (state->sftp_open_task) co_await *state->sftp_open_task;
  state->sftp_opening = true;
  if (state->sftp_cancel_source) (void)state->sftp_cancel_source->cancel();
  if (state->sftp_connection_check_task) co_await *state->sftp_connection_check_task;
  if (!elder_terms::file_transfer_window_widget(state->sftp_window)) {
    state->sftp_opening = false;
    co_return;
  }
  state->sftp_cancel_source.emplace();
  const auto cancellation = state->sftp_cancel_source->get_cancellation();
  state->sftp_client.reset();
  state->sftp_transport.reset();
  std::string failure;
  try {
    if (state->test_options.fixture) {
      state->sftp_client = elder_terms::create_sftp_fixture_client(state->test_options.sftp_pause_transfer);
      state->test_options.shared_sftp_disconnected = false;
    } else {
      elder_terms::TerminalSessionCallbacks callbacks{
          .activity = {}, .indicator_state = {}, .connection_phase = {},
          .ssh_prompt = [state](const elder_terms::SshUserPrompt &prompt,
                               cardio::cancellation cancellation) -> cardio::promise<elder_terms::SshUserPromptResponse> {
            elder_terms::InlinePromptRequest request{
                .title = prompt.title.empty() ? _("SSH") : prompt.title,
                .message = prompt.message, .monospace_message = prompt.monospace_message,
                .accept_label = prompt.kind == elder_terms::SshUserPromptKind::host_key ? _("Accept")
                    : prompt.kind == elder_terms::SshUserPromptKind::username ? _("Connect") : _("OK"),
                .cancel_label = _("Cancel"), .initial_text = prompt.initial_text,
                .input_required = prompt.input_required, .echo = prompt.echo,
                .cancel_visible = true, .accept_visible = prompt.accept_visible,
                .alternative_label = _("Reset and Connect"), .alternative_visible = prompt.host_key_reset_available};
            auto response = co_await elder_terms::prompt_file_transfer_window_async(
                state->sftp_window, std::move(request), cancellation);
            co_return elder_terms::SshUserPromptResponse{
                .accepted = response.accepted, .text = std::move(response.text),
                .reset_host_key = response.alternative};
          }};
      elder_terms::AuthenticatedSshTransportOptions options{
          .known_hosts_file = state->test_options.ssh_known_hosts_file, .config_file = {},
          .proxy = elder_terms::ssh_proxy_connection_settings(settings)};
      state->sftp_transport = co_await elder_terms::AuthenticatedSshTransport::connect_async(
          elder_terms::ssh_endpoint_settings(settings), callbacks, options, cancellation);
      state->sftp_client = co_await elder_terms::open_sftp_client_async(state->sftp_transport, cancellation);
    }
    cancellation.throw_if_cancellation_requested();
    elder_terms::attach_file_transfer_window_client(state->sftp_window, state->sftp_client);
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) { failure = error.what(); }
  if (!failure.empty()) co_await elder_terms::show_file_transfer_window_connection_error_async(
      state->sftp_window, _("Failed to start SFTP"), std::move(failure), cancellation);
  state->sftp_opening = false;
}

static void create_shared_sftp_window(ApplicationState *state) {
  const elder_terms::SftpConnectionSettings settings =
      elder_terms::sftp_connection_settings(state->settings_store);
  state->sftp_window = elder_terms::create_file_transfer_window(
      {
          .connection_name =
              elder_terms::general_connection_name(state->settings_store),
          .protocol_name = "SFTP",
          .local_directory =
              elder_terms::resolve_file_transfer_local_directory(
                  state->settings_store, settings.local_directory),
          .remote_directory = settings.remote_directory,
          .remote_file_hash =
              [state](std::string path,
                      cardio::cancellation cancellation) {
                return calculate_shared_sftp_file_hashes_async(
                    state, std::move(path), std::move(cancellation));
              },
          .colors =
              elder_terms::general_color_settings(state->settings_store),
          .closed =
              [state]() {
                on_shared_sftp_window_closed(state);
              },
          .settings = state->settings_store,
          .config_path = state->config_path,
          .reconnect = [state](elder_terms::SettingsStore settings) {
            return reconnect_shared_sftp_window_async(state, std::move(settings));
          },
      });
  elder_terms::show_file_transfer_window(state->sftp_window);
}

static cardio::promise<void> open_shared_sftp_window_async(
    ApplicationState *state,
    std::shared_ptr<elder_terms::AuthenticatedSshTransport> transport,
    cardio::cancellation cancellation) {
  std::string failure;
  try {
    std::shared_ptr<elder_terms::RemoteFileClient> client;
    if (state->test_options.fixture) {
      client = elder_terms::create_sftp_fixture_client(
          state->test_options.sftp_pause_transfer);
    } else {
      client = co_await elder_terms::open_sftp_client_async(
          transport, cancellation);
    }
    cancellation.throw_if_cancellation_requested();
    state->sftp_transport = std::move(transport);
    state->sftp_client = std::move(client);
    elder_terms::attach_file_transfer_window_client(
        state->sftp_window, state->sftp_client);
    state->sftp_opening = false;
    if (state->test_options.focus_transfer_on_sftp_open &&
        state->window != nullptr) {
      gtk_window_set_focus(
          GTK_WINDOW(state->window), state->main_window->transfer_button);
    }
    if (state->test_options.shared_sftp_disconnected ||
        state->connection_phase ==
            elder_terms::TerminalSessionConnectionPhase::disconnected) {
      start_shared_sftp_connection_check(state);
    }
    co_return;
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) {
    failure = error.what();
    std::cerr << "Warning: failed to open shared SFTP window: "
              << failure << '\n';
    if (state->window != nullptr) {
      elder_terms::set_main_window_status_text(
          state->main_window, _("SFTP unavailable"));
    }
  }

  state->sftp_opening = false;
  state->sftp_client.reset();
  if (!failure.empty() && state->sftp_window != nullptr) {
    co_await elder_terms::show_file_transfer_window_connection_error_async(
        state->sftp_window, _("Failed to start SFTP"), std::move(failure),
        std::move(cancellation));
  }
  maybe_shutdown_application(state);
}

static void on_sftp_menu_item_activate(GtkMenuItem *,
                                       gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state == nullptr) {
    return;
  }
  if (state->sftp_window != nullptr) {
    elder_terms::present_file_transfer_window(state->sftp_window);
    return;
  }
  if (state->sftp_opening || !state->connection_active) {
    return;
  }

  std::shared_ptr<elder_terms::AuthenticatedSshTransport> transport;
  if (!state->test_options.fixture) {
    transport =
        elder_terms::terminal_runtime_authenticated_ssh(state->runtime);
    if (transport == nullptr) {
      elder_terms::set_main_window_status_text(
          state->main_window, _("SFTP unavailable"));
      return;
    }
  }

  if (state->sftp_cancel_source.has_value()) {
    (void)state->sftp_cancel_source->cancel();
  }
  state->sftp_cancel_source = cardio::cancellation_source{};
  state->sftp_transport = transport;
  state->sftp_opening = true;
  create_shared_sftp_window(state);
  state->sftp_open_task.reset();
  state->sftp_open_task.emplace(
      open_shared_sftp_window_async(
          state, std::move(transport),
          state->sftp_cancel_source->get_cancellation()));
}

static GtkWidget *create_sftp_menu_item(ApplicationState *state) {
  GtkWidget *item = gtk_menu_item_new_with_label(_("SFTP"));
  gestament_gtk_assign_accessible_id(item, "transfer_sftp_item");
  g_signal_connect(item, "activate",
                   G_CALLBACK(on_sftp_menu_item_activate), state);
  return item;
}

static void on_log_enabled_menu_item_toggled(GtkCheckMenuItem *item,
                                             gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state == nullptr) {
    return;
  }

  elder_terms::set_setting_value(
      &state->settings_store, elder_terms::terminal_log_enabled_setting_key(),
      elder_terms::SettingValue{
          gtk_check_menu_item_get_active(item) != FALSE});
  elder_terms::apply_terminal_log_settings(
      state->log_state,
      elder_terms::terminal_log_settings(state->settings_store));
}

static GtkWidget *create_log_enabled_menu_item(ApplicationState *state) {
  GtkWidget *item = gtk_check_menu_item_new_with_label(_("Log recording"));
  gestament_gtk_assign_accessible_id(item, "transfer_log_enabled_item");
  gtk_check_menu_item_set_active(
      GTK_CHECK_MENU_ITEM(item),
      elder_terms::terminal_log_settings(state->settings_store).enabled ? TRUE
                                                                       : FALSE);
  g_signal_connect(item, "toggled",
                   G_CALLBACK(on_log_enabled_menu_item_toggled), state);
  state->log_enabled_menu_item = item;
  return item;
}

static void install_transfer_menu(ApplicationState *state) {
  GtkWidget *menu = gtk_menu_new();

  gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                        create_log_enabled_menu_item(state));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                        gtk_separator_menu_item_new());

  if (elder_terms::general_settings_select_ssh_connection(
          state->settings_store)) {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          create_sftp_menu_item(state));
  }

  gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                        create_text_send_menu_item(state));

  if (state->active_profile.kind == elder_terms::TerminalConnectionKind::local_shell) {
    gtk_widget_show_all(menu);
    gtk_menu_button_set_popup(
        GTK_MENU_BUTTON(state->main_window->transfer_button), menu);
    return;
  }

  gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                        gtk_separator_menu_item_new());

  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_zmodem_send_item", _("ZMODEM (send)"),
          elder_terms::TerminalTransferProtocol::zmodem,
          elder_terms::TerminalTransferDirection::send,
          elder_terms::TerminalTransferOptions{}));
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_ymodem_send_item", _("YMODEM (send)"),
          elder_terms::TerminalTransferProtocol::ymodem,
          elder_terms::TerminalTransferDirection::send,
          elder_terms::TerminalTransferOptions{}));
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_xmodem_1k_send_item", _("XMODEM 1K (send)"),
          elder_terms::TerminalTransferProtocol::xmodem,
          elder_terms::TerminalTransferDirection::send,
          make_xmodem_transfer_options(
              elder_terms::TerminalTransferXmodemPacketSize::bytes_1024,
              elder_terms::TerminalTransferXmodemChecksumMode::automatic)));
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_xmodem_send_item", _("XMODEM (send)"),
          elder_terms::TerminalTransferProtocol::xmodem,
          elder_terms::TerminalTransferDirection::send,
          make_xmodem_transfer_options(
              elder_terms::TerminalTransferXmodemPacketSize::bytes_128,
              elder_terms::TerminalTransferXmodemChecksumMode::automatic)));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_zmodem_receive_item", _("ZMODEM (receive)"),
          elder_terms::TerminalTransferProtocol::zmodem,
          elder_terms::TerminalTransferDirection::receive,
          elder_terms::TerminalTransferOptions{}));
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_ymodem_g_receive_item", _("YMODEM-g (receive)"),
          elder_terms::TerminalTransferProtocol::ymodem,
          elder_terms::TerminalTransferDirection::receive,
          make_ymodem_transfer_options(
              elder_terms::TerminalTransferYmodemVariant::g)));
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_ymodem_receive_item", _("YMODEM (receive)"),
          elder_terms::TerminalTransferProtocol::ymodem,
          elder_terms::TerminalTransferDirection::receive,
          make_ymodem_transfer_options(
              elder_terms::TerminalTransferYmodemVariant::standard)));
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_xmodem_crc_receive_item",
          _("XMODEM CRC (receive)"),
          elder_terms::TerminalTransferProtocol::xmodem,
          elder_terms::TerminalTransferDirection::receive,
          make_xmodem_transfer_options(
              elder_terms::TerminalTransferXmodemPacketSize::bytes_128,
              elder_terms::TerminalTransferXmodemChecksumMode::crc)));
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_xmodem_receive_item", _("XMODEM (receive)"),
          elder_terms::TerminalTransferProtocol::xmodem,
          elder_terms::TerminalTransferDirection::receive,
          make_xmodem_transfer_options(
              elder_terms::TerminalTransferXmodemPacketSize::bytes_128,
              elder_terms::TerminalTransferXmodemChecksumMode::checksum)));
  gtk_widget_show_all(menu);
  gtk_menu_button_set_popup(GTK_MENU_BUTTON(state->main_window->transfer_button),
                            menu);
}

static bool select_new_window_connection(elder_terms::LaunchOptions *options) {
  if (!options->new_window || options->config_path.has_value() ||
      options->startup_config_path.has_value()) {
    return true;
  }
  const auto global_path = elder_terms::default_global_config_path();
  const auto global = elder_terms::load_global_settings(global_path, 12.0);
  const auto name = elder_terms::application_new_window_connection(global.store);
  if (name.empty()) {
    return true;
  }
  // A connection name is a repository file stem, never an arbitrary path.
  if (std::filesystem::path(name).filename() != name || name == "." ||
      name == ".." || name.find('\\') != std::string::npos) {
    std::cerr << "Error: invalid New Window connection name: " << name << '\n';
    return false;
  }
  options->config_path = global_path.parent_path() / "connections" /
                         (name + ".ini");
  return true;
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
  auto launch_options =
    elder_terms::parse_launch_options(&argc, argv);
  if (!select_new_window_connection(&launch_options)) {
    return 1;
  }
  gtk_init(&argc, &argv);
  g_set_prgname(terminal_application_id);
  (void)elder_terms::initialize_application_window_icon();

  const double default_font_size_dip = 12.0;
  const elder_terms::SettingsLoadOptions settings_load_options{
      .config_path = launch_options.config_path,
      .startup_config_path = launch_options.startup_config_path,
      .global_config_path = elder_terms::default_global_config_path(),
  };
  auto settings_result =
    elder_terms::load_settings(settings_load_options, default_font_size_dip);

  for (const std::string &warning : settings_result.warnings) {
    std::cerr << warning << '\n';
  }

  if (launch_options.new_window && !settings_result.loaded) {
    std::cerr << "Error: failed to load the New Window connection\n";
    return 1;
  }

  const std::optional<elder_terms::TerminalConnectionProfile>
      connection_profile =
          elder_terms::terminal_connection_profile(settings_result.store);
  if (!connection_profile.has_value()) {
    std::cerr << "Error: configured connection type is not a terminal "
                 "connection\n";
    return 1;
  }
  (void)elder_terms::initialize_connection_desktop_session(
      &settings_result.store, launch_options.config_path,
      launch_options.startup_config_path, argv[0]);

  cardio::dispatcher_group_glib dispatcher_group;
  // Let GLib prioritize ready I/O alongside native events. Pumping GTK before
  // every continuation repeatedly renders partially received output batches.
  cardio::dispatcher_host_glib_auto dispatcher(dispatcher_group);

  auto main_window = elder_terms::load_main_window(
      elder_terms::terminal_ambiguous_width(settings_result.store));
  if (!main_window.has_value()) {
    return 1;
  }
  elder_terms::set_main_window_activity_indicators_latched(
      &*main_window, launch_options.test.latch_activity_indicators);
  elder_terms::set_main_window_colors(
      &*main_window,
      elder_terms::general_color_settings(settings_result.store));
  elder_terms::set_main_window_indicator_color(
      &*main_window, elder_terms::general_indicator_color(settings_result.store),
      elder_terms::general_indicator_off_color(settings_result.store));
  elder_terms::set_connection_window_icon(GTK_WINDOW(main_window->window),
      GTK_IMAGE(main_window->header_connection_icon),
      elder_terms::general_connection_icon(settings_result.store), 24);
  elder_terms::set_main_window_compact_mode(
      &*main_window, elder_terms::general_compact_mode(settings_result.store));

  ApplicationState app_state;
  app_state.main_window = &*main_window;
  app_state.runtime = main_window->runtime;
  app_state.active_profile = *connection_profile;
  app_state.dispatcher_group = &dispatcher_group;
  app_state.settings_store = settings_result.store;
  if (main_window->compact_mode) {
    elder_terms::set_connection_window_icon(GTK_WINDOW(main_window->window),
        GTK_IMAGE(main_window->header_connection_icon),
        elder_terms::general_connection_icon(app_state.settings_store), 18);
  }
  app_state.config_path = launch_options.config_path;
  app_state.test_options = launch_options.test;
  app_state.auto_close = elder_terms::general_auto_close(settings_result.store);
  app_state.window = main_window->window;

  app_state.bell_player = elder_terms::create_terminal_bell_player();
  app_state.bell_state = elder_terms::create_terminal_bell({
      .set_audible = [&app_state](bool audible) { app_state.audible_bell = audible; },
      .cancel_playback = [&app_state]() {
        elder_terms::cancel_terminal_bell_playback(app_state.bell_player);
      },
      .play_file = [&app_state](const std::filesystem::path &sound_file) {
        return elder_terms::play_terminal_bell_file(app_state.bell_player,
                                                    sound_file);
      },
      .warning = [](std::string message) {
        std::cerr << "Warning: " << message << '\n';
      },
  });
  elder_terms::apply_terminal_bell_settings(
      app_state.bell_state,
      elder_terms::terminal_bell_settings(app_state.settings_store));

  app_state.log_state = elder_terms::create_terminal_log({
      .settings = elder_terms::terminal_log_settings(app_state.settings_store),
      .now = {},
      .active = [&app_state](bool active) {
        set_application_indicator_state(
            &app_state, elder_terms::ActivityIndicatorId::log, active);
      },
      .warning = {},
  });

  elder_terms::set_main_window_activity_indicator_connection_kind(&*main_window, connection_profile->kind);
  app_state.presentation = elder_terms::create_terminal_presentation(&*main_window, launch_options.test,
      [&app_state](elder_terms::TerminalDisplaySettings display) {
        update_runtime_terminal_display_settings(&app_state, display);
      }, [&app_state] { request_terminal_break(&app_state); });
  elder_terms::terminal_runtime_set_host_callbacks(app_state.runtime, {
      [&app_state](BoxrigGtk3TerminalWidget *) {
        elder_terms::attach_main_window_terminal(app_state.main_window);
        elder_terms::attach_terminal_presentation(app_state.presentation);
      },
      [&app_state](std::string title) {
        app_state.terminal_title = std::move(title);
        update_application_session_identity(&app_state);
      },
      [&app_state] {
        if (app_state.audible_bell) gdk_display_beep(gtk_widget_get_display(app_state.main_window->terminal));
        else elder_terms::ring_terminal_bell(app_state.bell_state);
      },
      [&app_state](const boxrig_mouse_event *event, const boxrig_ui_hit_test_result *hit) {
        const auto route = elder_terms::route_main_window_terminal_mouse(app_state.main_window, *event, *hit);
        return route == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL && app_state.presentation ?
            elder_terms::route_terminal_presentation_mouse(app_state.presentation, *event) : route;
      }});
  elder_terms::terminal_runtime_set_changed_callback(app_state.runtime, application_terminal_changed, &app_state);
  elder_terms::terminal_runtime_set_output_callback(app_state.runtime,
      [&app_state](std::span<const unsigned char> raw, std::span<const unsigned char> cooked) {
        // Ready I/O can arrive before the next frame observes the connection.
        // Start logging before accepting its first received chunk.
        if (!app_state.closing && app_state.connection_phase !=
            elder_terms::TerminalSessionConnectionPhase::connected) {
          const auto connection = elder_terms::terminal_runtime_connection_state(app_state.runtime);
          if (connection.phase == elder_terms::TerminalConnectionPhase::connected &&
              connection.carrier_connected)
            set_application_connection_phase(
                &app_state, elder_terms::TerminalSessionConnectionPhase::connected);
        }
        elder_terms::write_terminal_log(app_state.log_state, raw, cooked);
      });
  (void)elder_terms::apply_terminal_presentation(app_state.presentation, app_state.settings_store, nullptr);
  elder_terms::terminal_runtime_request_input_settings(app_state.runtime, connection_profile->text_settings);
  apply_received_macros(&app_state);
  apply_terminal_hyperlink_settings(&app_state);
  elder_terms::set_main_window_terminal_paste_callbacks(
      &*main_window,
      {
          .can_paste =
              [&app_state]() {
                return can_paste_terminal_text(&app_state);
              },
          .paste =
              [&app_state](std::string utf8_text) {
                if (!start_text_send_request(
                        &app_state,
                        elder_terms::TerminalTextSendBufferSource{
                            .utf8_text = std::move(utf8_text),
                        })) {
                  elder_terms::set_main_window_status_text(
                      app_state.main_window, _("Text send unavailable"));
                }
              },
      });
  elder_terms::set_main_window_terminal_break_callbacks(
      &*main_window,
      {
          .can_send =
              [&app_state]() {
                return can_send_terminal_break(&app_state);
              },
          .send =
              [&app_state]() {
                request_terminal_break(&app_state);
              },
      });
  elder_terms::set_main_window_transfer_cancel_callback(
      &*main_window, [&app_state]() {
        return elder_terms::terminal_runtime_cancel_transfer(app_state.runtime);
      });
  apply_zmodem_auto_start(&app_state);
  install_transfer_menu(&app_state);
  elder_terms::set_main_window_transfer_button_visible(&*main_window, true);
  update_application_terminal_presentation(&app_state);
  update_application_session_identity(&app_state);

  g_signal_connect(main_window->window, "delete-event", G_CALLBACK(on_main_window_delete), &app_state);

  g_signal_connect(
    main_window->window, "destroy",
    G_CALLBACK(on_main_window_destroy), &app_state);
  g_signal_connect(main_window->reconnect_button, "clicked",
                   G_CALLBACK(on_reconnect_clicked), &app_state);
  g_signal_connect_after(
    main_window->window, "focus-in-event",
    G_CALLBACK(on_main_window_focus_in), &app_state);
  g_signal_connect(app_state.window, "notify::sensitive",
      G_CALLBACK(on_parent_sensitivity_changed), &app_state);
  g_signal_connect(
    main_window->settings_menu_item, "activate",
    G_CALLBACK(on_settings_menu_item_activate), &app_state);
  g_signal_connect(
    main_window->about_menu_item, "activate",
    G_CALLBACK(on_about_menu_item_activate), &app_state);

  bool session_started = false;
  if (!launch_options.test.fixture) {
    app_state.connect_when_ready = true;
    session_started = true;
    set_application_connection_phase(&app_state, elder_terms::TerminalSessionConnectionPhase::connecting);
  }
  if (launch_options.test.fixture) {
    set_application_indicator_state(&app_state,
                                    elder_terms::ActivityIndicatorId::conn,
                                    true);
  } else if (!session_started) {
    set_application_connection_phase(
        &app_state,
        elder_terms::TerminalSessionConnectionPhase::disconnected);
  }

  gtk_widget_show_all(main_window->window);
  gtk_window_present_with_time(GTK_WINDOW(main_window->window), GDK_CURRENT_TIME);
  elder_terms::attach_tracked_window(main_window->window,
      elder_terms::present_tracked_window);
  if (launch_options.test.show_transfer_progress) {
    elder_terms::set_main_window_transfer_progress_visible(
        &*main_window, true);
  }
  elder_terms::focus_main_window_terminal_if_interactive(
      &*main_window);

  elder_terms::terminal_runtime_refresh(app_state.runtime);
  if (launch_options.test.ssh_prompt.has_value()) {
    app_state.ssh_prompt_fixture_task.emplace(
        run_ssh_prompt_fixture_async(
            &app_state, *launch_options.test.ssh_prompt));
  }

  if (!elder_terms::terminal_background_image_settings(app_state.settings_store).path.empty())
    elder_terms::prepare_terminal_presentation_background(app_state.presentation, app_state.settings_store,
      [&app_state] { return !app_state.closing; },
      [&app_state](elder_terms::TerminalBackgroundImagePtr image, std::string error) {
        // Missing startup images keep the stored path and an operable terminal.
        // The background status exposes the error when Settings is opened.
        if (error.empty() && !apply_runtime_settings(&app_state, app_state.settings_store, std::move(image)))
          std::cerr << "Failed to apply startup background image\n";
        update_settings_renderer_status(&app_state);
      });
  dispatcher.park();

  if (app_state.sftp_cancel_source.has_value()) {
    (void)app_state.sftp_cancel_source->cancel();
  }
  app_state.sftp_connection_check_task.reset();
  app_state.sftp_open_task.reset();
  app_state.sftp_close_task.reset();
  app_state.sftp_window.reset();
  app_state.sftp_client.reset();
  app_state.sftp_transport.reset();
  app_state.sftp_cancel_source.reset();
  app_state.ssh_prompt_fixture_task.reset();
  if (app_state.runtime) {
    elder_terms::terminal_runtime_set_changed_callback(app_state.runtime, nullptr, nullptr);
    elder_terms::terminal_runtime_set_host_callbacks(app_state.runtime, {});
  }
  app_state.shutdown_task.reset();
  elder_terms::destroy_terminal_log(app_state.log_state);
  elder_terms::destroy_terminal_bell(app_state.bell_state);
  elder_terms::destroy_terminal_bell_player(app_state.bell_player);
  elder_terms::destroy_terminal_presentation(app_state.presentation);
  elder_terms::release_main_window(&*main_window);

  return 0;
}

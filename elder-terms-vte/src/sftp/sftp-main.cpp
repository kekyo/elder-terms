#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <gestament/gtk.h>
#include <gtk/gtk.h>

#include <cardio.h>
#include <elder-terms/settings.h>

#include "../launch-options.h"
#include "../terminal-session-callbacks.h"
#include "../terminal-sessions/ssh-session/authenticated-ssh-transport.h"
#include "sftp-client.h"
#include "sftp-fixture-client.h"
#include "sftp-paths.h"
#include "sftp-window.h"

struct SftpAuthenticationPromptRequest {
  GtkWidget *dialog = nullptr;
  GtkWidget *entry = nullptr;
  bool input_required = false;
  std::shared_ptr<
      cardio::promise_source<elder_terms::SshUserPromptResponse>>
      source;
  cardio::cancellation_registration cancellation_registration;
  bool completed = false;
};

struct SftpApplicationState {
  cardio::dispatcher_group_glib *dispatcher_group = nullptr;
  elder_terms::LaunchOptions launch_options;
  elder_terms::SettingsStore settings;
  elder_terms::SftpConnectionSettings connection;
  std::shared_ptr<elder_terms::AuthenticatedSshTransport> transport;
  std::shared_ptr<elder_terms::SftpClient> client;
  std::shared_ptr<elder_terms::SftpWindow> window;
  cardio::cancellation_source stop_source;
  std::optional<cardio::promise<void>> startup_task;
  GtkWidget *startup_error_dialog = nullptr;
  bool shutting_down = false;
};

static void complete_authentication_prompt(
    SftpAuthenticationPromptRequest *request, bool accepted) {
  if (request == nullptr || request->completed) {
    return;
  }
  request->completed = true;
  request->cancellation_registration = {};
  std::string text;
  if (accepted && request->input_required &&
      request->entry != nullptr) {
    const char *value =
        gtk_entry_get_text(GTK_ENTRY(request->entry));
    text = value == nullptr ? std::string() : std::string(value);
  }
  if (request->dialog != nullptr) {
    GtkWidget *dialog = std::exchange(request->dialog, nullptr);
    g_signal_handlers_disconnect_by_data(dialog, request);
    gtk_widget_destroy(dialog);
  }
  (void)request->source->try_resolve(
      {
          .accepted = accepted,
          .text = std::move(text),
      });
}

static void on_authentication_prompt_response(
    GtkDialog *, gint response, gpointer data) {
  complete_authentication_prompt(
      static_cast<SftpAuthenticationPromptRequest *>(data),
      response == GTK_RESPONSE_ACCEPT);
}

static gboolean cancel_authentication_prompt_idle(gpointer data) {
  auto *weak = static_cast<
      std::weak_ptr<SftpAuthenticationPromptRequest> *>(data);
  const std::shared_ptr<SftpAuthenticationPromptRequest> request =
      weak->lock();
  delete weak;
  if (request != nullptr) {
    complete_authentication_prompt(request.get(), false);
  }
  return G_SOURCE_REMOVE;
}

static cardio::promise<elder_terms::SshUserPromptResponse>
prompt_sftp_authentication_async(
    const elder_terms::SshUserPrompt &prompt,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  auto request =
      std::make_shared<SftpAuthenticationPromptRequest>();
  request->source = std::make_shared<
      cardio::promise_source<elder_terms::SshUserPromptResponse>>();
  request->input_required = prompt.input_required;
  request->dialog = gtk_dialog_new_with_buttons(
      prompt.title.c_str(), nullptr,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL), "Cancel",
      GTK_RESPONSE_CANCEL,
      prompt.input_required ? "Continue" : "Accept",
      GTK_RESPONSE_ACCEPT, nullptr);
  gestament_gtk_assign_accessible_id(
      request->dialog, "sftp_ssh_prompt_dialog");
  gtk_window_set_default_size(
      GTK_WINDOW(request->dialog), 440, -1);
  GtkWidget *content = gtk_dialog_get_content_area(
      GTK_DIALOG(request->dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 14);
  GtkWidget *message = gtk_label_new(prompt.message.c_str());
  gestament_gtk_assign_accessible_id(
      message, "sftp_ssh_prompt_message");
  gtk_label_set_xalign(GTK_LABEL(message), 0.0F);
  gtk_label_set_line_wrap(GTK_LABEL(message), TRUE);
  gtk_label_set_selectable(GTK_LABEL(message), TRUE);
  gtk_box_pack_start(GTK_BOX(content), message, TRUE, TRUE, 0);
  if (prompt.input_required) {
    request->entry = gtk_entry_new();
    gestament_gtk_assign_accessible_id(
        request->entry, "sftp_ssh_prompt_entry");
    gtk_entry_set_visibility(
        GTK_ENTRY(request->entry), prompt.echo ? TRUE : FALSE);
    gtk_box_pack_start(
        GTK_BOX(content), request->entry, FALSE, TRUE, 8);
  }
  g_signal_connect(
      request->dialog, "response",
      G_CALLBACK(on_authentication_prompt_response), request.get());
  request->cancellation_registration =
      cancellation.on_cancellation_requested(
          [weak = std::weak_ptr<SftpAuthenticationPromptRequest>(
               request)]() {
            g_idle_add(
                cancel_authentication_prompt_idle,
                new std::weak_ptr<SftpAuthenticationPromptRequest>(
                    weak));
          });
  gtk_widget_show_all(request->dialog);
  if (request->entry != nullptr) {
    gtk_widget_grab_focus(request->entry);
  }
  elder_terms::SshUserPromptResponse response =
      co_await request->source->get_promise();
  request->cancellation_registration = {};
  co_return response;
}

static void stop_sftp_application(SftpApplicationState *state) {
  if (state == nullptr || state->shutting_down) {
    return;
  }
  state->shutting_down = true;
  (void)state->stop_source.cancel();
  state->dispatcher_group->shutdown();
}

static void on_startup_error_response(
    GtkDialog *dialog, gint, gpointer data) {
  auto *state = static_cast<SftpApplicationState *>(data);
  state->startup_error_dialog = nullptr;
  gtk_widget_destroy(GTK_WIDGET(dialog));
  stop_sftp_application(state);
}

static void show_startup_error(
    SftpApplicationState *state, const std::string &message) {
  std::cerr << message << '\n';
  state->startup_error_dialog = gtk_message_dialog_new(
      nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
      GTK_BUTTONS_CLOSE, "%s", "Failed to start SFTP");
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(state->startup_error_dialog), "%s",
      message.c_str());
  gestament_gtk_assign_accessible_id(
      state->startup_error_dialog, "sftp_startup_error_dialog");
  g_signal_connect(
      state->startup_error_dialog, "response",
      G_CALLBACK(on_startup_error_response), state);
  gtk_widget_show_all(state->startup_error_dialog);
}

static void open_sftp_application_window(
    SftpApplicationState *state) {
  state->window = elder_terms::create_sftp_window(
      {
          .connection_name =
              elder_terms::general_connection_name(state->settings),
          .local_directory =
              elder_terms::resolve_sftp_local_directory(
                  state->settings, state->connection),
          .remote_directory = state->connection.remote_directory,
          .client = state->client,
          .closed =
              [state]() {
                stop_sftp_application(state);
              },
      });
  elder_terms::show_sftp_window(state->window);
}

static cardio::promise<void>
start_sftp_application_async(SftpApplicationState *state) {
  try {
    const cardio::cancellation cancellation =
        state->stop_source.get_cancellation();
    elder_terms::TerminalSessionCallbacks callbacks{
        .ended = {},
        .activity = {},
        .indicator_state = {},
        .connection_phase = {},
        .output = {},
        .zmodem_auto_start = {},
        .ssh_prompt =
            [](const elder_terms::SshUserPrompt &prompt,
               cardio::cancellation prompt_cancellation) {
              return prompt_sftp_authentication_async(
                  prompt, std::move(prompt_cancellation));
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
    open_sftp_application_window(state);
  } catch (const cardio::canceled_exception &) {
    stop_sftp_application(state);
  } catch (const std::exception &exception) {
    if (!state->shutting_down) {
      show_startup_error(state, exception.what());
    }
  }
}

int main(int argc, char **argv) {
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

  if (state.launch_options.test.fixture) {
    state.client = elder_terms::create_sftp_fixture_client(
        state.launch_options.test.sftp_pause_transfer);
    open_sftp_application_window(&state);
  } else {
    state.startup_task.emplace(
        start_sftp_application_async(&state));
  }

  dispatcher.park();
  (void)state.stop_source.cancel();
  state.window.reset();
  state.client.reset();
  state.transport.reset();
  state.startup_task.reset();
  return 0;
}

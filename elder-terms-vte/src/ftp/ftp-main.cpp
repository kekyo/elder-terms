#include <clocale>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <gestament/gtk.h>
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

struct FtpPasswordPromptRequest {
  GtkWidget *dialog = nullptr;
  GtkWidget *entry = nullptr;
  std::shared_ptr<cardio::promise_source<std::optional<std::string>>> source;
  cardio::cancellation_registration cancellation_registration;
  bool completed = false;
};

struct FtpApplicationState {
  cardio::dispatcher_group_glib *dispatcher_group = nullptr;
  elder_terms::SettingsStore settings;
  elder_terms::FtpConnectionSettings connection;
  std::shared_ptr<elder_terms::RemoteFileClient> client;
  std::shared_ptr<elder_terms::FileTransferWindow> window;
  cardio::cancellation_source stop_source;
  std::optional<cardio::promise<void>> startup_task;
  GtkWidget *startup_error_dialog = nullptr;
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

static void complete_password_prompt(FtpPasswordPromptRequest *request,
                                     bool accepted) {
  if (request == nullptr || request->completed) {
    return;
  }
  request->completed = true;
  request->cancellation_registration = {};
  std::optional<std::string> password;
  if (accepted && request->entry != nullptr) {
    const char *value = gtk_entry_get_text(GTK_ENTRY(request->entry));
    password.emplace(value == nullptr ? std::string() : std::string(value));
  }
  if (request->dialog != nullptr) {
    GtkWidget *dialog = std::exchange(request->dialog, nullptr);
    g_signal_handlers_disconnect_by_data(dialog, request);
    gtk_widget_destroy(dialog);
  }
  (void)request->source->try_resolve(std::move(password));
}

static void on_password_prompt_response(GtkDialog *, gint response,
                                        gpointer data) {
  complete_password_prompt(
      static_cast<FtpPasswordPromptRequest *>(data),
      response == GTK_RESPONSE_ACCEPT);
}

static gboolean cancel_password_prompt_idle(gpointer data) {
  auto *weak =
      static_cast<std::weak_ptr<FtpPasswordPromptRequest> *>(data);
  const std::shared_ptr<FtpPasswordPromptRequest> request = weak->lock();
  delete weak;
  if (request != nullptr) {
    complete_password_prompt(request.get(), false);
  }
  return G_SOURCE_REMOVE;
}

static cardio::promise<std::optional<std::string>>
prompt_ftp_password_async(const elder_terms::FtpConnectionSettings &connection,
                          cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  auto request = std::make_shared<FtpPasswordPromptRequest>();
  request->source = std::make_shared<
      cardio::promise_source<std::optional<std::string>>>();
  request->dialog = gtk_dialog_new_with_buttons(
      _("FTP authentication"), nullptr,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL), _("Cancel"),
      GTK_RESPONSE_CANCEL, _("Connect"), GTK_RESPONSE_ACCEPT, nullptr);
  gestament_gtk_assign_accessible_id(request->dialog,
                                     "ftp_password_dialog");
  gtk_window_set_default_size(GTK_WINDOW(request->dialog), 440, -1);
  GtkWidget *content =
      gtk_dialog_get_content_area(GTK_DIALOG(request->dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 14);
  const std::string prompt = format_message(
      _("Enter the password for %s on %s."), connection.username,
      connection.address);
  GtkWidget *message = gtk_label_new(prompt.c_str());
  gestament_gtk_assign_accessible_id(message, "ftp_password_message");
  gtk_label_set_xalign(GTK_LABEL(message), 0.0F);
  gtk_label_set_line_wrap(GTK_LABEL(message), TRUE);
  gtk_box_pack_start(GTK_BOX(content), message, TRUE, TRUE, 0);
  request->entry = gtk_entry_new();
  gestament_gtk_assign_accessible_id(request->entry,
                                     "ftp_password_entry");
  gtk_entry_set_visibility(GTK_ENTRY(request->entry), FALSE);
  gtk_entry_set_activates_default(GTK_ENTRY(request->entry), TRUE);
  gtk_box_pack_start(GTK_BOX(content), request->entry, FALSE, TRUE, 8);
  gtk_dialog_set_default_response(GTK_DIALOG(request->dialog),
                                  GTK_RESPONSE_ACCEPT);
  g_signal_connect(request->dialog, "response",
                   G_CALLBACK(on_password_prompt_response), request.get());
  request->cancellation_registration =
      cancellation.on_cancellation_requested(
          [weak = std::weak_ptr<FtpPasswordPromptRequest>(request)]() {
            g_idle_add(
                cancel_password_prompt_idle,
                new std::weak_ptr<FtpPasswordPromptRequest>(weak));
          });
  gtk_widget_show_all(request->dialog);
  gtk_widget_grab_focus(request->entry);
  std::optional<std::string> result =
      std::move(co_await request->source->get_promise());
  request->cancellation_registration = {};
  co_return result;
}

static void stop_ftp_application(FtpApplicationState *state) {
  if (state == nullptr || state->shutting_down) {
    return;
  }
  state->shutting_down = true;
  (void)state->stop_source.cancel();
  state->dispatcher_group->shutdown();
}

static void on_startup_error_response(GtkDialog *dialog, gint,
                                      gpointer data) {
  auto *state = static_cast<FtpApplicationState *>(data);
  state->startup_error_dialog = nullptr;
  gtk_widget_destroy(GTK_WIDGET(dialog));
  stop_ftp_application(state);
}

static void show_startup_error(FtpApplicationState *state,
                               const std::string &message) {
  std::cerr << message << '\n';
  state->startup_error_dialog = gtk_message_dialog_new(
      nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s",
      _("Failed to start FTP"));
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(state->startup_error_dialog), "%s",
      message.c_str());
  gestament_gtk_assign_accessible_id(state->startup_error_dialog,
                                     "ftp_startup_error_dialog");
  g_signal_connect(state->startup_error_dialog, "response",
                   G_CALLBACK(on_startup_error_response), state);
  gtk_widget_show_all(state->startup_error_dialog);
}

static void open_ftp_application_window(FtpApplicationState *state) {
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
          .client = state->client,
          .closed =
              [state]() {
                stop_ftp_application(state);
              },
      });
  elder_terms::show_file_transfer_window(state->window);
}

static cardio::promise<void>
start_ftp_application_async(FtpApplicationState *state) {
  try {
    const cardio::cancellation cancellation =
        state->stop_source.get_cancellation();
    std::string password;
    if (!state->connection.username.empty()) {
      std::optional<std::string> prompted =
          co_await prompt_ftp_password_async(state->connection, cancellation);
      if (!prompted.has_value()) {
        stop_ftp_application(state);
        co_return;
      }
      password = std::move(*prompted);
    }
    state->client = co_await elder_terms::open_ftp_client_async(
        {
            .connection = state->connection,
            .password = std::move(password),
        },
        cancellation);
    open_ftp_application_window(state);
  } catch (const cardio::canceled_exception &) {
    stop_ftp_application(state);
  } catch (const std::exception &exception) {
    if (!state->shutting_down) {
      show_startup_error(state, exception.what());
    }
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
  state.settings = settings_result.store;
  state.connection =
      elder_terms::ftp_connection_settings(settings_result.store);

  if (launch_options.test.fixture) {
    state.client = elder_terms::create_sftp_fixture_client(false);
    open_ftp_application_window(&state);
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

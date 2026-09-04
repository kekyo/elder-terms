#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <gestament/gtk.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

#include <unistd.h>

#include <cardio.h>
#include <elder-terms/application-icon.h>
#include <elder-terms/application-settings-widget.h>
#include <elder-terms/application-version.h>
#include <elder-terms/localization.h>
#include <elder-terms/settings/application-settings.h>
#include <elder-terms/settings-widget.h>
#include <elder-terms/settings.h>

#include "connection-repository.h"
#include "hotkey-backend.h"
#include "main-window.h"
#include "tray-backend.h"

namespace {

static constexpr char open_application_hotkey_action_id[] =
    "open-application";
static constexpr char autostart_option_name[] = "autostart";
static constexpr char application_settings_option_name[] =
    "application-settings";
static constexpr char about_option_name[] = "about";
static constexpr char language_environment_name[] = "LANGUAGE";
static constexpr char language_environment_prefix[] = "LANGUAGE=";

enum class PendingActionKind {
  none,
  select,
  new_connection,
  close,
  quit,
  restart,
};

enum class ApplicationDialogPage {
  application = 0,
  about = 1,
};

struct PendingAction {
  PendingActionKind kind = PendingActionKind::none;
  std::filesystem::path path;
};

struct ConnectionRow {
  bool valid = false;
  bool is_new = false;
  std::string name;
  std::filesystem::path path;
};

struct ApplicationState;

struct ChildLaunch {
  ApplicationState *application = nullptr;
  GSubprocess *process = nullptr;
  std::filesystem::path temporary_startup_path;
};

struct ConnectionHotkeyTarget {
  std::string action_id;
  std::string name;
  std::filesystem::path path;
  elder_terms::KeyBinding binding;
};

struct ApplicationState {
  GApplication *application = nullptr;
  cardio::dispatcher *dispatcher = nullptr;
  cardio::dispatcher_group_glib *dispatcher_group = nullptr;
  std::optional<elder_terms::LauncherMainWindow> main_window_storage;
  elder_terms::LauncherMainWindow *main_window = nullptr;
  elder_terms::HotkeyBackendState *hotkey_backend = nullptr;
  elder_terms::TrayBackendState *tray_backend = nullptr;
  elder_terms::SettingsWidgetState *settings_widget = nullptr;
  elder_terms::SettingsWidgetState *global_defaults_widget = nullptr;
  elder_terms::ApplicationSettingsWidgetState *application_settings_widget =
      nullptr;
  std::filesystem::path connection_directory;
  std::filesystem::path global_config_path;
  std::vector<elder_terms::ConnectionProfile> profiles;
  std::optional<std::filesystem::path> selected_path;
  std::string persisted_name;
  std::string draft_name;
  std::string name_error;
  bool has_selection = false;
  bool current_is_new = false;
  bool name_dirty = false;
  bool rename_existing_on_edit = false;
  bool suppress_selection = false;
  GtkWidget *confirmation_dialog = nullptr;
  GtkWidget *delete_connection_dialog = nullptr;
  std::optional<std::filesystem::path> context_connection_path;
  std::string context_connection_name;
  std::optional<std::filesystem::path> delete_connection_path;
  GtkWidget *global_defaults_dialog = nullptr;
  GtkWidget *global_defaults_save_button = nullptr;
  GtkWidget *application_dialog = nullptr;
  GtkWidget *application_dialog_notebook = nullptr;
  GtkWidget *application_dialog_save_button = nullptr;
  GtkWidget *application_dialog_cancel_button = nullptr;
  GtkWidget *ui_language_restart_dialog = nullptr;
  elder_terms::ApplicationUiLanguage application_initial_ui_language =
      elder_terms::ApplicationUiLanguage::system;
  bool application_dialog_parent_was_visible = false;
  PendingAction pending_action;
  GFileMonitor *connection_monitor = nullptr;
  guint monitor_refresh_source = 0;
  bool monitor_reload_selected = false;
  std::optional<std::filesystem::path> monitor_renamed_path;
  std::string vte_executable;
  std::string file_transfer_executable;
  std::string launcher_argv0;
  std::vector<ChildLaunch *> child_launches;
  std::vector<ConnectionHotkeyTarget> connection_hotkey_targets;
  elder_terms::StartupMode startup_mode =
      elder_terms::StartupMode::window;
  elder_terms::TrayBackendAvailabilityState tray_availability =
      elder_terms::TrayBackendAvailabilityState::pending;
  bool application_held = false;
  bool activated = false;
  bool initial_activation_from_autostart = false;
  bool quitting = false;
  bool window_destroyed = false;
  bool application_shutting_down = false;
  bool startup_failed = false;
  bool shutting_down = false;
  bool restart_requested = false;
};

enum ConnectionColumns {
  connection_name_column = 0,
  connection_path_column = 1,
  connection_is_new_column = 2,
  connection_column_count = 3,
};

static void update_action_sensitivity(ApplicationState *state);
static void begin_new_connection(ApplicationState *state);
static void select_existing_connection(
    ApplicationState *state, const std::filesystem::path &path);
static void launch_selected_connection(ApplicationState *state);
static void launch_saved_connection(
    ApplicationState *state, const std::filesystem::path &path,
    const std::optional<std::string> &activation_token);
static void replace_registered_hotkeys(
    ApplicationState *state,
    const elder_terms::SettingsStore &global_store);
static void reload_hotkey_actions(ApplicationState *state);
static void present_main_window(
    ApplicationState *state,
    std::optional<std::uint32_t> activation_time = std::nullopt,
    std::optional<std::string> activation_token = std::nullopt);
static void request_application_quit(ApplicationState *state);
static void request_application_restart(ApplicationState *state);
static void enable_connection_selection(ApplicationState *state);
static void open_application_dialog(ApplicationState *state,
                                    ApplicationDialogPage page);

static void print_warnings(const std::vector<std::string> &warnings) {
  for (const std::string &warning : warnings) {
    std::cerr << warning << '\n';
  }
}

static std::string format_translated_string(const char *format,
                                            const char *value) {
  gchar *formatted = g_strdup_printf(format, value);
  const std::string result = formatted == nullptr ? std::string() : formatted;
  g_free(formatted);
  return result;
}

static std::string format_translated_string(const char *format, int value) {
  gchar *formatted = g_strdup_printf(format, value);
  const std::string result = formatted == nullptr ? std::string() : formatted;
  g_free(formatted);
  return result;
}

static void on_notice_response(GtkDialog *dialog, gint, gpointer) {
  gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void show_error_for_parent(GtkWindow *parent,
                                  const std::string &summary,
                                  const std::vector<std::string> &details) {
  print_warnings(details);
  std::string secondary;
  if (!details.empty()) {
    secondary = details.front();
  }
  GtkWidget *dialog = gtk_message_dialog_new(
      parent,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", summary.c_str());
  gestament_gtk_assign_accessible_id(dialog, "operation_error_dialog");
  if (!secondary.empty()) {
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
                                             secondary.c_str());
  }
  g_signal_connect(dialog, "response", G_CALLBACK(on_notice_response), nullptr);
  gtk_widget_show(dialog);
}

static void show_error(ApplicationState *state, const std::string &summary,
                       const std::vector<std::string> &details) {
  show_error_for_parent(GTK_WINDOW(state->main_window->window), summary,
                        details);
}

static void show_hotkey_registration_error(ApplicationState *state) {
  if (state == nullptr || state->main_window == nullptr ||
      state->window_destroyed || state->application_shutting_down) {
    return;
  }

  const char *summary = _("Global shortcuts are unavailable");
  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(state->main_window->window),
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", summary);
  gtk_window_set_title(GTK_WINDOW(dialog), summary);
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dialog), "%s",
      _("One or more configured global shortcuts could not be registered and "
        "will not work. Check whether your desktop environment supports "
        "global shortcuts."));
  GtkWidget *close = gtk_dialog_add_button(
      GTK_DIALOG(dialog), _("OK"), GTK_RESPONSE_CLOSE);
  gestament_gtk_assign_accessible_id(
      dialog, "hotkey_registration_error_dialog");
  gestament_gtk_assign_accessible_id(
      close, "hotkey_registration_error_close_button");
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
  g_signal_connect(dialog, "response", G_CALLBACK(on_notice_response),
                   nullptr);
  gtk_widget_show_all(dialog);
}

static void on_ui_language_restart_dialog_destroy(GtkWidget *dialog,
                                                  gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state->ui_language_restart_dialog == dialog) {
    state->ui_language_restart_dialog = nullptr;
  }
}

static void on_ui_language_restart_dialog_response(GtkDialog *dialog,
                                                   gint response,
                                                   gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  gtk_widget_destroy(GTK_WIDGET(dialog));
  if (response == GTK_RESPONSE_ACCEPT) {
    request_application_restart(state);
  }
}

static void show_ui_language_restart_dialog(ApplicationState *state) {
  if (state->ui_language_restart_dialog != nullptr) {
    gtk_window_present(GTK_WINDOW(state->ui_language_restart_dialog));
    return;
  }

  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(state->main_window->window),
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      GTK_MESSAGE_INFO, GTK_BUTTONS_NONE, "%s",
      _("Restart to apply display language?"));
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dialog), "%s",
      _("The display language change will take effect after elder-terms "
        "restarts."));
  GtkWidget *later = gtk_dialog_add_button(
      GTK_DIALOG(dialog), _("Later"), GTK_RESPONSE_CANCEL);
  GtkWidget *restart = gtk_dialog_add_button(
      GTK_DIALOG(dialog), _("Restart now"), GTK_RESPONSE_ACCEPT);
  gestament_gtk_assign_accessible_id(dialog,
                                     "ui_language_restart_dialog");
  gestament_gtk_assign_accessible_id(
      later, "ui_language_restart_later_button");
  gestament_gtk_assign_accessible_id(
      restart, "ui_language_restart_now_button");
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  state->ui_language_restart_dialog = dialog;
  g_signal_connect(dialog, "response",
                   G_CALLBACK(on_ui_language_restart_dialog_response), state);
  g_signal_connect(dialog, "destroy",
                   G_CALLBACK(on_ui_language_restart_dialog_destroy), state);
  gtk_widget_show_all(dialog);
}

static bool editor_is_dirty(const ApplicationState *state) {
  return state->has_selection &&
         (state->current_is_new || state->name_dirty ||
          elder_terms::settings_widget_is_dirty(state->settings_widget));
}

static bool editor_is_valid(const ApplicationState *state) {
  return state->has_selection && state->name_error.empty() &&
         elder_terms::settings_widget_is_valid(state->settings_widget);
}

static void update_action_sensitivity(ApplicationState *state) {
  const bool valid = editor_is_valid(state);
  gtk_widget_set_sensitive(state->main_window->apply_button,
                           valid && editor_is_dirty(state));
  gtk_widget_set_sensitive(state->main_window->connect_button, valid);
  gtk_widget_set_tooltip_text(
      state->main_window->connection_list,
      state->name_error.empty() ? nullptr : state->name_error.c_str());
}

static void
update_global_defaults_save_sensitivity(ApplicationState *state) {
  if (state->global_defaults_save_button == nullptr) {
    return;
  }
  const bool sensitive =
      state->global_defaults_widget != nullptr &&
      elder_terms::settings_widget_is_valid(state->global_defaults_widget) &&
      elder_terms::settings_widget_is_dirty(state->global_defaults_widget);
  gtk_widget_set_sensitive(state->global_defaults_save_button, sensitive);
}

static gboolean on_window_drag_button_press(GtkWidget *,
                                            GdkEventButton *event,
                                            gpointer user_data) {
  if (event == nullptr || event->type != GDK_BUTTON_PRESS ||
      event->button != GDK_BUTTON_PRIMARY || user_data == nullptr ||
      !GTK_IS_WINDOW(user_data)) {
    return GDK_EVENT_PROPAGATE;
  }

  gtk_window_begin_move_drag(
      GTK_WINDOW(user_data), static_cast<gint>(event->button),
      static_cast<gint>(event->x_root), static_cast<gint>(event->y_root),
      event->time);
  return GDK_EVENT_STOP;
}

static gboolean on_main_window_focus_in(GtkWidget *, GdkEventFocus *,
                                        gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state == nullptr) {
    return GDK_EVENT_PROPAGATE;
  }
  GtkWidget *dialog = state->application_dialog != nullptr
                          ? state->application_dialog
                          : state->global_defaults_dialog;
  if (dialog != nullptr) {
    gtk_window_present_with_time(GTK_WINDOW(dialog),
                                 gtk_get_current_event_time());
    return GDK_EVENT_STOP;
  }
  return GDK_EVENT_PROPAGATE;
}

static void on_global_defaults_dialog_destroy(GtkWidget *,
                                              gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  state->global_defaults_dialog = nullptr;
  state->global_defaults_save_button = nullptr;
  if (state->global_defaults_widget != nullptr) {
    elder_terms::destroy_settings_widget(state->global_defaults_widget);
    state->global_defaults_widget = nullptr;
  }
  if (!state->window_destroyed && state->main_window != nullptr) {
    gtk_widget_set_sensitive(state->main_window->window, TRUE);
    gtk_window_present_with_time(
        GTK_WINDOW(state->main_window->window),
        gtk_get_current_event_time());
  }
}

static void close_global_defaults_dialog(ApplicationState *state) {
  if (state != nullptr && state->global_defaults_dialog != nullptr) {
    gtk_widget_destroy(state->global_defaults_dialog);
  }
}

static void on_global_defaults_dialog_response(GtkDialog *dialog,
                                               gint response,
                                               gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (response != GTK_RESPONSE_ACCEPT) {
    gtk_widget_destroy(GTK_WIDGET(dialog));
    return;
  }
  if (state->global_defaults_widget == nullptr ||
      !elder_terms::settings_widget_is_valid(
          state->global_defaults_widget) ||
      !elder_terms::settings_widget_is_dirty(
          state->global_defaults_widget)) {
    update_global_defaults_save_sensitivity(state);
    return;
  }

  const elder_terms::SettingsStore store =
      elder_terms::settings_widget_draft_store(
          state->global_defaults_widget);
  const elder_terms::SettingsSaveResult result =
      elder_terms::save_global_settings(store, state->global_config_path);
  if (!result.saved) {
    show_error_for_parent(GTK_WINDOW(dialog),
                          _("Failed to save connection defaults"),
                          result.warnings);
    return;
  }
  print_warnings(result.warnings);
  elder_terms::settings_widget_rebase_fallbacks(state->settings_widget, store);
  update_action_sensitivity(state);
  gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void on_global_defaults_cancel_clicked(GtkButton *,
                                              gpointer user_data) {
  gtk_dialog_response(GTK_DIALOG(user_data), GTK_RESPONSE_CANCEL);
}

static void on_global_defaults_save_clicked(GtkButton *,
                                            gpointer user_data) {
  gtk_dialog_response(GTK_DIALOG(user_data), GTK_RESPONSE_ACCEPT);
}

static void open_global_defaults_dialog(ApplicationState *state) {
  if (state->global_defaults_dialog != nullptr) {
    gtk_window_present(GTK_WINDOW(state->global_defaults_dialog));
    return;
  }

  elder_terms::SettingsLoadResult loaded =
      elder_terms::load_global_settings(state->global_config_path, 1.0);
  print_warnings(loaded.warnings);
  GtkWidget *dialog = gtk_dialog_new();
  gestament_gtk_assign_accessible_id(dialog, "global_defaults_dialog");
  gtk_window_set_title(GTK_WINDOW(dialog), _("Connection defaults"));
  gtk_window_set_transient_for(GTK_WINDOW(dialog),
                               GTK_WINDOW(state->main_window->window));
  gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 720, 420);

  GtkWidget *action_row = gtk_event_box_new();
  gestament_gtk_assign_accessible_id(action_row,
                                     "global_defaults_action_row");
  GtkWidget *action_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(action_content, 12);
  gtk_widget_set_margin_end(action_content, 12);
  gtk_widget_set_margin_top(action_content, 10);
  gtk_widget_set_margin_bottom(action_content, 10);
  gtk_container_add(GTK_CONTAINER(action_row), action_content);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  GtkWidget *cancel = gtk_button_new_with_label(_("Cancel"));
  GtkWidget *save = gtk_button_new_with_label(_("Save"));
  gestament_gtk_assign_accessible_id(cancel,
                                     "global_defaults_cancel_button");
  gestament_gtk_assign_accessible_id(save, "global_defaults_save_button");
  gtk_widget_set_can_default(save, TRUE);
  gtk_box_pack_start(GTK_BOX(action_content), spacer, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(action_content), cancel, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(action_content), save, FALSE, TRUE, 0);

  state->global_defaults_dialog = dialog;
  state->global_defaults_save_button = save;
  elder_terms::SettingsWidgetCallbacks callbacks;
  callbacks.changed = [state]() {
    update_global_defaults_save_sensitivity(state);
  };
  state->global_defaults_widget = elder_terms::create_settings_widget({
      .store = std::move(loaded.store),
      .is_runtime = false,
      .show_actions = false,
      .mode = elder_terms::SettingsWidgetMode::global_defaults,
      .id_prefix = "global_settings",
      .callbacks = std::move(callbacks),
  });
  gtk_box_pack_start(
      GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
      elder_terms::settings_widget_root(state->global_defaults_widget), TRUE,
      TRUE, 0);
  gtk_box_pack_start(
      GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), action_row,
      FALSE, TRUE, 0);
  gtk_widget_grab_default(save);

  g_signal_connect(cancel, "clicked",
                   G_CALLBACK(on_global_defaults_cancel_clicked), dialog);
  g_signal_connect(save, "clicked",
                   G_CALLBACK(on_global_defaults_save_clicked), dialog);
  g_signal_connect(dialog, "response",
                   G_CALLBACK(on_global_defaults_dialog_response), state);
  g_signal_connect(dialog, "destroy",
                   G_CALLBACK(on_global_defaults_dialog_destroy), state);
  g_signal_connect(action_row, "button-press-event",
                   G_CALLBACK(on_window_drag_button_press), dialog);
  update_global_defaults_save_sensitivity(state);
  gtk_widget_set_sensitive(state->main_window->window, FALSE);
  gtk_widget_show_all(dialog);
  gtk_window_present(GTK_WINDOW(dialog));
}

static void on_global_defaults_clicked(GtkButton *, gpointer user_data) {
  open_global_defaults_dialog(
      static_cast<ApplicationState *>(user_data));
}

static void update_application_dialog_actions(ApplicationState *state) {
  if (state == nullptr || state->application_dialog_notebook == nullptr ||
      state->application_dialog_save_button == nullptr ||
      state->application_dialog_cancel_button == nullptr) {
    return;
  }
  const int page = gtk_notebook_get_current_page(
      GTK_NOTEBOOK(state->application_dialog_notebook));
  const bool editing =
      page == static_cast<int>(ApplicationDialogPage::application);
  gtk_widget_set_visible(state->application_dialog_save_button,
                         editing ? TRUE : FALSE);
  gtk_button_set_label(
      GTK_BUTTON(state->application_dialog_cancel_button),
      editing ? _("Cancel") : _("Close"));
  if (editing && state->application_settings_widget != nullptr) {
    const bool sensitive =
        elder_terms::application_settings_widget_is_valid(
            state->application_settings_widget) &&
        elder_terms::application_settings_widget_is_dirty(
            state->application_settings_widget);
    gtk_widget_set_sensitive(state->application_dialog_save_button,
                             sensitive ? TRUE : FALSE);
  }
}

static void on_application_dialog_switch_page(GtkNotebook *, GtkWidget *,
                                              guint, gpointer user_data) {
  update_application_dialog_actions(
      static_cast<ApplicationState *>(user_data));
}

static void on_application_dialog_destroy(GtkWidget *dialog,
                                          gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state->application_dialog != dialog) {
    return;
  }
  state->application_dialog = nullptr;
  state->application_dialog_notebook = nullptr;
  state->application_dialog_save_button = nullptr;
  state->application_dialog_cancel_button = nullptr;
  if (state->application_settings_widget != nullptr) {
    elder_terms::destroy_application_settings_widget(
        state->application_settings_widget);
    state->application_settings_widget = nullptr;
  }
  if (!state->window_destroyed && state->main_window != nullptr) {
    gtk_widget_set_sensitive(state->main_window->window, TRUE);
    if (state->application_dialog_parent_was_visible) {
      gtk_window_present_with_time(
          GTK_WINDOW(state->main_window->window),
          gtk_get_current_event_time());
    }
  }
  state->application_dialog_parent_was_visible = false;
}

static void close_application_dialog(ApplicationState *state) {
  if (state != nullptr && state->application_dialog != nullptr) {
    gtk_widget_destroy(state->application_dialog);
  }
}

static void on_application_dialog_response(GtkDialog *dialog, gint response,
                                           gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (response != GTK_RESPONSE_ACCEPT) {
    gtk_widget_destroy(GTK_WIDGET(dialog));
    return;
  }
  if (state->application_settings_widget == nullptr ||
      !elder_terms::application_settings_widget_is_valid(
          state->application_settings_widget) ||
      !elder_terms::application_settings_widget_is_dirty(
          state->application_settings_widget)) {
    update_application_dialog_actions(state);
    return;
  }

  const elder_terms::SettingsStore store =
      elder_terms::application_settings_widget_draft_store(
          state->application_settings_widget);
  const bool ui_language_changed =
      elder_terms::application_ui_language(store) !=
      state->application_initial_ui_language;
  const elder_terms::SettingsSaveResult result =
      elder_terms::save_application_settings(store,
                                             state->global_config_path);
  if (!result.saved) {
    show_error_for_parent(GTK_WINDOW(dialog),
                          _("Failed to save application settings"),
                          result.warnings);
    return;
  }
  print_warnings(result.warnings);
  replace_registered_hotkeys(state, store);
  gtk_widget_destroy(GTK_WIDGET(dialog));
  if (ui_language_changed) {
    show_ui_language_restart_dialog(state);
  }
}

static void on_application_dialog_cancel_clicked(GtkButton *,
                                                 gpointer user_data) {
  gtk_dialog_response(GTK_DIALOG(user_data), GTK_RESPONSE_CANCEL);
}

static void on_application_dialog_save_clicked(GtkButton *,
                                               gpointer user_data) {
  gtk_dialog_response(GTK_DIALOG(user_data), GTK_RESPONSE_ACCEPT);
}

static GtkWidget *create_application_about_page() {
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(page, 32);
  gtk_widget_set_margin_bottom(page, 32);
  gtk_widget_set_margin_start(page, 32);
  gtk_widget_set_margin_end(page, 32);
  gtk_widget_set_valign(page, GTK_ALIGN_CENTER);

  GtkWidget *title = gtk_label_new("elder-terms");
  PangoAttrList *attributes = pango_attr_list_new();
  pango_attr_list_insert(attributes,
                         pango_attr_scale_new(PANGO_SCALE_LARGE));
  pango_attr_list_insert(attributes,
                         pango_attr_weight_new(PANGO_WEIGHT_BOLD));
  gtk_label_set_attributes(GTK_LABEL(title), attributes);
  pango_attr_list_unref(attributes);
  gtk_box_pack_start(GTK_BOX(page), title, FALSE, FALSE, 0);

  const std::string version = format_translated_string(
      _("Version %s"), elder_terms::application_version());
  GtkWidget *version_label = gtk_label_new(version.c_str());
  gtk_label_set_selectable(GTK_LABEL(version_label), TRUE);
  gestament_gtk_assign_accessible_id(
      version_label, "application_about_version_label");
  gtk_box_pack_start(GTK_BOX(page), version_label, FALSE, FALSE, 0);
  return page;
}

static void select_application_dialog_page(ApplicationState *state,
                                           ApplicationDialogPage page) {
  if (state == nullptr || state->application_dialog_notebook == nullptr) {
    return;
  }
  gtk_notebook_set_current_page(
      GTK_NOTEBOOK(state->application_dialog_notebook),
      static_cast<int>(page));
  update_application_dialog_actions(state);
}

static void open_application_dialog(ApplicationState *state,
                                    ApplicationDialogPage page) {
  if (state == nullptr || state->main_window == nullptr ||
      state->window_destroyed) {
    return;
  }
  if (state->application_dialog != nullptr) {
    select_application_dialog_page(state, page);
    gtk_window_present(GTK_WINDOW(state->application_dialog));
    return;
  }
  if (state->global_defaults_dialog != nullptr) {
    gtk_window_present(GTK_WINDOW(state->global_defaults_dialog));
    return;
  }

  elder_terms::SettingsLoadResult loaded =
      elder_terms::load_global_settings(state->global_config_path, 1.0);
  print_warnings(loaded.warnings);
  state->application_initial_ui_language =
      elder_terms::application_ui_language(loaded.store);

  GtkWidget *dialog = gtk_dialog_new();
  gestament_gtk_assign_accessible_id(dialog, "application_dialog");
  gtk_window_set_title(GTK_WINDOW(dialog), _("Application"));
  gtk_window_set_transient_for(GTK_WINDOW(dialog),
                               GTK_WINDOW(state->main_window->window));
  gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 620, 350);

  GtkWidget *notebook = gtk_notebook_new();
  gestament_gtk_assign_accessible_id(notebook,
                                     "application_dialog_notebook");
  GtkWidget *settings_page =
      gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  elder_terms::ApplicationSettingsWidgetOptions options{
      .store = std::move(loaded.store),
      .id_prefix = "application_settings",
      .changed = [state]() { update_application_dialog_actions(state); },
  };
  state->application_settings_widget =
      elder_terms::create_application_settings_widget(std::move(options));
  gtk_box_pack_start(
      GTK_BOX(settings_page),
      elder_terms::application_settings_widget_root(
          state->application_settings_widget),
      TRUE, TRUE, 0);
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), settings_page,
                           gtk_label_new(_("Application")));
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                           create_application_about_page(),
                           gtk_label_new(_("About")));

  GtkWidget *action_row = gtk_event_box_new();
  GtkWidget *action_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(action_content, 12);
  gtk_widget_set_margin_end(action_content, 12);
  gtk_widget_set_margin_top(action_content, 10);
  gtk_widget_set_margin_bottom(action_content, 10);
  gtk_container_add(GTK_CONTAINER(action_row), action_content);
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  GtkWidget *cancel = gtk_button_new_with_label(_("Cancel"));
  GtkWidget *save = gtk_button_new_with_label(_("Save"));
  gestament_gtk_assign_accessible_id(cancel,
                                     "application_dialog_cancel_button");
  gestament_gtk_assign_accessible_id(save,
                                     "application_dialog_save_button");
  gtk_widget_set_can_default(save, TRUE);
  gtk_box_pack_start(GTK_BOX(action_content), spacer, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(action_content), cancel, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(action_content), save, FALSE, TRUE, 0);

  state->application_dialog = dialog;
  state->application_dialog_notebook = notebook;
  state->application_dialog_save_button = save;
  state->application_dialog_cancel_button = cancel;
  state->application_dialog_parent_was_visible =
      gtk_widget_get_visible(state->main_window->window) != FALSE;
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_box_pack_start(GTK_BOX(content), notebook, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(content), action_row, FALSE, TRUE, 0);
  gtk_widget_grab_default(save);

  g_signal_connect(notebook, "switch-page",
                   G_CALLBACK(on_application_dialog_switch_page), state);
  g_signal_connect(cancel, "clicked",
                   G_CALLBACK(on_application_dialog_cancel_clicked), dialog);
  g_signal_connect(save, "clicked",
                   G_CALLBACK(on_application_dialog_save_clicked), dialog);
  g_signal_connect(dialog, "response",
                   G_CALLBACK(on_application_dialog_response), state);
  g_signal_connect(dialog, "destroy",
                   G_CALLBACK(on_application_dialog_destroy), state);
  g_signal_connect(action_row, "button-press-event",
                   G_CALLBACK(on_window_drag_button_press), dialog);
  gtk_widget_set_sensitive(state->main_window->window, FALSE);
  gtk_widget_show_all(dialog);
  select_application_dialog_page(state, page);
  gtk_window_present(GTK_WINDOW(dialog));
}

static void on_application_settings_menu_item_activate(GtkMenuItem *,
                                                       gpointer user_data) {
  open_application_dialog(static_cast<ApplicationState *>(user_data),
                          ApplicationDialogPage::application);
}

static void on_about_menu_item_activate(GtkMenuItem *, gpointer user_data) {
  open_application_dialog(static_cast<ApplicationState *>(user_data),
                          ApplicationDialogPage::about);
}

static ConnectionRow selected_row(ApplicationState *state) {
  GtkTreeSelection *selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(state->main_window->connection_list));
  GtkTreeModel *model = nullptr;
  GtkTreeIter iterator;
  if (!gtk_tree_selection_get_selected(selection, &model, &iterator)) {
    return {};
  }

  gchar *name = nullptr;
  gchar *path = nullptr;
  gboolean is_new = FALSE;
  gtk_tree_model_get(model, &iterator, connection_name_column, &name,
                     connection_path_column, &path,
                     connection_is_new_column, &is_new, -1);
  ConnectionRow result{
      .valid = true,
      .is_new = is_new != FALSE,
      .name = name == nullptr ? std::string() : std::string(name),
      .path = path == nullptr ? std::filesystem::path()
                             : std::filesystem::path(path),
  };
  g_free(name);
  g_free(path);
  return result;
}

static ConnectionRow connection_row_at_path(ApplicationState *state,
                                             GtkTreePath *path) {
  GtkTreeIter iterator;
  GtkTreeModel *model =
      GTK_TREE_MODEL(state->main_window->connection_store);
  if (path == nullptr || !gtk_tree_model_get_iter(model, &iterator, path)) {
    return {};
  }

  gchar *name = nullptr;
  gchar *profile_path = nullptr;
  gboolean is_new = FALSE;
  gtk_tree_model_get(model, &iterator, connection_name_column, &name,
                     connection_path_column, &profile_path,
                     connection_is_new_column, &is_new, -1);
  ConnectionRow result{
      .valid = true,
      .is_new = is_new != FALSE,
      .name = name == nullptr ? std::string() : std::string(name),
      .path = profile_path == nullptr ? std::filesystem::path()
                                     : std::filesystem::path(profile_path),
  };
  g_free(name);
  g_free(profile_path);
  return result;
}

static bool row_matches_current(const ApplicationState *state,
                                const ConnectionRow &row) {
  if (!state->has_selection || !row.valid ||
      state->current_is_new != row.is_new) {
    return false;
  }
  return row.is_new ? true
                    : state->selected_path.has_value() &&
                          state->selected_path.value() == row.path;
}

static bool select_matching_row(ApplicationState *state,
                                const std::optional<std::filesystem::path> &path,
                                bool select_new) {
  GtkTreeModel *model = GTK_TREE_MODEL(state->main_window->connection_store);
  GtkTreeIter iterator;
  if (!gtk_tree_model_get_iter_first(model, &iterator)) {
    return false;
  }
  do {
    gchar *row_path = nullptr;
    gboolean is_new = FALSE;
    gtk_tree_model_get(model, &iterator, connection_path_column, &row_path,
                       connection_is_new_column, &is_new, -1);
    const bool matches =
        select_new ? is_new != FALSE
                   : is_new == FALSE && path.has_value() &&
                         row_path != nullptr && path.value() == row_path;
    g_free(row_path);
    if (!matches) {
      continue;
    }
    GtkTreeSelection *selection = gtk_tree_view_get_selection(
        GTK_TREE_VIEW(state->main_window->connection_list));
    gtk_tree_selection_select_iter(selection, &iterator);
    GtkTreePath *tree_path = gtk_tree_model_get_path(model, &iterator);
    gtk_tree_view_scroll_to_cell(
        GTK_TREE_VIEW(state->main_window->connection_list), tree_path, nullptr,
        FALSE, 0.0F, 0.0F);
    gtk_tree_path_free(tree_path);
    return true;
  } while (gtk_tree_model_iter_next(model, &iterator));
  return false;
}

static void restore_current_selection(ApplicationState *state) {
  state->suppress_selection = true;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(state->main_window->connection_list));
  if (!state->has_selection) {
    gtk_tree_selection_unselect_all(selection);
  } else {
    select_matching_row(state, state->selected_path, state->current_is_new);
  }
  state->suppress_selection = false;
}

static void populate_profile_rows(ApplicationState *state) {
  state->profiles = elder_terms::list_connection_profiles(
      state->connection_directory);
  state->suppress_selection = true;
  gtk_list_store_clear(state->main_window->connection_store);
  for (const elder_terms::ConnectionProfile &profile : state->profiles) {
    GtkTreeIter iterator;
    gtk_list_store_append(state->main_window->connection_store, &iterator);
    gtk_list_store_set(state->main_window->connection_store, &iterator,
                       connection_name_column, profile.name.c_str(),
                       connection_path_column, profile.path.c_str(),
                       connection_is_new_column, FALSE, -1);
  }
  state->suppress_selection = false;
}

static std::vector<elder_terms::HotkeyAction>
build_registered_hotkey_actions(
    ApplicationState *state,
    const elder_terms::SettingsStore &global_store,
    std::vector<std::string> *warnings) {
  state->connection_hotkey_targets.clear();
  std::vector<elder_terms::HotkeyAction> actions;
  actions.reserve(state->profiles.size() + 1);

  for (const elder_terms::ConnectionProfile &profile : state->profiles) {
    const elder_terms::SettingsLoadResult loaded =
        elder_terms::load_connection_profile(profile.path);
    for (const std::string &warning : loaded.warnings) {
      if (!loaded.loaded ||
          warning.find("[general] open_connection") !=
              std::string::npos) {
        warnings->push_back(warning);
      }
    }
    if (!loaded.loaded) {
      continue;
    }

    const std::optional<elder_terms::KeyBinding> binding =
        elder_terms::general_open_connection_hotkey(loaded.store);
    if (!binding.has_value()) {
      continue;
    }
    const auto duplicate = std::find_if(
        state->connection_hotkey_targets.begin(),
        state->connection_hotkey_targets.end(),
        [&binding](const ConnectionHotkeyTarget &target) {
          return elder_terms::key_bindings_equal(
              target.binding, *binding);
        });
    if (duplicate != state->connection_hotkey_targets.end()) {
      warnings->push_back(
          "Warning: connection hotkey " +
          elder_terms::general_open_connection_hotkey_text(
              loaded.store) +
          " is assigned to both " + duplicate->name + " and " +
          profile.name + "; using " + duplicate->name);
      continue;
    }

    const std::string action_id =
        "open-connection-" +
        std::to_string(state->connection_hotkey_targets.size());
    state->connection_hotkey_targets.push_back({
        .action_id = action_id,
        .name = profile.name,
        .path = profile.path,
        .binding = *binding,
    });
    actions.push_back({
        .id = action_id,
        .description = format_translated_string(
            _("Open connection %s"), profile.name.c_str()),
        .binding = *binding,
    });
  }

  const std::optional<elder_terms::KeyBinding> application_binding =
      elder_terms::application_open_hotkey(global_store);
  if (!application_binding.has_value()) {
    return actions;
  }
  const auto collision = std::find_if(
      state->connection_hotkey_targets.begin(),
      state->connection_hotkey_targets.end(),
      [&application_binding](const ConnectionHotkeyTarget &target) {
        return elder_terms::key_bindings_equal(
            target.binding, *application_binding);
      });
  if (collision != state->connection_hotkey_targets.end()) {
    warnings->push_back(
        "Warning: application hotkey " +
        elder_terms::application_open_hotkey_text(global_store) +
        " conflicts with connection " + collision->name +
        "; using " + collision->name);
    return actions;
  }
  actions.push_back({
      .id = open_application_hotkey_action_id,
      .description = _("Open elder-terms"),
      .binding = *application_binding,
  });
  return actions;
}

static void replace_registered_hotkeys(
    ApplicationState *state,
    const elder_terms::SettingsStore &global_store) {
  state->profiles = elder_terms::list_connection_profiles(
      state->connection_directory);
  std::vector<std::string> warnings;
  const std::vector<elder_terms::HotkeyAction> actions =
      build_registered_hotkey_actions(state, global_store, &warnings);
  print_warnings(warnings);
  if (state->hotkey_backend != nullptr) {
    elder_terms::replace_hotkey_actions(state->hotkey_backend, actions);
  }
}

static void reload_hotkey_actions(ApplicationState *state) {
  const elder_terms::SettingsLoadResult global =
      elder_terms::load_global_settings(state->global_config_path, 1.0);
  print_warnings(global.warnings);
  replace_registered_hotkeys(state, global.store);
}

static void append_new_profile_row(ApplicationState *state) {
  GtkTreeIter iterator;
  gtk_list_store_append(state->main_window->connection_store, &iterator);
  gtk_list_store_set(state->main_window->connection_store, &iterator,
                     connection_name_column, state->draft_name.c_str(),
                     connection_path_column, "", connection_is_new_column,
                     TRUE, -1);
}

static void show_empty_details(ApplicationState *state) {
  state->has_selection = false;
  state->current_is_new = false;
  state->selected_path.reset();
  state->persisted_name.clear();
  state->draft_name.clear();
  state->name_error.clear();
  state->name_dirty = false;
  gtk_stack_set_visible_child_name(GTK_STACK(state->main_window->details_stack),
                                   "empty");
  update_action_sensitivity(state);
}

static bool load_existing_connection(ApplicationState *state,
                                     const std::filesystem::path &path) {
  const elder_terms::SettingsLoadResult result =
      elder_terms::load_connection_profile(path);
  print_warnings(result.warnings);
  if (!result.loaded) {
    show_error(state, _("Failed to load connection"), result.warnings);
    return false;
  }

  state->has_selection = true;
  state->current_is_new = false;
  state->selected_path = path;
  state->persisted_name = path.stem().string();
  state->draft_name = state->persisted_name;
  state->name_error.clear();
  state->name_dirty = false;
  elder_terms::update_settings_widget_store(state->settings_widget,
                                             result.store);
  gtk_stack_set_visible_child_name(GTK_STACK(state->main_window->details_stack),
                                   "settings");
  update_action_sensitivity(state);
  return true;
}

static void start_name_editing(ApplicationState *state,
                               bool rename_existing) {
  GtkTreeSelection *selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(state->main_window->connection_list));
  GtkTreeModel *model = nullptr;
  GtkTreeIter iterator;
  if (!gtk_tree_selection_get_selected(selection, &model, &iterator)) {
    return;
  }
  state->rename_existing_on_edit =
      rename_existing && state->has_selection && !state->current_is_new;
  GtkTreePath *path = gtk_tree_model_get_path(model, &iterator);
  GtkTreeViewColumn *column = gtk_tree_view_get_column(
      GTK_TREE_VIEW(state->main_window->connection_list), 0);
  g_object_set(state->main_window->connection_name_renderer, "editable", TRUE,
               nullptr);
  gtk_widget_grab_focus(state->main_window->connection_list);
  gtk_tree_view_set_cursor_on_cell(
      GTK_TREE_VIEW(state->main_window->connection_list), path, column,
      state->main_window->connection_name_renderer, TRUE);
  gtk_tree_path_free(path);
}

static std::string next_new_connection_name(
    const std::vector<elder_terms::ConnectionProfile> &profiles) {
  for (int suffix = 1;; ++suffix) {
    const std::string candidate =
        suffix == 1
            ? _("New connection")
            : format_translated_string(_("New connection %d"), suffix);
    const auto validation = elder_terms::validate_connection_name(
        candidate, profiles, std::nullopt);
    if (validation.valid) {
      return candidate;
    }
  }
}

static std::string next_duplicate_connection_name(
    const std::string &source_name,
    const std::vector<elder_terms::ConnectionProfile> &profiles) {
  for (int suffix = 2;; ++suffix) {
    const std::string candidate =
        source_name + " (" + std::to_string(suffix) + ")";
    const auto validation = elder_terms::validate_connection_name(
        candidate, profiles, std::nullopt);
    if (validation.valid) {
      return candidate;
    }
  }
}

static void begin_new_connection(ApplicationState *state) {
  populate_profile_rows(state);
  state->has_selection = true;
  state->current_is_new = true;
  state->selected_path.reset();
  state->persisted_name.clear();
  state->draft_name = next_new_connection_name(state->profiles);
  state->name_error.clear();
  state->name_dirty = true;

  append_new_profile_row(state);
  state->suppress_selection = true;
  select_matching_row(state, std::nullopt, true);
  state->suppress_selection = false;

  elder_terms::SettingsStore store = elder_terms::create_default_settings(
      elder_terms::default_terminal_display_settings(1.0), state->draft_name);
  const elder_terms::SettingsLoadResult global_defaults =
      elder_terms::load_global_settings(state->global_config_path, 1.0);
  print_warnings(global_defaults.warnings);
  elder_terms::rebase_settings_store_fallbacks(&store,
                                               global_defaults.store);
  elder_terms::update_settings_widget_store(state->settings_widget,
                                             std::move(store));
  gtk_stack_set_visible_child_name(GTK_STACK(state->main_window->details_stack),
                                   "settings");
  update_action_sensitivity(state);
  start_name_editing(state, false);
}

static void select_existing_connection(
    ApplicationState *state, const std::filesystem::path &path) {
  populate_profile_rows(state);
  state->suppress_selection = true;
  const bool found = select_matching_row(state, path, false);
  state->suppress_selection = false;
  if (!found || !load_existing_connection(state, path)) {
    show_empty_details(state);
  }
}

static std::optional<std::filesystem::path> file_path(GFile *file) {
  if (file == nullptr) {
    return std::nullopt;
  }
  gchar *path = g_file_get_path(file);
  if (path == nullptr) {
    return std::nullopt;
  }
  std::filesystem::path result(path);
  g_free(path);
  return result;
}

static bool monitor_path_matches_selected(ApplicationState *state,
                                          GFile *file) {
  const auto path = file_path(file);
  return state->selected_path.has_value() && path.has_value() &&
         state->selected_path.value() == path.value();
}

static void preserve_current_editor_after_list_refresh(ApplicationState *state) {
  populate_profile_rows(state);
  state->suppress_selection = true;
  bool selected = false;
  if (state->has_selection && state->current_is_new) {
    append_new_profile_row(state);
    selected = select_matching_row(state, std::nullopt, true);
  } else if (state->has_selection && state->selected_path.has_value()) {
    selected = select_matching_row(state, state->selected_path, false);
  }
  state->suppress_selection = false;
  if (state->has_selection && !selected) {
    show_empty_details(state);
  }
}

static gboolean refresh_connections_from_monitor(gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  state->monitor_refresh_source = 0;
  if (state->shutting_down) {
    return G_SOURCE_REMOVE;
  }

  if (state->monitor_reload_selected && state->has_selection &&
      !state->current_is_new) {
    const auto path = state->monitor_renamed_path.has_value()
                          ? state->monitor_renamed_path
                          : state->selected_path;
    state->monitor_reload_selected = false;
    state->monitor_renamed_path.reset();
    std::error_code path_error;
    if (path.has_value() &&
        std::filesystem::is_regular_file(path.value(), path_error) &&
        !path_error) {
      select_existing_connection(state, path.value());
    } else {
      populate_profile_rows(state);
      show_empty_details(state);
    }
    reload_hotkey_actions(state);
    return G_SOURCE_REMOVE;
  }

  state->monitor_reload_selected = false;
  state->monitor_renamed_path.reset();
  preserve_current_editor_after_list_refresh(state);
  reload_hotkey_actions(state);
  return G_SOURCE_REMOVE;
}

static void on_connection_directory_changed(GFileMonitor *, GFile *file,
                                            GFile *other_file,
                                            GFileMonitorEvent event,
                                            gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state->shutting_down) {
    return;
  }

  const bool file_is_selected = monitor_path_matches_selected(state, file);
  const bool other_is_selected =
      monitor_path_matches_selected(state, other_file);
  if (file_is_selected || other_is_selected) {
    state->monitor_reload_selected = true;
  }
  if (file_is_selected && event == G_FILE_MONITOR_EVENT_RENAMED) {
    state->monitor_renamed_path = file_path(other_file);
  }
  if (state->monitor_refresh_source == 0) {
    state->monitor_refresh_source =
        g_idle_add(refresh_connections_from_monitor, state);
  }
}

static void stop_connection_monitor(ApplicationState *state) {
  if (state->monitor_refresh_source != 0) {
    g_source_remove(state->monitor_refresh_source);
    state->monitor_refresh_source = 0;
  }
  state->monitor_reload_selected = false;
  state->monitor_renamed_path.reset();
  if (state->connection_monitor == nullptr) {
    return;
  }
  g_file_monitor_cancel(state->connection_monitor);
  g_object_unref(state->connection_monitor);
  state->connection_monitor = nullptr;
}

static void start_connection_monitor(ApplicationState *state) {
  std::error_code directory_error;
  std::filesystem::create_directories(state->connection_directory,
                                      directory_error);
  if (directory_error) {
    show_error(state, _("Failed to monitor connections"),
               {directory_error.message()});
    return;
  }

  GFile *directory =
      g_file_new_for_path(state->connection_directory.c_str());
  GError *error = nullptr;
  state->connection_monitor = g_file_monitor_directory(
      directory, G_FILE_MONITOR_WATCH_MOVES, nullptr, &error);
  g_object_unref(directory);
  if (state->connection_monitor == nullptr) {
    const std::string message =
        error == nullptr ? _("Unknown file monitor error") : error->message;
    if (error != nullptr) {
      g_error_free(error);
    }
    show_error(state, _("Failed to monitor connections"), {message});
    return;
  }
  g_signal_connect(state->connection_monitor, "changed",
                   G_CALLBACK(on_connection_directory_changed), state);
}

static bool executable_file(const std::filesystem::path &path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error &&
         access(path.c_str(), X_OK) == 0;
}

static std::string resolve_child_executable(
    const char *launcher_argv0, const char *environment_name,
    const char *executable_name) {
  const char *override_path = g_getenv(environment_name);
  if (override_path != nullptr && override_path[0] != '\0') {
    return override_path;
  }

  std::error_code error;
  const std::filesystem::path launcher_path =
      std::filesystem::absolute(launcher_argv0, error).lexically_normal();
  if (!error) {
    const std::filesystem::path launcher_directory =
        launcher_path.parent_path();
    const std::vector<std::filesystem::path> candidates = {
        launcher_directory / executable_name,
        launcher_directory.parent_path() / "elder-terms-vte" /
            executable_name,
    };
    for (const auto &candidate : candidates) {
      if (executable_file(candidate)) {
        return candidate.string();
      }
    }
  }

  gchar *program = g_find_program_in_path(executable_name);
  if (program == nullptr) {
    return {};
  }
  std::string result(program);
  g_free(program);
  return result;
}

static std::vector<std::string> copy_process_arguments(int argc,
                                                       char **argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(std::max(argc, 1)));
  for (int index = 0; index < argc; ++index) {
    if (argv[index] != nullptr) {
      arguments.emplace_back(argv[index]);
    }
  }
  if (arguments.empty()) {
    arguments.emplace_back("elder-terms");
  }
  return arguments;
}

static std::string
resolve_launcher_executable(const std::string &argument_zero) {
  const std::string executable_name =
      argument_zero.empty() ? "elder-terms" : argument_zero;
  const std::filesystem::path argument_path(executable_name);
  if (argument_path.has_parent_path()) {
    std::error_code error;
    const std::filesystem::path absolute_path =
        std::filesystem::absolute(argument_path, error).lexically_normal();
    if (!error && executable_file(absolute_path)) {
      return absolute_path.string();
    }
    return {};
  }

  gchar *program = g_find_program_in_path(executable_name.c_str());
  if (program == nullptr) {
    return {};
  }
  std::string result(program);
  g_free(program);
  return result;
}

static std::vector<std::string> copy_restart_environment(
    const std::optional<std::string> &inherited_language) {
  // Localization can replace LANGUAGE for this process. A restart that selects
  // the system default must instead inherit the value from before that change.
  std::vector<std::string> environment;
  for (char **entry = environ; entry != nullptr && *entry != nullptr;
       ++entry) {
    const std::string value(*entry);
    if (value.rfind(language_environment_prefix, 0) != 0) {
      environment.push_back(value);
    }
  }
  if (inherited_language.has_value()) {
    environment.push_back(std::string(language_environment_prefix) +
                          inherited_language.value());
  }
  return environment;
}

static int restart_launcher(
    const std::string &executable,
    const std::vector<std::string> &process_arguments,
    const std::optional<std::string> &inherited_language) {
  if (executable.empty()) {
    std::cerr << "Error: unable to resolve elder-terms for restart\n";
    return 1;
  }

  std::vector<std::string> argument_storage = process_arguments;
  std::vector<char *> argument_pointers;
  argument_pointers.reserve(argument_storage.size() + 1);
  for (std::string &argument : argument_storage) {
    argument_pointers.push_back(argument.data());
  }
  argument_pointers.push_back(nullptr);

  std::vector<std::string> environment_storage =
      copy_restart_environment(inherited_language);
  std::vector<char *> environment_pointers;
  environment_pointers.reserve(environment_storage.size() + 1);
  for (std::string &entry : environment_storage) {
    environment_pointers.push_back(entry.data());
  }
  environment_pointers.push_back(nullptr);

  execve(executable.c_str(), argument_pointers.data(),
         environment_pointers.data());
  const int error = errno;
  std::cerr << "Error: failed to restart elder-terms: "
            << std::strerror(error) << '\n';
  return 1;
}

static std::optional<std::filesystem::path>
create_temporary_startup_profile(ApplicationState *state,
                                 std::vector<std::string> *warnings) {
  gchar *temporary_path = nullptr;
  GError *error = nullptr;
  const int descriptor = g_file_open_tmp("elder-terms-startup-XXXXXX",
                                         &temporary_path, &error);
  if (descriptor == -1) {
    warnings->push_back(error == nullptr ? _("Failed to create startup profile")
                                         : error->message);
    if (error != nullptr) {
      g_error_free(error);
    }
    return std::nullopt;
  }
  close(descriptor);

  const std::filesystem::path path(temporary_path);
  g_free(temporary_path);
  elder_terms::SettingsStore startup_store =
      elder_terms::settings_widget_draft_store(state->settings_widget);
  elder_terms::set_explicit_setting_value(
      &startup_store, elder_terms::general_name_setting_key(),
      elder_terms::SettingValue{
          elder_terms::general_connection_name(startup_store)});
  const elder_terms::SettingsSaveResult result = elder_terms::save_settings(
      startup_store, path);
  warnings->insert(warnings->end(), result.warnings.begin(),
                   result.warnings.end());
  if (!result.saved) {
    g_remove(path.c_str());
    return std::nullopt;
  }
  return path;
}

static void on_child_finished(GObject *source, GAsyncResult *result,
                              gpointer user_data) {
  auto *launch = static_cast<ChildLaunch *>(user_data);
  GError *error = nullptr;
  if (!g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error)) {
    if (error != nullptr) {
      std::cerr << error->message << '\n';
      g_error_free(error);
    }
  }
  if (!launch->temporary_startup_path.empty()) {
    g_remove(launch->temporary_startup_path.c_str());
  }
  if (launch->application != nullptr) {
    auto &launches = launch->application->child_launches;
    launches.erase(std::remove(launches.begin(), launches.end(), launch),
                   launches.end());
  }
  g_object_unref(launch->process);
  delete launch;
}

static void launch_child_process(
    ApplicationState *state, const std::string &executable,
    const char *executable_name,
    const std::vector<std::string> &arguments,
    const std::filesystem::path &temporary_startup_path,
    const std::optional<std::string> &activation_token) {
  const std::string error_summary =
      format_translated_string(_("Failed to start %s"), executable_name);
  if (executable.empty()) {
    if (!temporary_startup_path.empty()) {
      g_remove(temporary_startup_path.c_str());
    }
    show_error(state, error_summary,
               {format_translated_string(
                   _("The %s executable was not found"), executable_name)});
    return;
  }

  std::vector<const gchar *> argv;
  argv.reserve(arguments.size() + 1);
  for (const std::string &argument : arguments) {
    argv.push_back(argument.c_str());
  }
  argv.push_back(nullptr);

  GSubprocessLauncher *launcher =
      g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
  if (activation_token.has_value() &&
      !activation_token->empty()) {
    g_subprocess_launcher_setenv(
        launcher, "XDG_ACTIVATION_TOKEN",
        activation_token->c_str(), TRUE);
  }
  GError *error = nullptr;
  GSubprocess *process =
      g_subprocess_launcher_spawnv(launcher, argv.data(), &error);
  g_object_unref(launcher);
  if (process == nullptr) {
    if (!temporary_startup_path.empty()) {
      g_remove(temporary_startup_path.c_str());
    }
    const std::string message =
        error == nullptr ? _("Unknown process launch error") : error->message;
    if (error != nullptr) {
      g_error_free(error);
    }
    show_error(state, error_summary, {message});
    return;
  }

  auto *launch = new ChildLaunch{
      .application = state,
      .process = process,
      .temporary_startup_path = temporary_startup_path,
  };
  state->child_launches.push_back(launch);
  g_subprocess_wait_async(process, nullptr, on_child_finished, launch);
}

static void launch_selected_connection(ApplicationState *state) {
  if (!editor_is_valid(state)) {
    return;
  }
  const elder_terms::SettingsStore &draft_store =
      elder_terms::settings_widget_draft_store(state->settings_widget);
  const elder_terms::ConnectionKind kind =
      elder_terms::general_connection_kind(draft_store);
  const bool file_transfer =
      kind == elder_terms::ConnectionKind::sftp ||
      kind == elder_terms::ConnectionKind::ftp;
  const std::string &executable =
      file_transfer ? state->file_transfer_executable
                    : state->vte_executable;
  const char *executable_name =
      file_transfer ? "elder-terms-file-transfer" : "elder-terms-vte";
  const std::string error_summary =
      format_translated_string(_("Failed to start %s"), executable_name);

  std::vector<std::string> arguments = {executable};
  if (!state->current_is_new && state->selected_path.has_value()) {
    arguments.emplace_back("-c");
    arguments.push_back(state->selected_path->string());
  }

  std::filesystem::path temporary_startup_path;
  if (state->current_is_new ||
      elder_terms::settings_widget_is_dirty(state->settings_widget)) {
    std::vector<std::string> warnings;
    const auto startup_path =
        create_temporary_startup_profile(state, &warnings);
    print_warnings(warnings);
    if (!startup_path.has_value()) {
      show_error(state, error_summary, warnings);
      return;
    }
    temporary_startup_path = startup_path.value();
    arguments.emplace_back("-s");
    arguments.push_back(temporary_startup_path.string());
  }

  launch_child_process(state, executable, executable_name, arguments,
                       temporary_startup_path, std::nullopt);
}

static void launch_saved_connection(
    ApplicationState *state, const std::filesystem::path &path,
    const std::optional<std::string> &activation_token) {
  const elder_terms::SettingsLoadResult loaded =
      elder_terms::load_connection_profile(path);
  print_warnings(loaded.warnings);
  if (!loaded.loaded) {
    show_error(state, _("Failed to load connection"), loaded.warnings);
    return;
  }

  const elder_terms::ConnectionKind kind =
      elder_terms::general_connection_kind(loaded.store);
  const bool file_transfer =
      kind == elder_terms::ConnectionKind::sftp ||
      kind == elder_terms::ConnectionKind::ftp;
  const std::string &executable =
      file_transfer ? state->file_transfer_executable
                    : state->vte_executable;
  const char *executable_name =
      file_transfer ? "elder-terms-file-transfer" : "elder-terms-vte";
  const std::vector<std::string> arguments = {
      executable,
      "-c",
      path.string(),
  };
  launch_child_process(state, executable, executable_name, arguments, {},
                       activation_token);
}

static void perform_pending_action(ApplicationState *state,
                                   PendingAction action) {
  if (action.kind == PendingActionKind::select) {
    select_existing_connection(state, action.path);
  } else if (action.kind == PendingActionKind::new_connection) {
    begin_new_connection(state);
  } else if (action.kind == PendingActionKind::close) {
    gtk_widget_destroy(state->main_window->window);
  } else if (action.kind == PendingActionKind::quit) {
    state->quitting = true;
    gtk_widget_destroy(state->main_window->window);
  } else if (action.kind == PendingActionKind::restart) {
    state->restart_requested = true;
    state->quitting = true;
    gtk_widget_destroy(state->main_window->window);
  }
}

static void on_confirmation_response(GtkDialog *dialog, gint response,
                                     gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  const PendingAction action = state->pending_action;
  state->pending_action = {};
  state->confirmation_dialog = nullptr;
  gtk_widget_destroy(GTK_WIDGET(dialog));
  if (response == GTK_RESPONSE_ACCEPT) {
    perform_pending_action(state, action);
  }
}

static void request_discard_confirmation(ApplicationState *state,
                                         PendingAction action) {
  if (state->confirmation_dialog != nullptr) {
    gtk_window_present(GTK_WINDOW(state->confirmation_dialog));
    return;
  }
  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(state->main_window->window),
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", _("Discard changes?"));
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dialog), "%s",
      _("The current connection has changes that have not been applied."));
  GtkWidget *cancel = gtk_dialog_add_button(
      GTK_DIALOG(dialog), _("Cancel"), GTK_RESPONSE_CANCEL);
  GtkWidget *discard = gtk_dialog_add_button(
      GTK_DIALOG(dialog), _("Discard"), GTK_RESPONSE_ACCEPT);
  gestament_gtk_assign_accessible_id(dialog, "discard_changes_dialog");
  gestament_gtk_assign_accessible_id(cancel, "cancel_discard_button");
  gestament_gtk_assign_accessible_id(discard, "discard_changes_button");
  state->pending_action = std::move(action);
  state->confirmation_dialog = dialog;
  g_signal_connect(dialog, "response", G_CALLBACK(on_confirmation_response),
                   state);
  gtk_widget_show_all(dialog);
}

static void request_application_restart(ApplicationState *state) {
  if (state == nullptr || state->quitting || state->window_destroyed) {
    return;
  }
  if (editor_is_dirty(state)) {
    request_discard_confirmation(
        state, PendingAction{
                   .kind = PendingActionKind::restart,
                   .path = {},
               });
    return;
  }
  perform_pending_action(
      state, PendingAction{
                 .kind = PendingActionKind::restart,
                 .path = {},
             });
}

static void on_connection_selection_changed(GtkTreeSelection *,
                                            gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state->suppress_selection) {
    return;
  }
  const ConnectionRow row = selected_row(state);
  if (!row.valid || row_matches_current(state, row)) {
    return;
  }
  if (editor_is_dirty(state)) {
    restore_current_selection(state);
    request_discard_confirmation(
        state, PendingAction{
                   .kind = PendingActionKind::select,
                   .path = row.path,
               });
    return;
  }
  if (!load_existing_connection(state, row.path)) {
    restore_current_selection(state);
  }
}

static gboolean on_connection_list_button_press(GtkWidget *widget,
                                                GdkEventButton *event,
                                                gpointer user_data) {
  if (event == nullptr || event->type != GDK_BUTTON_PRESS ||
      event->button != GDK_BUTTON_SECONDARY) {
    return GDK_EVENT_PROPAGATE;
  }

  auto *state = static_cast<ApplicationState *>(user_data);
  GtkTreePath *path = nullptr;
  if (!gtk_tree_view_get_path_at_pos(
          GTK_TREE_VIEW(widget), static_cast<gint>(event->x),
          static_cast<gint>(event->y), &path, nullptr, nullptr, nullptr)) {
    return GDK_EVENT_PROPAGATE;
  }
  const ConnectionRow row = connection_row_at_path(state, path);
  if (!row.valid || row.is_new) {
    gtk_tree_path_free(path);
    return GDK_EVENT_PROPAGATE;
  }

  const bool target_is_current = row_matches_current(state, row);
  if (editor_is_dirty(state) && !target_is_current) {
    gtk_tree_path_free(path);
    return GDK_EVENT_STOP;
  }

  if (!target_is_current) {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(
        GTK_TREE_VIEW(state->main_window->connection_list));
    state->suppress_selection = true;
    gtk_tree_selection_select_path(selection, path);
    state->suppress_selection = false;
    if (!load_existing_connection(state, row.path)) {
      restore_current_selection(state);
      gtk_tree_path_free(path);
      return GDK_EVENT_STOP;
    }
  }
  gtk_tree_path_free(path);

  state->context_connection_path = row.path;
  state->context_connection_name = row.name;
  gtk_widget_grab_focus(widget);
  gtk_menu_popup_at_pointer(
      GTK_MENU(state->main_window->connection_context_menu),
      reinterpret_cast<GdkEvent *>(event));
  return GDK_EVENT_STOP;
}

static void start_context_connection_rename(ApplicationState *state) {
  if (state->shutting_down) {
    return;
  }
  if (!state->context_connection_path.has_value() ||
      !state->selected_path.has_value() || state->current_is_new ||
      state->context_connection_path.value() !=
          state->selected_path.value()) {
    return;
  }
  state->suppress_selection = true;
  const bool selected = select_matching_row(
      state, state->context_connection_path, false);
  state->suppress_selection = false;
  if (!selected) {
    return;
  }
  start_name_editing(state, true);
}

static void on_rename_connection_menu_item_activate(GtkMenuItem *,
                                                    gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  gtk_menu_popdown(GTK_MENU(state->main_window->connection_context_menu));
  start_context_connection_rename(state);
}

static void on_duplicate_connection_menu_item_activate(GtkMenuItem *,
                                                       gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  gtk_menu_popdown(GTK_MENU(state->main_window->connection_context_menu));
  if (state->shutting_down ||
      !state->context_connection_path.has_value() ||
      !state->selected_path.has_value() || state->current_is_new ||
      state->context_connection_path.value() !=
          state->selected_path.value()) {
    return;
  }

  const std::vector<elder_terms::ConnectionProfile> profiles =
      elder_terms::list_connection_profiles(state->connection_directory);
  const std::string name = next_duplicate_connection_name(
      state->context_connection_name, profiles);
  const elder_terms::ConnectionSaveResult result =
      elder_terms::save_connection_profile(
          state->connection_directory, std::nullopt, name,
          elder_terms::settings_widget_draft_store(state->settings_widget));
  print_warnings(result.warnings);
  if (!result.saved) {
    show_error(state, _("Failed to duplicate connection"), result.warnings);
    return;
  }

  select_existing_connection(state, result.path);
  reload_hotkey_actions(state);
}

static void on_delete_connection_dialog_destroy(GtkWidget *dialog,
                                                gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state->delete_connection_dialog == dialog) {
    state->delete_connection_dialog = nullptr;
    state->delete_connection_path.reset();
  }
}

static void on_delete_connection_dialog_response(GtkDialog *dialog,
                                                 gint response,
                                                 gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  const std::optional<std::filesystem::path> path =
      state->delete_connection_path;
  gtk_widget_destroy(GTK_WIDGET(dialog));
  if (response != GTK_RESPONSE_ACCEPT || !path.has_value()) {
    return;
  }

  stop_connection_monitor(state);
  const elder_terms::ConnectionDeleteResult result =
      elder_terms::delete_connection_profile(path.value());
  start_connection_monitor(state);
  print_warnings(result.warnings);
  if (!result.deleted) {
    show_error(state, _("Failed to delete connection"), result.warnings);
    return;
  }

  preserve_current_editor_after_list_refresh(state);
  reload_hotkey_actions(state);
}

static void on_delete_connection_menu_item_activate(GtkMenuItem *,
                                                    gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  gtk_menu_popdown(GTK_MENU(state->main_window->connection_context_menu));
  if (!state->context_connection_path.has_value() ||
      !state->selected_path.has_value() || state->current_is_new ||
      state->context_connection_path.value() !=
          state->selected_path.value()) {
    return;
  }
  if (state->delete_connection_dialog != nullptr) {
    gtk_window_present(GTK_WINDOW(state->delete_connection_dialog));
    return;
  }

  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(state->main_window->window),
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", _("Delete connection?"));
  const std::string secondary = format_translated_string(
      _("The connection \"%s\" will be permanently deleted."),
      state->context_connection_name.c_str());
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dialog), "%s", secondary.c_str());
  GtkWidget *cancel = gtk_dialog_add_button(
      GTK_DIALOG(dialog), _("Cancel"), GTK_RESPONSE_CANCEL);
  GtkWidget *remove = gtk_dialog_add_button(
      GTK_DIALOG(dialog), _("Delete"), GTK_RESPONSE_ACCEPT);
  gestament_gtk_assign_accessible_id(dialog, "delete_connection_dialog");
  gestament_gtk_assign_accessible_id(
      cancel, "cancel_delete_connection_button");
  gestament_gtk_assign_accessible_id(remove, "delete_connection_button");
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  state->delete_connection_path = state->context_connection_path;
  state->delete_connection_dialog = dialog;
  g_signal_connect(dialog, "response",
                   G_CALLBACK(on_delete_connection_dialog_response), state);
  g_signal_connect(dialog, "destroy",
                   G_CALLBACK(on_delete_connection_dialog_destroy), state);
  gtk_widget_show_all(dialog);
}

static void on_name_edited(GtkCellRendererText *, gchar *path_text,
                           gchar *new_text, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  const bool rename_existing =
      state->rename_existing_on_edit && !state->current_is_new &&
      state->selected_path.has_value();
  state->rename_existing_on_edit = false;
  g_object_set(state->main_window->connection_name_renderer, "editable", FALSE,
               nullptr);
  GtkTreePath *path = gtk_tree_path_new_from_string(path_text);
  GtkTreeIter iterator;
  if (path == nullptr ||
      !gtk_tree_model_get_iter(GTK_TREE_MODEL(state->main_window->connection_store),
                               &iterator, path)) {
    if (path != nullptr) {
      gtk_tree_path_free(path);
    }
    return;
  }
  gtk_tree_path_free(path);

  const auto validation = elder_terms::validate_connection_name(
      new_text == nullptr ? std::string() : std::string(new_text),
      state->profiles, state->selected_path);
  if (rename_existing) {
    if (!validation.valid) {
      show_error(state, _("Failed to rename connection"),
                 {validation.error});
      return;
    }

    stop_connection_monitor(state);
    const elder_terms::ConnectionRenameResult result =
        elder_terms::rename_connection_profile(
            state->connection_directory, state->selected_path.value(),
            validation.name);
    start_connection_monitor(state);
    print_warnings(result.warnings);
    if (!result.renamed) {
      show_error(state, _("Failed to rename connection"), result.warnings);
      return;
    }

    state->selected_path = result.path;
    state->persisted_name = validation.name;
    state->draft_name = validation.name;
    state->name_error.clear();
    state->name_dirty = false;
    elder_terms::settings_widget_set_default_connection_name(
        state->settings_widget, validation.name);
    preserve_current_editor_after_list_refresh(state);
    reload_hotkey_actions(state);
    update_action_sensitivity(state);
    return;
  }

  state->draft_name = validation.name;
  state->name_error = validation.valid ? std::string() : validation.error;
  state->name_dirty =
      state->current_is_new || state->draft_name != state->persisted_name;
  if (validation.valid) {
    elder_terms::settings_widget_set_default_connection_name(
        state->settings_widget, state->draft_name);
  }
  gtk_list_store_set(state->main_window->connection_store, &iterator,
                     connection_name_column, state->draft_name.c_str(), -1);
  update_action_sensitivity(state);
}

static void on_name_editing_canceled(GtkCellRenderer *, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  state->rename_existing_on_edit = false;
  g_object_set(state->main_window->connection_name_renderer, "editable", FALSE,
               nullptr);
}

static void on_name_editing_started(GtkCellRenderer *,
                                    GtkCellEditable *editable, gchar *,
                                    gpointer) {
  if (GTK_IS_WIDGET(editable)) {
    gestament_gtk_assign_accessible_id(GTK_WIDGET(editable),
                                       "connection_name_editor");
  }
}

static gboolean on_connection_list_key_press(GtkWidget *, GdkEventKey *event,
                                             gpointer user_data) {
  if (event->keyval != GDK_KEY_F2) {
    return FALSE;
  }
  start_name_editing(static_cast<ApplicationState *>(user_data), true);
  return TRUE;
}

static gboolean on_rename_accelerator(GtkAccelGroup *, GObject *, guint,
                                      GdkModifierType, gpointer user_data) {
  start_name_editing(static_cast<ApplicationState *>(user_data), true);
  return TRUE;
}

static gboolean on_close_accelerator(GtkAccelGroup *, GObject *, guint,
                                     GdkModifierType, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  gtk_window_close(GTK_WINDOW(state->main_window->window));
  return TRUE;
}

static void on_new_clicked(GtkButton *, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (editor_is_dirty(state)) {
    request_discard_confirmation(
        state, PendingAction{
                   .kind = PendingActionKind::new_connection,
                   .path = {},
               });
    return;
  }
  begin_new_connection(state);
}

static void on_apply_clicked(GtkButton *, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  const auto validation = elder_terms::validate_connection_name(
      state->draft_name, state->profiles, state->selected_path);
  if (!validation.valid ||
      !elder_terms::settings_widget_is_valid(state->settings_widget)) {
    update_action_sensitivity(state);
    return;
  }
  const elder_terms::ConnectionSaveResult result =
      elder_terms::save_connection_profile(
          state->connection_directory, state->selected_path, validation.name,
          elder_terms::settings_widget_draft_store(state->settings_widget));
  print_warnings(result.warnings);
  if (!result.saved) {
    show_error(state, _("Failed to save connection"), result.warnings);
    return;
  }
  select_existing_connection(state, result.path);
  reload_hotkey_actions(state);
}

static void on_connect_clicked(GtkButton *, gpointer user_data) {
  launch_selected_connection(static_cast<ApplicationState *>(user_data));
}

static void on_connection_row_activated(GtkTreeView *, GtkTreePath *,
                                        GtkTreeViewColumn *,
                                        gpointer user_data) {
  launch_selected_connection(static_cast<ApplicationState *>(user_data));
}

static std::optional<std::uint32_t> current_x11_server_time(
    GtkWidget *widget) {
  if (widget == nullptr || !gtk_widget_get_realized(widget)) {
    return std::nullopt;
  }
  GdkWindow *window = gtk_widget_get_window(widget);
  if (window == nullptr || !GDK_IS_X11_WINDOW(window)) {
    return std::nullopt;
  }

  const GdkEventMask event_mask = gdk_window_get_events(window);
  if ((event_mask & GDK_PROPERTY_CHANGE_MASK) == 0) {
    gdk_window_set_events(
        window,
        static_cast<GdkEventMask>(
            event_mask | GDK_PROPERTY_CHANGE_MASK));
  }
  const std::uint32_t timestamp = gdk_x11_get_server_time(window);
  return timestamp == GDK_CURRENT_TIME
             ? std::optional<std::uint32_t>()
             : std::optional<std::uint32_t>(timestamp);
}

static void present_main_window(
    ApplicationState *state,
    std::optional<std::uint32_t> activation_time,
    std::optional<std::string> activation_token) {
  if (state == nullptr || state->main_window == nullptr ||
      state->window_destroyed) {
    return;
  }
  if (activation_token.has_value() &&
      !activation_token->empty()) {
    gtk_window_set_startup_id(
        GTK_WINDOW(state->main_window->window),
        activation_token->c_str());
  }
  gtk_widget_show_all(state->main_window->window);
  enable_connection_selection(state);
  if (!activation_time.has_value()) {
    // StatusNotifierItem activation does not carry an event timestamp. A
    // fresh X server timestamp preserves the user-activation context needed
    // by the window manager to grant focus.
    activation_time =
        current_x11_server_time(state->main_window->window);
  }
  if (activation_time.has_value()) {
    gtk_window_present_with_time(
        GTK_WINDOW(state->main_window->window),
        activation_time.value());
  } else {
    gtk_window_present(GTK_WINDOW(state->main_window->window));
  }
}

static void release_application_hold(ApplicationState *state) {
  if (state->application_held) {
    g_application_release(G_APPLICATION(state->application));
    state->application_held = false;
  }
}

static void quit_application_loop(ApplicationState *state) {
  release_application_hold(state);
  g_application_quit(G_APPLICATION(state->application));
}

static void request_application_quit(ApplicationState *state) {
  if (state == nullptr || state->quitting) {
    return;
  }
  if (state->main_window == nullptr || state->window_destroyed) {
    state->quitting = true;
    quit_application_loop(state);
    return;
  }
  if (editor_is_dirty(state)) {
    present_main_window(state);
    request_discard_confirmation(
        state, PendingAction{
                   .kind = PendingActionKind::quit,
                   .path = {},
               });
    return;
  }
  state->quitting = true;
  gtk_widget_destroy(state->main_window->window);
}

static gboolean on_window_delete(GtkWidget *, GdkEvent *, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state->tray_availability ==
          elder_terms::TrayBackendAvailabilityState::available &&
      !state->quitting) {
    gtk_widget_hide(state->main_window->window);
    return TRUE;
  }
  if (!editor_is_dirty(state)) {
    return FALSE;
  }
  request_discard_confirmation(
      state, PendingAction{
                 .kind = PendingActionKind::close,
                 .path = {},
             });
  return TRUE;
}

static void on_window_destroy(GtkWidget *, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state->window_destroyed) {
    return;
  }
  state->window_destroyed = true;
  state->shutting_down = true;
  stop_connection_monitor(state);
  for (ChildLaunch *launch : state->child_launches) {
    launch->application = nullptr;
    if (!launch->temporary_startup_path.empty()) {
      g_remove(launch->temporary_startup_path.c_str());
      launch->temporary_startup_path.clear();
    }
  }
  close_application_dialog(state);
  close_global_defaults_dialog(state);
  if (state->settings_widget != nullptr) {
    elder_terms::destroy_settings_widget(state->settings_widget);
    state->settings_widget = nullptr;
  }
  if (!state->application_shutting_down &&
      (state->quitting ||
       state->tray_availability !=
           elder_terms::TrayBackendAvailabilityState::available)) {
    quit_application_loop(state);
  }
}

static bool initialize_main_window(ApplicationState *state) {
  state->main_window_storage =
      elder_terms::load_launcher_main_window();
  if (!state->main_window_storage.has_value()) {
    return false;
  }
  state->main_window = &state->main_window_storage.value();
  elder_terms::LauncherMainWindow *main_window = state->main_window;

  elder_terms::SettingsWidgetCallbacks callbacks;
  callbacks.changed = [state]() { update_action_sensitivity(state); };
  state->settings_widget = elder_terms::create_settings_widget({
      .store = elder_terms::create_default_settings(
          elder_terms::default_terminal_display_settings(1.0),
          "elder-terms"),
      .is_runtime = false,
      .show_actions = false,
      .callbacks = std::move(callbacks),
  });
  gtk_container_add(
      GTK_CONTAINER(main_window->settings_container),
      elder_terms::settings_widget_root(state->settings_widget));

  GtkTreeSelection *selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(main_window->connection_list));
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_NONE);
  g_signal_connect(selection, "changed",
                   G_CALLBACK(on_connection_selection_changed), state);
  g_signal_connect(main_window->connection_name_renderer, "edited",
                   G_CALLBACK(on_name_edited), state);
  g_signal_connect(main_window->connection_name_renderer, "editing-canceled",
                   G_CALLBACK(on_name_editing_canceled), state);
  g_signal_connect(main_window->connection_name_renderer, "editing-started",
                   G_CALLBACK(on_name_editing_started), state);
  gtk_menu_attach_to_widget(
      GTK_MENU(main_window->connection_context_menu),
      main_window->connection_list, nullptr);
  gtk_widget_show_all(main_window->connection_context_menu);
  g_signal_connect(main_window->connection_list, "button-press-event",
                   G_CALLBACK(on_connection_list_button_press), state);
  g_signal_connect(main_window->rename_connection_menu_item, "activate",
                   G_CALLBACK(on_rename_connection_menu_item_activate),
                   state);
  g_signal_connect(main_window->duplicate_connection_menu_item, "activate",
                   G_CALLBACK(on_duplicate_connection_menu_item_activate),
                   state);
  g_signal_connect(main_window->delete_connection_menu_item, "activate",
                   G_CALLBACK(on_delete_connection_menu_item_activate),
                   state);
  g_signal_connect(
      main_window->application_settings_menu_item, "activate",
      G_CALLBACK(on_application_settings_menu_item_activate), state);
  g_signal_connect(main_window->about_menu_item, "activate",
                   G_CALLBACK(on_about_menu_item_activate), state);
  g_signal_connect(main_window->connection_list, "key-press-event",
                   G_CALLBACK(on_connection_list_key_press), state);
  g_signal_connect(main_window->window, "key-press-event",
                   G_CALLBACK(on_connection_list_key_press), state);
  g_signal_connect_after(main_window->window, "focus-in-event",
                         G_CALLBACK(on_main_window_focus_in), state);
  g_signal_connect(main_window->new_button, "clicked",
                   G_CALLBACK(on_new_clicked), state);
  g_signal_connect(main_window->global_defaults_button, "clicked",
                   G_CALLBACK(on_global_defaults_clicked), state);
  g_signal_connect(main_window->action_row, "button-press-event",
                   G_CALLBACK(on_window_drag_button_press),
                   main_window->window);
  g_signal_connect(main_window->apply_button, "clicked",
                   G_CALLBACK(on_apply_clicked), state);
  g_signal_connect(main_window->connect_button, "clicked",
                   G_CALLBACK(on_connect_clicked), state);
  g_signal_connect(main_window->connection_list, "row-activated",
                   G_CALLBACK(on_connection_row_activated), state);
  g_signal_connect(main_window->window, "delete-event",
                   G_CALLBACK(on_window_delete), state);
  g_signal_connect(main_window->window, "destroy",
                   G_CALLBACK(on_window_destroy), state);

  GtkAccelGroup *application_accelerators = gtk_accel_group_new();
  GClosure *rename_closure =
      g_cclosure_new(G_CALLBACK(on_rename_accelerator), state, nullptr);
  gtk_accel_group_connect(application_accelerators, GDK_KEY_F2,
                          static_cast<GdkModifierType>(0), GTK_ACCEL_VISIBLE,
                          rename_closure);
  GClosure *close_closure =
      g_cclosure_new(G_CALLBACK(on_close_accelerator), state, nullptr);
  gtk_accel_group_connect(application_accelerators, GDK_KEY_w,
                          GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, close_closure);
  gtk_window_add_accel_group(GTK_WINDOW(main_window->window),
                             application_accelerators);
  g_object_unref(application_accelerators);

  populate_profile_rows(state);
  state->vte_executable = resolve_child_executable(
      state->launcher_argv0.c_str(), "ELDER_TERMS_VTE_PATH",
      "elder-terms-vte");
  state->file_transfer_executable = resolve_child_executable(
      state->launcher_argv0.c_str(),
      "ELDER_TERMS_FILE_TRANSFER_PATH",
      "elder-terms-file-transfer");
  start_connection_monitor(state);
  show_empty_details(state);
  state->suppress_selection = true;
  gtk_tree_selection_unselect_all(selection);
  state->suppress_selection = false;
  show_empty_details(state);
  gtk_stack_set_visible_child_name(GTK_STACK(main_window->details_stack),
                                   "empty");
  update_action_sensitivity(state);
  return true;
}

static void enable_connection_selection(ApplicationState *state) {
  GtkTreeSelection *selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(state->main_window->connection_list));
  if (gtk_tree_selection_get_mode(selection) != GTK_SELECTION_NONE) {
    return;
  }
  state->suppress_selection = true;
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
  gtk_tree_selection_unselect_all(selection);
  state->suppress_selection = false;
  show_empty_details(state);
}

static void on_tray_availability_changed(ApplicationState *state,
                                         elder_terms::TrayBackendAvailabilityState
                                             availability) {
  if (state->application_shutting_down ||
      state->window_destroyed) {
    return;
  }
  state->tray_availability = availability;
  if (availability ==
      elder_terms::TrayBackendAvailabilityState::available) {
    // After the initial tray registration succeeds, a later host loss uses
    // the normal window fallback even when the process began at login.
    state->initial_activation_from_autostart = false;
  }
  if (availability ==
          elder_terms::TrayBackendAvailabilityState::unavailable &&
      state->activated &&
      state->startup_mode == elder_terms::StartupMode::tray &&
      !state->initial_activation_from_autostart) {
    present_main_window(state);
  }
}

static void on_application_startup(GApplication *,
                                   gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  g_set_prgname(elder_terms::launcher_application_id());
  (void)elder_terms::initialize_application_window_icon();
  state->connection_directory =
      elder_terms::default_connection_directory();
  state->global_config_path =
      elder_terms::default_global_config_path();
  const elder_terms::InitialConnectionProfileResult initial_profile =
      elder_terms::create_initial_local_terminal_profile(
          state->connection_directory);
  print_warnings(initial_profile.warnings);
  elder_terms::SettingsLoadResult global_settings =
      elder_terms::load_global_settings(state->global_config_path, 1.0);
  print_warnings(global_settings.warnings);
  state->startup_mode =
      elder_terms::application_startup_mode(global_settings.store);
  if (!initialize_main_window(state)) {
    state->startup_failed = true;
    g_application_quit(G_APPLICATION(state->application));
    return;
  }

  g_application_hold(state->application);
  state->application_held = true;
  std::vector<std::string> hotkey_warnings;
  const std::vector<elder_terms::HotkeyAction> hotkey_actions =
      build_registered_hotkey_actions(
          state, global_settings.store, &hotkey_warnings);
  print_warnings(hotkey_warnings);
  state->hotkey_backend = elder_terms::create_hotkey_backend(
      {
          .application = state->application,
          .dispatcher = state->dispatcher,
          .activated =
              [state](
                  const std::string &action_id,
                  const elder_terms::HotkeyActivationContext &context) {
                if (action_id != open_application_hotkey_action_id) {
                  const auto target = std::find_if(
                      state->connection_hotkey_targets.begin(),
                      state->connection_hotkey_targets.end(),
                      [&action_id](
                          const ConnectionHotkeyTarget &candidate) {
                        return candidate.action_id == action_id;
                      });
                  if (target !=
                      state->connection_hotkey_targets.end()) {
                    launch_saved_connection(
                        state, target->path,
                        context.activation_token);
                  }
                  return;
                }
                present_main_window(state, context.activation_time,
                                    context.activation_token);
              },
          .registration_failed = [state]() {
            show_hotkey_registration_error(state);
          },
      },
      hotkey_actions);
  if (state->startup_mode == elder_terms::StartupMode::window ||
      state->startup_mode == elder_terms::StartupMode::background) {
    return;
  }
  state->tray_backend = elder_terms::create_tray_backend({
      .application = state->application,
      .dispatcher = state->dispatcher,
      .identifier = "elder-terms",
      .title = "elder-terms",
      .icon_name = "elder-terms",
      .callbacks =
          {
              .activate =
                  [state](
                      const elder_terms::TrayActivationContext &context) {
                    present_main_window(state,
                                        context.activation_time);
                  },
              .application_settings = [state]() {
                open_application_dialog(
                    state, ApplicationDialogPage::application);
              },
              .about = [state]() {
                open_application_dialog(state,
                                        ApplicationDialogPage::about);
              },
              .quit = [state]() {
                request_application_quit(state);
              },
              .availability_changed =
                  [state](elder_terms::TrayBackendAvailabilityState
                              availability) {
                on_tray_availability_changed(state, availability);
              },
          },
  });
}

static void on_application_activate(GApplication *,
                                    gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state->startup_failed || state->main_window == nullptr ||
      state->window_destroyed) {
    return;
  }
  if (state->activated) {
    present_main_window(state);
    return;
  }
  state->activated = true;
  if (state->startup_mode == elder_terms::StartupMode::background) {
    return;
  }
  if (state->startup_mode == elder_terms::StartupMode::tray) {
    if (state->tray_availability ==
            elder_terms::TrayBackendAvailabilityState::unavailable &&
        !state->initial_activation_from_autostart) {
      present_main_window(state);
    }
    return;
  }
  present_main_window(state);
}

static gint on_application_command_line(
    GApplication *application,
    GApplicationCommandLine *command_line,
    gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  gboolean autostart = FALSE;
  gboolean application_settings = FALSE;
  gboolean about = FALSE;
  GVariantDict *options =
      g_application_command_line_get_options_dict(command_line);
  if (options != nullptr) {
    (void)g_variant_dict_lookup(options, autostart_option_name, "b",
                                &autostart);
    (void)g_variant_dict_lookup(options, application_settings_option_name,
                                "b", &application_settings);
    (void)g_variant_dict_lookup(options, about_option_name, "b", &about);
  }
  if (application_settings != FALSE || about != FALSE) {
    state->activated = true;
    open_application_dialog(
        state, about != FALSE ? ApplicationDialogPage::about
                              : ApplicationDialogPage::application);
    return 0;
  }
  if (autostart != FALSE && state->activated) {
    return 0;
  }
  if (!state->activated) {
    state->initial_activation_from_autostart = autostart != FALSE;
  }
  g_application_activate(application);
  return 0;
}

static void on_application_shutdown(GApplication *,
                                    gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  state->application_shutting_down = true;
  if (state->hotkey_backend != nullptr) {
    elder_terms::destroy_hotkey_backend(state->hotkey_backend);
    state->hotkey_backend = nullptr;
  }
  if (state->tray_backend != nullptr) {
    elder_terms::destroy_tray_backend(state->tray_backend);
    state->tray_backend = nullptr;
  }
  if (state->main_window != nullptr && !state->window_destroyed) {
    gtk_widget_destroy(state->main_window->window);
  }
  release_application_hold(state);
  state->dispatcher_group->shutdown();
}

} // namespace

int main(int argc, char **argv) {
  const std::vector<std::string> process_arguments =
      copy_process_arguments(argc, argv);
  const std::string restart_executable =
      resolve_launcher_executable(process_arguments.front());
  const char *inherited_language_value =
      g_getenv(language_environment_name);
  const std::optional<std::string> inherited_language =
      inherited_language_value == nullptr
          ? std::nullopt
          : std::optional<std::string>(inherited_language_value);
  const elder_terms::ApplicationUiLanguage ui_language =
      elder_terms::load_application_ui_language_preference(
          elder_terms::default_global_config_path());
  const elder_terms::LocalizationInitializationResult localization =
      elder_terms::initialize_localization(ui_language);
  for (const std::string &warning : localization.warnings) {
    std::cerr << warning << '\n';
  }
  gtk_disable_setlocale();

  int application_result = 1;
  bool restart_requested = false;
  // Leave this scope before execve() so dispatcher-owned descriptors and all
  // GApplication resources are released before the process image is replaced.
  {
    cardio::dispatcher_group_glib dispatcher_group;
    cardio::dispatcher_host_glib_auto dispatcher(dispatcher_group);
    GApplication *application = g_application_new(
        elder_terms::launcher_application_id(),
        G_APPLICATION_HANDLES_COMMAND_LINE);
    g_application_add_option_group(application,
                                   gtk_get_option_group(TRUE));
    g_application_add_main_option(
        application, autostart_option_name, 0, G_OPTION_FLAG_NONE,
        G_OPTION_ARG_NONE, "Start from the desktop session", nullptr);
    g_application_add_main_option(
        application, application_settings_option_name, 0,
        G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
        "Open application settings", nullptr);
    g_application_add_main_option(
        application, about_option_name, 0, G_OPTION_FLAG_NONE,
        G_OPTION_ARG_NONE, "Show application information", nullptr);

    ApplicationState state;
    state.application = application;
    state.dispatcher = &dispatcher;
    state.dispatcher_group = &dispatcher_group;
    state.launcher_argv0 = process_arguments.front();
    g_signal_connect(application, "startup",
                     G_CALLBACK(on_application_startup), &state);
    g_signal_connect(application, "activate",
                     G_CALLBACK(on_application_activate), &state);
    g_signal_connect(application, "command-line",
                     G_CALLBACK(on_application_command_line), &state);
    g_signal_connect(application, "shutdown",
                     G_CALLBACK(on_application_shutdown), &state);

    const int result = g_application_run(application, argc, argv);
    if (!state.application_shutting_down) {
      dispatcher_group.shutdown();
    }
    if (state.main_window_storage.has_value()) {
      elder_terms::destroy_launcher_main_window(
          &state.main_window_storage.value());
    }
    restart_requested = state.restart_requested && !state.startup_failed;
    application_result = state.startup_failed ? 1 : result;
    g_object_unref(application);
  }
  if (restart_requested) {
    return restart_launcher(restart_executable, process_arguments,
                            inherited_language);
  }
  return application_result;
}

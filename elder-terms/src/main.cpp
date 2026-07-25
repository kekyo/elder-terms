#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gdk/gdkkeysyms.h>
#include <gestament/gtk.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include <unistd.h>

#include <cardio.h>
#include <elder-terms/settings/application-settings.h>
#include <elder-terms/settings-widget.h>
#include <elder-terms/settings.h>

#include "connection-repository.h"
#include "main-window.h"
#include "tray-backend.h"

namespace {

enum class PendingActionKind {
  none,
  select,
  new_connection,
  close,
  quit,
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

struct ApplicationState {
  GApplication *application = nullptr;
  cardio::dispatcher *dispatcher = nullptr;
  cardio::dispatcher_group_glib *dispatcher_group = nullptr;
  std::optional<elder_terms::LauncherMainWindow> main_window_storage;
  elder_terms::LauncherMainWindow *main_window = nullptr;
  elder_terms::TrayBackendState *tray_backend = nullptr;
  elder_terms::SettingsWidgetState *settings_widget = nullptr;
  elder_terms::SettingsWidgetState *global_defaults_widget = nullptr;
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
  bool suppress_selection = false;
  GtkWidget *confirmation_dialog = nullptr;
  GtkWidget *global_defaults_dialog = nullptr;
  GtkWidget *global_defaults_save_button = nullptr;
  PendingAction pending_action;
  GFileMonitor *connection_monitor = nullptr;
  guint monitor_refresh_source = 0;
  bool monitor_reload_selected = false;
  std::optional<std::filesystem::path> monitor_renamed_path;
  std::string vte_executable;
  std::string sftp_executable;
  std::string launcher_argv0;
  std::vector<ChildLaunch *> child_launches;
  elder_terms::StartupMode startup_mode =
      elder_terms::StartupMode::window;
  bool application_held = false;
  bool activated = false;
  bool hide_when_tray_available = false;
  bool tray_available = false;
  bool quitting = false;
  bool window_destroyed = false;
  bool application_shutting_down = false;
  bool startup_failed = false;
  bool shutting_down = false;
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
static void present_main_window(
    ApplicationState *state,
    std::optional<std::uint32_t> activation_time = std::nullopt);
static void request_application_quit(ApplicationState *state);
static void enable_connection_selection(ApplicationState *state);

static void print_warnings(const std::vector<std::string> &warnings) {
  for (const std::string &warning : warnings) {
    std::cerr << warning << '\n';
  }
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

static void on_global_defaults_dialog_destroy(GtkWidget *,
                                              gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  state->global_defaults_dialog = nullptr;
  state->global_defaults_save_button = nullptr;
  if (state->global_defaults_widget != nullptr) {
    elder_terms::destroy_settings_widget(state->global_defaults_widget);
    state->global_defaults_widget = nullptr;
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
                          "Failed to save global defaults", result.warnings);
    return;
  }
  print_warnings(result.warnings);
  elder_terms::settings_widget_rebase_fallbacks(state->settings_widget, store);
  update_action_sensitivity(state);
  gtk_widget_destroy(GTK_WIDGET(dialog));
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
  gtk_window_set_title(GTK_WINDOW(dialog), "Global defaults");
  gtk_window_set_transient_for(GTK_WINDOW(dialog),
                               GTK_WINDOW(state->main_window->window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 720, 420);

  GtkWidget *cancel = gtk_dialog_add_button(
      GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
  GtkWidget *save = gtk_dialog_add_button(
      GTK_DIALOG(dialog), "Save", GTK_RESPONSE_ACCEPT);
  gestament_gtk_assign_accessible_id(cancel,
                                     "global_defaults_cancel_button");
  gestament_gtk_assign_accessible_id(save, "global_defaults_save_button");
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

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

  g_signal_connect(dialog, "response",
                   G_CALLBACK(on_global_defaults_dialog_response), state);
  g_signal_connect(dialog, "destroy",
                   G_CALLBACK(on_global_defaults_dialog_destroy), state);
  update_global_defaults_save_sensitivity(state);
  gtk_widget_show_all(dialog);
}

static void on_global_defaults_clicked(GtkButton *, gpointer user_data) {
  open_global_defaults_dialog(
      static_cast<ApplicationState *>(user_data));
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
    show_error(state, "Failed to load connection", result.warnings);
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

static void start_name_editing(ApplicationState *state) {
  GtkTreeSelection *selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(state->main_window->connection_list));
  GtkTreeModel *model = nullptr;
  GtkTreeIter iterator;
  if (!gtk_tree_selection_get_selected(selection, &model, &iterator)) {
    return;
  }
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
        suffix == 1 ? "New connection"
                    : "New connection " + std::to_string(suffix);
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
  start_name_editing(state);
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
    return G_SOURCE_REMOVE;
  }

  state->monitor_reload_selected = false;
  state->monitor_renamed_path.reset();
  preserve_current_editor_after_list_refresh(state);
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

static void start_connection_monitor(ApplicationState *state) {
  std::error_code directory_error;
  std::filesystem::create_directories(state->connection_directory,
                                      directory_error);
  if (directory_error) {
    show_error(state, "Failed to monitor connections",
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
        error == nullptr ? "Unknown file monitor error" : error->message;
    if (error != nullptr) {
      g_error_free(error);
    }
    show_error(state, "Failed to monitor connections", {message});
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

static std::optional<std::filesystem::path>
create_temporary_startup_profile(ApplicationState *state,
                                 std::vector<std::string> *warnings) {
  gchar *temporary_path = nullptr;
  GError *error = nullptr;
  const int descriptor = g_file_open_tmp("elder-terms-startup-XXXXXX",
                                         &temporary_path, &error);
  if (descriptor == -1) {
    warnings->push_back(error == nullptr ? "Failed to create startup profile"
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

static void launch_selected_connection(ApplicationState *state) {
  if (!editor_is_valid(state)) {
    return;
  }
  const elder_terms::SettingsStore &draft_store =
      elder_terms::settings_widget_draft_store(state->settings_widget);
  const bool sftp =
      elder_terms::general_connection_kind(draft_store) ==
      elder_terms::ConnectionKind::sftp;
  const std::string &executable =
      sftp ? state->sftp_executable : state->vte_executable;
  const char *executable_name =
      sftp ? "elder-terms-sftp" : "elder-terms-vte";
  const std::string error_summary =
      std::string("Failed to start ") + executable_name;
  if (executable.empty()) {
    show_error(state, error_summary,
               {std::string(executable_name) +
                " executable was not found"});
    return;
  }

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

  std::vector<const gchar *> argv;
  argv.reserve(arguments.size() + 1);
  for (const std::string &argument : arguments) {
    argv.push_back(argument.c_str());
  }
  argv.push_back(nullptr);

  GError *error = nullptr;
  GSubprocess *process =
      g_subprocess_newv(argv.data(), G_SUBPROCESS_FLAGS_NONE, &error);
  if (process == nullptr) {
    if (!temporary_startup_path.empty()) {
      g_remove(temporary_startup_path.c_str());
    }
    const std::string message =
        error == nullptr ? "Unknown process launch error" : error->message;
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
      GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", "Discard changes?");
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dialog), "%s",
      "The current connection has changes that have not been applied.");
  GtkWidget *cancel = gtk_dialog_add_button(
      GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
  GtkWidget *discard = gtk_dialog_add_button(
      GTK_DIALOG(dialog), "Discard", GTK_RESPONSE_ACCEPT);
  gestament_gtk_assign_accessible_id(dialog, "discard_changes_dialog");
  gestament_gtk_assign_accessible_id(cancel, "cancel_discard_button");
  gestament_gtk_assign_accessible_id(discard, "discard_changes_button");
  state->pending_action = std::move(action);
  state->confirmation_dialog = dialog;
  g_signal_connect(dialog, "response", G_CALLBACK(on_confirmation_response),
                   state);
  gtk_widget_show_all(dialog);
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

static void on_name_edited(GtkCellRendererText *, gchar *path_text,
                           gchar *new_text, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
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
  start_name_editing(static_cast<ApplicationState *>(user_data));
  return TRUE;
}

static gboolean on_rename_accelerator(GtkAccelGroup *, GObject *, guint,
                                      GdkModifierType, gpointer user_data) {
  start_name_editing(static_cast<ApplicationState *>(user_data));
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
    show_error(state, "Failed to save connection", result.warnings);
    return;
  }
  select_existing_connection(state, result.path);
}

static void on_connect_clicked(GtkButton *, gpointer user_data) {
  launch_selected_connection(static_cast<ApplicationState *>(user_data));
}

static void on_connection_row_activated(GtkTreeView *, GtkTreePath *,
                                        GtkTreeViewColumn *,
                                        gpointer user_data) {
  launch_selected_connection(static_cast<ApplicationState *>(user_data));
}

static void present_main_window(
    ApplicationState *state,
    std::optional<std::uint32_t> activation_time) {
  if (state == nullptr || state->main_window == nullptr ||
      state->window_destroyed) {
    return;
  }
  state->hide_when_tray_available = false;
  gtk_widget_show_all(state->main_window->window);
  enable_connection_selection(state);
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
  if (state->tray_available && !state->quitting) {
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
  if (state->monitor_refresh_source != 0) {
    g_source_remove(state->monitor_refresh_source);
    state->monitor_refresh_source = 0;
  }
  if (state->connection_monitor != nullptr) {
    g_file_monitor_cancel(state->connection_monitor);
    g_object_unref(state->connection_monitor);
    state->connection_monitor = nullptr;
  }
  for (ChildLaunch *launch : state->child_launches) {
    launch->application = nullptr;
    if (!launch->temporary_startup_path.empty()) {
      g_remove(launch->temporary_startup_path.c_str());
      launch->temporary_startup_path.clear();
    }
  }
  close_global_defaults_dialog(state);
  if (state->settings_widget != nullptr) {
    elder_terms::destroy_settings_widget(state->settings_widget);
    state->settings_widget = nullptr;
  }
  if (!state->application_shutting_down &&
      (state->quitting || !state->tray_available)) {
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
  g_signal_connect(main_window->connection_list, "key-press-event",
                   G_CALLBACK(on_connection_list_key_press), state);
  g_signal_connect(main_window->window, "key-press-event",
                   G_CALLBACK(on_connection_list_key_press), state);
  g_signal_connect(main_window->new_button, "clicked",
                   G_CALLBACK(on_new_clicked), state);
  g_signal_connect(main_window->global_defaults_button, "clicked",
                   G_CALLBACK(on_global_defaults_clicked), state);
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
  state->sftp_executable = resolve_child_executable(
      state->launcher_argv0.c_str(), "ELDER_TERMS_SFTP_PATH",
      "elder-terms-sftp");
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
                                         bool available) {
  if (state->application_shutting_down ||
      state->window_destroyed) {
    return;
  }
  state->tray_available = available;
  if (available && state->hide_when_tray_available) {
    gtk_widget_hide(state->main_window->window);
  } else if (!available &&
             state->startup_mode == elder_terms::StartupMode::tray) {
    present_main_window(state);
  }
}

static void on_application_startup(GApplication *,
                                   gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  state->connection_directory =
      elder_terms::default_connection_directory();
  state->global_config_path =
      elder_terms::default_global_config_path();
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
  if (state->startup_mode == elder_terms::StartupMode::window) {
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
              .quit = [state]() {
                request_application_quit(state);
              },
              .availability_changed = [state](bool available) {
                on_tray_availability_changed(state, available);
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
  if (state->startup_mode == elder_terms::StartupMode::tray) {
    state->hide_when_tray_available = true;
    gtk_widget_show_all(state->main_window->window);
    enable_connection_selection(state);
    if (state->tray_available) {
      gtk_widget_hide(state->main_window->window);
    } else {
      gtk_window_present(GTK_WINDOW(state->main_window->window));
    }
    return;
  }
  present_main_window(state);
}

static void on_application_shutdown(GApplication *,
                                    gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  state->application_shutting_down = true;
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
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib dispatcher_group;
  cardio::dispatcher_host_glib_auto dispatcher(dispatcher_group);
  GApplication *application = g_application_new(
      elder_terms::launcher_application_id(),
      G_APPLICATION_DEFAULT_FLAGS);

  ApplicationState state;
  state.application = application;
  state.dispatcher = &dispatcher;
  state.dispatcher_group = &dispatcher_group;
  state.launcher_argv0 =
      argc > 0 && argv[0] != nullptr ? argv[0] : "elder-terms";
  g_signal_connect(application, "startup",
                   G_CALLBACK(on_application_startup), &state);
  g_signal_connect(application, "activate",
                   G_CALLBACK(on_application_activate), &state);
  g_signal_connect(application, "shutdown",
                   G_CALLBACK(on_application_shutdown), &state);

  const int result =
      g_application_run(application, argc, argv);
  if (!state.application_shutting_down) {
    dispatcher_group.shutdown();
  }
  if (state.main_window_storage.has_value()) {
    elder_terms::destroy_launcher_main_window(
        &state.main_window_storage.value());
  }
  g_object_unref(application);
  return state.startup_failed ? 1 : result;
}

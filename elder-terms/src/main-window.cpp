#include "main-window.h"

#include <filesystem>
#include <iostream>

#include <gestament/gtk.h>

namespace elder_terms {

static std::filesystem::path executable_directory() {
  return std::filesystem::read_symlink("/proc/self/exe").parent_path();
}

static GtkWidget *required_widget(GtkBuilder *builder, const char *id) {
  GObject *object = gtk_builder_get_object(builder, id);
  if (object == nullptr || !GTK_IS_WIDGET(object)) {
    std::cerr << "Missing GTK widget: " << id << '\n';
    return nullptr;
  }
  GtkWidget *widget = GTK_WIDGET(object);
  gestament_gtk_assign_accessible_id(widget, id);
  return widget;
}

static GObject *required_object(GtkBuilder *builder, const char *id,
                                GType type) {
  GObject *object = gtk_builder_get_object(builder, id);
  if (object == nullptr || !g_type_is_a(G_OBJECT_TYPE(object), type)) {
    std::cerr << "Missing GTK object: " << id << '\n';
    return nullptr;
  }
  return object;
}

std::optional<LauncherMainWindow> load_launcher_main_window() {
  GtkBuilder *builder = gtk_builder_new();
  gtk_builder_set_translation_domain(builder, "elder-terms");
  const std::filesystem::path path =
      executable_directory() / "main-window.ui";
  GError *error = nullptr;
  if (gtk_builder_add_from_file(builder, path.c_str(), &error) == 0) {
    std::cerr << "Failed to load launcher UI: " << path << ": "
              << (error == nullptr ? "unknown error" : error->message)
              << '\n';
    g_clear_error(&error);
    g_object_unref(builder);
    return std::nullopt;
  }

  LauncherMainWindow result{
      .builder = builder,
      .window = required_widget(builder, "main_window"),
      .split_pane = required_widget(builder, "split_pane"),
      .terminal_entries_label =
          required_widget(builder, "terminal_entries_label"),
      .connection_scroller =
          required_widget(builder, "connection_scroller"),
      .connection_list = required_widget(builder, "connection_list"),
      .connection_store = GTK_LIST_STORE(required_object(
          builder, "connection_store", GTK_TYPE_LIST_STORE)),
      .connection_name_renderer = GTK_CELL_RENDERER(required_object(
          builder, "connection_name_renderer", GTK_TYPE_CELL_RENDERER_TEXT)),
      .connection_context_menu =
          required_widget(builder, "connection_context_menu"),
      .rename_connection_menu_item =
          required_widget(builder, "rename_connection_menu_item"),
      .delete_connection_menu_item =
          required_widget(builder, "delete_connection_menu_item"),
      .details_stack = required_widget(builder, "details_stack"),
      .empty_details_label =
          required_widget(builder, "empty_details_label"),
      .settings_container = required_widget(builder, "settings_container"),
      .action_row = required_widget(builder, "action_row"),
      .new_button = required_widget(builder, "new_button"),
      .global_defaults_button =
          required_widget(builder, "global_defaults_button"),
      .apply_button = required_widget(builder, "apply_button"),
      .connect_button = required_widget(builder, "connect_button"),
  };
  if (result.window == nullptr || result.split_pane == nullptr ||
      result.terminal_entries_label == nullptr ||
      result.connection_scroller == nullptr ||
      result.connection_list == nullptr || result.connection_store == nullptr ||
      result.connection_name_renderer == nullptr ||
      result.connection_context_menu == nullptr ||
      result.rename_connection_menu_item == nullptr ||
      result.delete_connection_menu_item == nullptr ||
      result.details_stack == nullptr ||
      result.empty_details_label == nullptr ||
      result.settings_container == nullptr || result.action_row == nullptr ||
      result.new_button == nullptr ||
      result.global_defaults_button == nullptr ||
      result.apply_button == nullptr || result.connect_button == nullptr) {
    g_object_unref(builder);
    return std::nullopt;
  }
  return result;
}

void destroy_launcher_main_window(LauncherMainWindow *main_window) {
  if (main_window == nullptr || main_window->builder == nullptr) {
    return;
  }
  g_object_unref(main_window->builder);
  main_window->builder = nullptr;
}

} // namespace elder_terms

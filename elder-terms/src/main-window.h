#pragma once

#include <optional>

#include <gtk/gtk.h>

namespace elder_terms {

/**
 * Widgets loaded from the launcher UI definition.
 */
struct LauncherMainWindow {
  /** Builder owning UI objects. */
  GtkBuilder *builder = nullptr;
  /** Top-level launcher window. */
  GtkWidget *window = nullptr;
  /** Resizable connection/details split pane. */
  GtkWidget *split_pane = nullptr;
  /** Heading above the terminal connection list. */
  GtkWidget *terminal_entries_label = nullptr;
  /** Scroller containing the connection list. */
  GtkWidget *connection_scroller = nullptr;
  /** Connection tree view. */
  GtkWidget *connection_list = nullptr;
  /** Connection list model. */
  GtkListStore *connection_store = nullptr;
  /** Editable connection name renderer. */
  GtkCellRenderer *connection_name_renderer = nullptr;
  /** Context menu for a saved connection. */
  GtkWidget *connection_context_menu = nullptr;
  /** Starts in-place editing of a saved connection name. */
  GtkWidget *rename_connection_menu_item = nullptr;
  /** Duplicates a saved connection. */
  GtkWidget *duplicate_connection_menu_item = nullptr;
  /** Requests deletion of a saved connection. */
  GtkWidget *delete_connection_menu_item = nullptr;
  /** Stack switching between empty and settings state. */
  GtkWidget *details_stack = nullptr;
  /** Message shown when no connection is selected. */
  GtkWidget *empty_details_label = nullptr;
  /** Container receiving the shared settings widget. */
  GtkWidget *settings_container = nullptr;
  /** Bottom action row that can move the launcher window. */
  GtkWidget *action_row = nullptr;
  /** Creates a new connection draft. */
  GtkWidget *new_button = nullptr;
  /** Opens the global defaults editor. */
  GtkWidget *global_defaults_button = nullptr;
  /** Saves the selected connection draft. */
  GtkWidget *apply_button = nullptr;
  /** Launches the selected connection. */
  GtkWidget *connect_button = nullptr;
};

/**
 * Loads the launcher window from its executable-adjacent UI file.
 *
 * @returns Required widgets, or nullopt when the UI cannot be loaded.
 */
std::optional<LauncherMainWindow> load_launcher_main_window();

/**
 * Releases the builder owned by a launcher main window.
 *
 * @param main_window Window state to release.
 */
void destroy_launcher_main_window(LauncherMainWindow *main_window);

} // namespace elder_terms

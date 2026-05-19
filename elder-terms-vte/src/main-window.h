#pragma once

#include <optional>

#include <gtk/gtk.h>

namespace elder_terms {

/**
 * Holds the GTK builder and required widgets from main-window.ui.
 */
struct MainWindow {
  /** Builder that owns loaded UI objects. */
  GtkBuilder *builder = nullptr;
  /** Top-level application window. */
  GtkWidget *window = nullptr;
  /** Header bar widget. */
  GtkWidget *header_bar = nullptr;
  /** Button that opens the runtime settings dialog. */
  GtkWidget *settings_button = nullptr;
  /** Root container inside the window. */
  GtkWidget *root_box = nullptr;
  /** Scroller surrounding the terminal and scrollbar. */
  GtkWidget *terminal_scroller = nullptr;
  /** VTE terminal widget. */
  GtkWidget *terminal = nullptr;
  /** Scrollbar bound to the terminal vadjustment. */
  GtkWidget *terminal_scrollbar = nullptr;
  /** Status bar container. */
  GtkWidget *status_bar = nullptr;
  /** Status text label. */
  GtkWidget *status_label = nullptr;
  /** Status bar activity indicator container. */
  GtkWidget *activity_indicator_bar = nullptr;
  /** SD activity indicator container. */
  GtkWidget *sd_indicator_box = nullptr;
  /** SD activity indicator image. */
  GtkWidget *sd_indicator_image = nullptr;
  /** SD activity indicator label. */
  GtkWidget *sd_indicator_label = nullptr;
  /** RD activity indicator container. */
  GtkWidget *rd_indicator_box = nullptr;
  /** RD activity indicator image. */
  GtkWidget *rd_indicator_image = nullptr;
  /** RD activity indicator label. */
  GtkWidget *rd_indicator_label = nullptr;
};

/**
 * Loads the main GTK window from the executable-adjacent UI file.
 *
 * @returns Loaded window widgets, or std::nullopt after logging an error.
 */
std::optional<MainWindow> load_main_window();

/**
 * Releases GTK resources owned by a loaded MainWindow.
 *
 * @param main_window Window handle to release.
 */
void release_main_window(MainWindow *main_window);

} // namespace elder_terms

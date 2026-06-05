#pragma once

#include <array>
#include <optional>
#include <string>

#include <gtk/gtk.h>

#include <elder-terms/settings.h>

#include "activity-indicator.h"
#include "activity-indicator-id.h"

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
  /** Button that opens the file transfer menu. */
  GtkWidget *transfer_button = nullptr;
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
  /** Activity indicator containers. */
  std::array<GtkWidget *, activity_indicator_count()> indicator_boxes{};
  /** Activity indicator images. */
  std::array<GtkWidget *, activity_indicator_count()> indicator_images{};
  /** Activity indicator labels. */
  std::array<GtkWidget *, activity_indicator_count()> indicator_labels{};
  /** True when the indicator should accept activity events. */
  std::array<bool, activity_indicator_count()> indicator_visible{};
  /** Shared lit indicator pixbuf. */
  GdkPixbuf *indicator_on_icon = nullptr;
  /** Shared dark indicator pixbuf. */
  GdkPixbuf *indicator_off_icon = nullptr;
  /** Activity indicator runtime states. */
  std::array<ActivityIndicatorWidget, activity_indicator_count()> indicators{};
};

/**
 * Loads the main GTK window from the executable-adjacent UI file.
 *
 * @returns Loaded window widgets, or std::nullopt after logging an error.
 */
std::optional<MainWindow> load_main_window();

/**
 * Records activity against one status-bar activity indicator.
 *
 * @param main_window Main window containing the indicator widgets.
 * @param indicator Indicator slot to blink.
 */
void note_main_window_activity(MainWindow *main_window,
                               ActivityIndicatorId indicator);

/**
 * Sets one status-bar activity indicator to an explicit state.
 *
 * @param main_window Main window containing the indicator widgets.
 * @param indicator Indicator slot to update.
 * @param active True to show the lit icon.
 */
void set_main_window_indicator_state(MainWindow *main_window,
                                     ActivityIndicatorId indicator,
                                     bool active);

/**
 * Updates the terminal connected/read-only presentation.
 *
 * @param main_window Main window containing the VTE terminal.
 * @param connected True when the backend is connected.
 */
void set_main_window_connection_active(MainWindow *main_window,
                                       bool connected);

/**
 * Updates only the terminal interactive/read-only presentation.
 *
 * @param main_window Main window containing the VTE terminal.
 * @param interactive True when VTE input should be accepted and fully bright.
 */
void set_main_window_terminal_interactive(MainWindow *main_window,
                                          bool interactive);

/**
 * Updates transfer button visibility.
 *
 * @param main_window Main window containing the header bar.
 * @param visible True when transfer actions should be shown.
 */
void set_main_window_transfer_button_visible(MainWindow *main_window,
                                             bool visible);

/**
 * Updates transfer button sensitivity.
 *
 * @param main_window Main window containing the header bar.
 * @param sensitive True when transfer actions can be selected.
 */
void set_main_window_transfer_button_sensitive(MainWindow *main_window,
                                               bool sensitive);

/**
 * Updates the status-bar text.
 *
 * @param main_window Main window containing the status label.
 * @param text Status text.
 */
void set_main_window_status_text(MainWindow *main_window,
                                 const std::string &text);

/**
 * Updates the main window title.
 *
 * @param main_window Main window containing the window and header bar widgets.
 * @param title Window title to show.
 */
void set_main_window_title(MainWindow *main_window, const std::string &title);

/**
 * Updates which activity indicators are visible for a connection kind.
 *
 * @param main_window Main window containing the indicator widgets.
 * @param kind Selected terminal connection kind.
 */
void set_main_window_activity_indicator_connection_kind(
    MainWindow *main_window, TerminalConnectionKind kind);

/**
 * Stops activity indicators and clears their non-owning widget references.
 *
 * @param main_window Main window containing the indicator widgets.
 */
void deactivate_main_window_activity_indicators(MainWindow *main_window);

/**
 * Releases GTK resources owned by a loaded MainWindow.
 *
 * @param main_window Window handle to release.
 */
void release_main_window(MainWindow *main_window);

} // namespace elder_terms

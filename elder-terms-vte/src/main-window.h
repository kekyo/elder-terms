#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>

#include <gtk/gtk.h>

#include <elder-terms/settings.h>

#include "activity-indicator.h"
#include "activity-indicator-id.h"
#include "terminal-connection-phase.h"
#include "terminal-transfer.h"

namespace elder_terms {

/**
 * Handles terminal context-menu Paste availability and selected text.
 */
struct MainWindowTerminalPasteCallbacks {
  /** Returns whether the current application state can start a text send. */
  std::function<bool()> can_paste;
  /** Receives non-empty UTF-8 text selected from the clipboard. */
  std::function<void(std::string utf8_text)> paste;
};

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
  /** Overlay stacking disconnected status on top of the terminal. */
  GtkWidget *terminal_overlay = nullptr;
  /** VTE terminal widget. */
  GtkWidget *terminal = nullptr;
  /** Overlay used to dim the terminal without changing the VTE background. */
  GtkWidget *terminal_dim_overlay = nullptr;
  /** Inline disconnected notice shown on the terminal surface. */
  GtkWidget *disconnected_notice = nullptr;
  /** Background layer inside the inline disconnected notice. */
  GtkWidget *disconnected_notice_background = nullptr;
  /** Label inside the inline disconnected notice. */
  GtkWidget *disconnected_notice_label = nullptr;
  /** Inline transfer progress notice shown on the terminal surface. */
  GtkWidget *transfer_progress_notice = nullptr;
  /** Background layer inside the inline transfer progress notice. */
  GtkWidget *transfer_progress_notice_background = nullptr;
  /** Label inside the inline transfer progress notice. */
  GtkWidget *transfer_progress_notice_label = nullptr;
  /** Progress bar inside the inline transfer progress notice. */
  GtkWidget *transfer_progress_bar = nullptr;
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
  /** Active transfer progress pulse timeout, or 0 when not pulsing. */
  guint transfer_progress_pulse_source = 0;
  /** Title without transient connection-state suffixes. */
  std::string base_title = "elder-terms-vte";
  /** True while the backend connection is currently active. */
  bool connection_active = false;
  /** Current backend connection lifecycle phase. */
  TerminalSessionConnectionPhase connection_phase =
      TerminalSessionConnectionPhase::disconnected;
};

/**
 * Loads the main GTK window from the executable-adjacent UI file.
 *
 * @returns Loaded window widgets, or std::nullopt after logging an error.
 */
std::optional<MainWindow> load_main_window();

/**
 * Configures Paste handling for the terminal context menu.
 *
 * @param main_window Main window containing the terminal context menu.
 * @param callbacks Paste availability and text callbacks.
 */
void set_main_window_terminal_paste_callbacks(
    MainWindow *main_window, MainWindowTerminalPasteCallbacks callbacks);

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
 * Updates the terminal presentation for a backend connection phase.
 *
 * @param main_window Main window containing the VTE terminal.
 * @param phase Current backend connection phase.
 */
void set_main_window_connection_phase(MainWindow *main_window,
                                      TerminalSessionConnectionPhase phase);

/**
 * Updates only the terminal interactive/read-only presentation.
 *
 * @param main_window Main window containing the VTE terminal.
 * @param interactive True when VTE input should be accepted and fully bright.
 */
void set_main_window_terminal_interactive(MainWindow *main_window,
                                          bool interactive);

/**
 * Updates transfer progress notice visibility.
 *
 * @param main_window Main window containing the terminal overlay.
 * @param visible True when the transfer progress notice should be shown.
 */
void set_main_window_transfer_progress_visible(MainWindow *main_window,
                                               bool visible);

/**
 * Updates the transfer progress bar mode and value.
 *
 * @param main_window Main window containing the transfer progress bar.
 * @param progress Transfer progress presentation state.
 */
void set_main_window_transfer_progress(MainWindow *main_window,
                                       TerminalTransferProgress progress);

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
 * Controls whether blink activity indicators stay lit until reset.
 *
 * @param main_window Main window containing the indicator widgets.
 * @param latch True to latch RD and SD activity for integration tests.
 * @remarks Normal runtime behavior keeps this disabled.
 */
void set_main_window_activity_indicators_latched(MainWindow *main_window,
                                                 bool latch);

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

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <gtk/gtk.h>

#include <cardio.h>

#include <elder-terms/settings.h>

#include "activity-indicator.h"
#include "activity-indicator-id.h"
#include "terminal-connection-phase.h"
#include "terminal-sessions/ssh-session/ssh-user-prompt.h"
#include "terminal-transfer.h"

namespace elder_terms {

struct InlinePromptController;
struct MainWindowTransferProgressState;

/**
 * Requests cancellation of the currently visible transfer.
 *
 * @returns True when an active transfer accepted the cancellation request.
 */
using MainWindowTransferCancelCallback = std::function<bool()>;

/**
 * Handles terminal context-menu Paste availability and selected text.
 */
struct MainWindowTerminalPasteCallbacks {
  /** Returns whether the current application state can start a text send. */
  std::function<bool()> can_paste;
  /** Receives non-empty UTF-8 text selected from the clipboard. */
  std::function<void(std::string utf8_text)> paste;
};

/** Handles terminal context-menu BREAK availability and activation. */
struct MainWindowTerminalBreakCallbacks {
  /** Returns whether current application state can send BREAK. */
  std::function<bool()> can_send;
  /** Requests one terminal BREAK action. */
  std::function<void()> send;
};

/** Handles activation of an OSC 8 hyperlink from the terminal. */
struct MainWindowTerminalHyperlinkCallbacks {
  /**
   * Receives the raw OSC 8 target under a Ctrl+left click.
   *
   * @returns True when the target was accepted and the event was consumed.
   */
  std::function<bool(std::string target)> activate;
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
  /** Button that opens the file transfer menu. */
  GtkWidget *transfer_button = nullptr;
  /** Button that opens application-level commands. */
  GtkWidget *application_menu_button = nullptr;
  /** Menu item that opens the runtime settings dialog. */
  GtkWidget *settings_menu_item = nullptr;
  /** Requests the launcher-owned application information page. */
  GtkWidget *about_menu_item = nullptr;
  /** Root container inside the window. */
  GtkWidget *root_box = nullptr;
  /** Optional left border around the terminal window content. */
  GtkWidget *frame_start_border = nullptr;
  /** Optional right border around the terminal window content. */
  GtkWidget *frame_end_border = nullptr;
  /** Scroller surrounding the terminal and scrollbar. */
  GtkWidget *terminal_scroller = nullptr;
  /** Overlay stacking disconnected status on top of the terminal. */
  GtkWidget *terminal_overlay = nullptr;
  /** VTE terminal widget. */
  GtkWidget *terminal = nullptr;
  /** Overlay used to dim the terminal without changing the VTE background. */
  GtkWidget *terminal_dim_overlay = nullptr;
  /** Inline panel used for every SSH host-key and authentication prompt. */
  GtkWidget *ssh_prompt_panel = nullptr;
  /** Opaque background layer inside the SSH prompt panel. */
  GtkWidget *ssh_prompt_background = nullptr;
  /** Heading inside the SSH prompt panel. */
  GtkWidget *ssh_prompt_title_label = nullptr;
  /** Question text inside the SSH prompt panel. */
  GtkWidget *ssh_prompt_message_label = nullptr;
  /** Preformatted security context inside the SSH prompt panel. */
  GtkWidget *ssh_prompt_monospace_message_label = nullptr;
  /** Maskable response entry inside the SSH prompt panel. */
  GtkWidget *ssh_prompt_entry = nullptr;
  /** Button that rejects the active SSH prompt. */
  GtkWidget *ssh_prompt_cancel_button = nullptr;
  /** Button that resets a changed per-user SSH host key. */
  GtkWidget *ssh_prompt_alternative_button = nullptr;
  /** Button that accepts the active SSH prompt. */
  GtkWidget *ssh_prompt_accept_button = nullptr;
  /** Inline disconnected notice shown on the terminal surface. */
  GtkWidget *disconnected_notice = nullptr;
  /** Background layer inside the inline disconnected notice. */
  GtkWidget *disconnected_notice_background = nullptr;
  /** Label inside the inline disconnected notice. */
  GtkWidget *disconnected_notice_label = nullptr;
  /** Overlay container for transfer progress and cancellation controls. */
  GtkWidget *transfer_progress_overlay = nullptr;
  /** Inline transfer progress notice shown on the terminal surface. */
  GtkWidget *transfer_progress_notice = nullptr;
  /** Background layer inside the inline transfer progress notice. */
  GtkWidget *transfer_progress_notice_background = nullptr;
  /** Label inside the inline transfer progress notice. */
  GtkWidget *transfer_progress_notice_label = nullptr;
  /** Progress bar inside the inline transfer progress notice. */
  GtkWidget *transfer_progress_bar = nullptr;
  /** Button that requests cancellation of the active transfer. */
  GtkWidget *transfer_cancel_button = nullptr;
  /** Scrollbar bound to the terminal vadjustment. */
  GtkWidget *terminal_scrollbar = nullptr;
  /** Status bar container. */
  GtkWidget *status_bar = nullptr;
  /** Status text label. */
  GtkWidget *status_label = nullptr;
  /** Hidden fixture-only terminal grid-size label. */
  GtkWidget *fixture_grid_size_label = nullptr;
  /** Hidden fixture-only VTE scrollback-size label. */
  GtkWidget *fixture_scrollback_lines_label = nullptr;
  /** Status bar activity indicator container. */
  GtkWidget *activity_indicator_bar = nullptr;
  /** Open runtime settings dialog receiving connection backgrounds. */
  GtkWidget *settings_dialog = nullptr;
  /** Root of the open settings widget receiving connection backgrounds. */
  GtkWidget *settings_widget_root = nullptr;
  /** Provider applying the configured header-bar and status-bar background. */
  GtkCssProvider *exterior_background_provider = nullptr;
  /** Screen provider scoped to Settings title and selection controls. */
  GtkCssProvider *settings_exterior_background_provider = nullptr;
  /** Screen provider applying exterior-derived colors below exterior
   * surfaces. */
  GtkCssProvider *exterior_component_background_provider = nullptr;
  /** Screen provider scoped to Settings content surfaces. */
  GtkCssProvider *settings_background_provider = nullptr;
  /** Provider applying the configured background to terminal overlay panels. */
  GtkCssProvider *overlay_background_provider = nullptr;
  /** Provider applying background-derived colors to content controls. */
  GtkCssProvider *component_background_provider = nullptr;
  /** Screen provider applying background-derived colors to popups. */
  GtkCssProvider *popup_component_background_provider = nullptr;
  /** True after overriding VTE's default background color. */
  bool terminal_background_overridden = false;
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
  /** True when the VTE may accept input and own keyboard focus. */
  bool terminal_interactive = false;
  /** Current backend connection lifecycle phase. */
  TerminalSessionConnectionPhase connection_phase =
      TerminalSessionConnectionPhase::disconnected;
  /** Shared inline controller for SSH prompt signals and pending responses. */
  std::shared_ptr<InlinePromptController> ssh_prompt;
  /** Stable controller for transfer progress actions. */
  std::shared_ptr<MainWindowTransferProgressState> transfer_progress_state;
};

/**
 * Loads the main GTK window from the executable-adjacent UI file.
 *
 * @returns Loaded window widgets, or std::nullopt after logging an error.
 */
std::optional<MainWindow> load_main_window();

/**
 * Applies optional connection colors to the window and VTE.
 *
 * @param main_window Main window containing the exterior and terminal widgets.
 * @param settings Effective optional RGB colors.
 */
void set_main_window_colors(MainWindow *main_window,
                            const GeneralColorSettings &settings);

/**
 * Registers the open runtime settings dialog for connection color updates.
 *
 * @param main_window Main window owning the connection color providers.
 * @param dialog Open GtkDialog, or null after it is destroyed.
 * @param settings_root Root settings widget, or null after it is destroyed.
 */
void set_main_window_settings_dialog(
    MainWindow *main_window, GtkWidget *dialog,
    GtkWidget *settings_root);

/**
 * Configures Paste handling for the terminal context menu.
 *
 * @param main_window Main window containing the terminal context menu.
 * @param callbacks Paste availability and text callbacks.
 */
void set_main_window_terminal_paste_callbacks(
    MainWindow *main_window, MainWindowTerminalPasteCallbacks callbacks);

/**
 * Configures BREAK handling for the terminal context menu.
 *
 * @param main_window Main window containing the terminal context menu.
 * @param callbacks BREAK availability and activation callbacks.
 */
void set_main_window_terminal_break_callbacks(
    MainWindow *main_window, MainWindowTerminalBreakCallbacks callbacks);

/**
 * Configures Ctrl+left-click OSC 8 hyperlink activation.
 *
 * @param main_window Main window containing the VTE terminal.
 * @param callbacks Hyperlink activation callback.
 */
void set_main_window_terminal_hyperlink_callbacks(
    MainWindow *main_window, MainWindowTerminalHyperlinkCallbacks callbacks);

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
 * Sets the failure reason retained by the disconnected terminal overlay.
 *
 * @param main_window Main window containing the disconnected notice.
 * @param message Backend failure reason.
 */
void set_main_window_connection_failure(MainWindow *main_window,
                                        const std::string &message);

/**
 * Updates only the terminal interactive/read-only presentation.
 *
 * @param main_window Main window containing the VTE terminal.
 * @param interactive True when VTE input should be accepted and fully bright.
 */
void set_main_window_terminal_interactive(MainWindow *main_window,
                                          bool interactive);

/**
 * Gives keyboard focus to VTE when its current presentation is interactive.
 *
 * @param main_window Main window containing the VTE terminal.
 * @remarks Hidden, unmapped, and read-only terminal states are left unchanged.
 */
void focus_main_window_terminal_if_interactive(MainWindow *main_window);

/**
 * Displays one SSH question as an overlay on the dimmed VTE surface.
 *
 * @param main_window Main window containing the terminal overlay.
 * @param prompt Host-key or authentication question to display.
 * @param cancellation Cancellation signal owned by the SSH session.
 * @returns User response, or a rejected response after cancellation.
 */
cardio::promise<SshUserPromptResponse> prompt_main_window_ssh_async(
    MainWindow *main_window, const SshUserPrompt &prompt,
    cardio::cancellation cancellation);

/**
 * Rejects and hides any active SSH prompt.
 *
 * @param main_window Main window containing the terminal overlay.
 */
void cancel_main_window_ssh_prompt(MainWindow *main_window);

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
 * Configures the action invoked by the transfer progress Cancel button.
 *
 * @param main_window Main window containing the transfer progress overlay.
 * @param callback Callback that requests cancellation of the active transfer.
 */
void set_main_window_transfer_cancel_callback(
    MainWindow *main_window, MainWindowTransferCancelCallback callback);

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

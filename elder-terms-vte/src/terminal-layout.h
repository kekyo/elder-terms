#pragma once

#include <functional>

#include <elder-terms/settings.h>

#include "launch-options.h"
#include "main-window.h"

namespace elder_terms {

/**
 * Opaque state for VTE cell-based window sizing and test fixture rendering.
 */
struct TerminalLayoutState;

/**
 * Called when the VTE grid size is initialized or changed.
 *
 * @param columns Current VTE column count.
 * @param rows Current VTE row count.
 */
using TerminalGridSizeChangedCallback =
    std::function<void(glong columns, glong rows)>;

/**
 * Called when the live terminal display settings change.
 *
 * @param settings Current terminal display settings.
 */
using TerminalDisplaySettingsChangedCallback =
    std::function<void(TerminalDisplaySettings settings)>;

/** Called when the configured terminal BREAK shortcut is pressed. */
using TerminalBreakRequestedCallback = std::function<void()>;

/**
 * Optional callbacks emitted by terminal layout state.
 */
struct TerminalLayoutCallbacks {
  /** Receives VTE grid size changes. */
  TerminalGridSizeChangedCallback grid_size_changed;
  /** Receives VTE grid size or font scale changes. */
  TerminalDisplaySettingsChangedCallback display_settings_changed;
  /** Receives one non-repeated BREAK shortcut press. */
  TerminalBreakRequestedCallback break_requested;
};

/**
 * Creates terminal layout state and binds static terminal UI relationships.
 *
 * @param main_window Loaded main window widgets.
 * @param options Test harness options.
 * @param terminal_display_settings Initial terminal display settings.
 * @param show_border Whether to show the terminal window side borders.
 * @param border_width Width of each terminal window side border in pixels.
 * @param terminal_font_families Initial terminal font family overrides.
 * @param terminal_key_bindings Initial terminal action key bindings.
 * @param callbacks Optional layout callbacks.
 * @returns New layout state owned by the caller.
 */
TerminalLayoutState *
create_terminal_layout(const MainWindow &main_window, TestOptions options,
                       TerminalDisplaySettings terminal_display_settings,
                       bool show_border, gint border_width,
                       TerminalFontFamilies terminal_font_families,
                       TerminalKeyBindings terminal_key_bindings,
                       TerminalLayoutCallbacks callbacks);

/**
 * Connects GTK and VTE signals required for cell-based layout updates.
 *
 * @param state Layout state created by create_terminal_layout.
 */
void connect_terminal_layout_signals(TerminalLayoutState *state);

/**
 * Queues the initial layout update and fixture feed when enabled.
 *
 * @param state Layout state created by create_terminal_layout.
 */
void start_terminal_layout(TerminalLayoutState *state);

/**
 * Applies terminal display settings to the live layout.
 *
 * @param state Layout state created by create_terminal_layout.
 * @param terminal_display_settings New terminal display settings.
 */
void apply_terminal_display_settings(
    TerminalLayoutState *state,
    TerminalDisplaySettings terminal_display_settings);

/**
 * Applies terminal window side-border visibility to the live layout.
 *
 * @param state Layout state created by create_terminal_layout.
 * @param show_border Whether to show the left and right frame borders.
 */
void apply_terminal_border_visibility(TerminalLayoutState *state,
                                      bool show_border);

/**
 * Applies terminal window side-border width to the live layout.
 *
 * @param state Layout state created by create_terminal_layout.
 * @param border_width Width of each frame border in pixels.
 */
void apply_terminal_border_width(TerminalLayoutState *state,
                                 gint border_width);

/**
 * Applies terminal font family overrides to the live layout.
 *
 * The size and all other font attributes continue to come from the runtime
 * font captured when the layout was created.
 *
 * @param state Layout state created by create_terminal_layout.
 * @param terminal_font_families New terminal font family overrides.
 */
void apply_terminal_font_families(
    TerminalLayoutState *state,
    const TerminalFontFamilies &terminal_font_families);

/**
 * Applies terminal action key bindings to the live layout.
 *
 * @param state Layout state created by create_terminal_layout.
 * @param terminal_key_bindings New terminal action key bindings.
 */
void apply_terminal_key_bindings(
    TerminalLayoutState *state,
    TerminalKeyBindings terminal_key_bindings);

/**
 * Releases pending layout timers and deletes layout state.
 *
 * @param state Layout state to destroy.
 */
void destroy_terminal_layout(TerminalLayoutState *state);

} // namespace elder_terms

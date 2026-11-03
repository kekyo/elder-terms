// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include "main-window.h"
#include "../../shared/src/connection-runtime/launch-options.h"
#include "terminal-background-image.h"

namespace elder_terms {

/** Application preferences and shortcuts for one boxrig attachment. */
struct TerminalPresentation;

/**
 * Applies the application's opaque gray selection and black selected text.
 * @param view Attached view on its owning UI thread.
 * @return The boxrig status; a failed change preserves the previous style.
 * @remarks Used by the application and its driver after every attachment.
 * Existing color filters and hidden-text attributes still apply.
 */
inline boxrig_ui_status apply_terminal_selection_style(boxrig_ui_view *view) {
  auto selection = boxrig_ui_selection_style_default();
  selection.presentation = BOXRIG_UI_SELECTION_FILL;
  selection.background = {179, 179, 179};
  selection.background_opacity = 255;
  selection.foreground_policy = BOXRIG_UI_SELECTION_FOREGROUND_RGB;
  selection.foreground = {0, 0, 0};
  return boxrig_ui_view_set_selection_style(view, &selection);
}

/**
 * Creates presentation integration without owning the runtime or GTK objects.
 * @param window Stable window state, retained until destruction.
 * @param options Optional deterministic UI fixture controls.
 * @param changed Receives committed grid, history capacity and DIP size.
 * @param send_break Requests the configured BREAK action.
 * @return Owned integration, released after runtime shutdown and before GTK ownership.
 */
TerminalPresentation *create_terminal_presentation(MainWindow *window, TestOptions options,
    std::function<void(TerminalDisplaySettings)> changed, std::function<void()> send_break);

/**
 * Attaches shortcuts and restores cursor, selection, background and color-filter preferences after widget reattachment.
 * @param presentation Live integration.
 */
void attach_terminal_presentation(TerminalPresentation *presentation);

/**
 * Routes Control+wheel steps to the native visual zoom sequence.
 * @param presentation Live integration.
 * @param event Normalized mouse event supplied by the native widget.
 * @return Application ownership for a zoom step, otherwise terminal routing.
 */
boxrig_input_mouse_route route_terminal_presentation_mouse(
    TerminalPresentation *presentation, const boxrig_mouse_event &event);

/**
 * Applies cursor, background, color filtering, font, history, grid, chrome and shortcuts.
 * @param presentation Live integration.
 * @param settings Effective application settings.
 * @param image Prepared image, or null to clear it.
 * @return Whether the native presentation, font, history and grid preferences were accepted.
 */
bool apply_terminal_presentation(TerminalPresentation *presentation, const SettingsStore &settings, TerminalBackgroundImagePtr image);

/** @param presentation Live integration. @returns Retained image for rollback. */
TerminalBackgroundImagePtr terminal_presentation_background_image(const TerminalPresentation *presentation);
/** Prepares an image without changing the displayed terminal.
 * @param presentation Live integration; destruction cancels the request.
 * @param settings Requested preferences.
 * @param current Whether this request remains current.
 * @param complete Receives the image or error on the UI thread, possibly synchronously. */
void prepare_terminal_presentation_background(TerminalPresentation *presentation, const SettingsStore &settings,
    std::function<bool()> current, std::function<void(TerminalBackgroundImagePtr, std::string)> complete);
/** @param presentation Live integration or null. Cancels pending image preparation. */
void cancel_terminal_presentation_background(TerminalPresentation *presentation);

/**
 * Observes native frame completion and publishes committed display settings.
 * @param presentation Live integration, called by the runtime change hook.
 */
void refresh_terminal_presentation(TerminalPresentation *presentation);

/** @param presentation Live integration or null. @returns Actual background capabilities and fallback status. */
std::string terminal_presentation_background_status(const TerminalPresentation *presentation);

/**
 * Releases presentation sources and callbacks before GTK ownership is released.
 * @param presentation Owned integration, or nullptr.
 */
void destroy_terminal_presentation(TerminalPresentation *presentation);

}

#pragma once

#include <functional>

#include <gtk/gtk.h>

#include <elder-terms/settings.h>

namespace elder_terms {

/**
 * Opaque state for an embeddable settings editor widget.
 */
struct SettingsWidgetState;

/**
 * Called when the user applies settings from the widget.
 *
 * @param store Draft settings after applying the current widget values.
 */
using SettingsWidgetApplyCallback =
    std::function<void(const SettingsStore &store)>;

/**
 * Called when the user cancels settings editing.
 */
using SettingsWidgetCancelCallback = std::function<void()>;

/**
 * Called when the user saves settings from the widget.
 *
 * @param store Draft settings after applying the current widget values.
 * @returns True when the settings were saved and should become applied.
 */
using SettingsWidgetSaveCallback =
    std::function<bool(const SettingsStore &store)>;

/**
 * Optional callbacks emitted by the settings widget.
 */
struct SettingsWidgetCallbacks {
  /** Receives applied draft settings. */
  SettingsWidgetApplyCallback apply;
  /** Receives save requests for draft settings. */
  SettingsWidgetSaveCallback save;
  /** Receives cancel requests. */
  SettingsWidgetCancelCallback cancel;
};

/**
 * Options used to create a settings editor widget.
 */
struct SettingsWidgetOptions {
  /** Initial store copied into the widget draft state. */
  SettingsStore store;
  /** True when editing settings for an already-running session. */
  bool is_runtime = false;
  /** Optional callbacks emitted by the widget. */
  SettingsWidgetCallbacks callbacks;
};

/**
 * Creates an embeddable settings editor widget.
 *
 * @param options Initial settings and callback behavior.
 * @returns New settings widget state owned by the caller.
 */
SettingsWidgetState *create_settings_widget(SettingsWidgetOptions options);

/**
 * Replaces the widget state with externally updated settings.
 *
 * @param state Settings widget state.
 * @param store Updated settings store copied into applied and draft state.
 *
 * @remarks This is intended for runtime editors whose backing application
 * state can change while the editor is open.
 */
void update_settings_widget_store(SettingsWidgetState *state,
                                  SettingsStore store);

/**
 * Returns the root GTK widget for insertion into a container.
 *
 * @param state Settings widget state.
 * @returns Root GtkBox widget, or nullptr when state is null.
 */
GtkWidget *settings_widget_root(SettingsWidgetState *state);

/**
 * Releases settings widget state.
 *
 * @param state Settings widget state to destroy.
 *
 * @remarks When the root widget has been inserted into a GTK container, the
 * container owns the GTK widget lifetime. Destroy that container before
 * releasing the state, or release the state from the container destroy handler.
 */
void destroy_settings_widget(SettingsWidgetState *state);

} // namespace elder_terms

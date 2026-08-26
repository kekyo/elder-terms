#pragma once

#include <functional>
#include <string>

#include <gtk/gtk.h>

#include <elder-terms/export.h>
#include <elder-terms/settings.h>

namespace elder_terms {

/** Opaque state for the application-level settings editor. */
struct ApplicationSettingsWidgetState;

/** Called after the application settings draft or its validity changes. */
using ApplicationSettingsWidgetChangedCallback = std::function<void()>;

/** Options used to create an application-level settings editor. */
struct ApplicationSettingsWidgetOptions {
  /** Initial global settings store copied into the widget draft. */
  SettingsStore store;
  /** Prefix used to form stable accessible widget identifiers. */
  std::string id_prefix = "application_settings";
  /** Optional callback invoked after an editable value changes. */
  ApplicationSettingsWidgetChangedCallback changed;
};

/**
 * Creates an editor for display language, startup mode, and application
 * shortcut settings.
 *
 * @param options Initial settings and callback behavior.
 * @returns New widget state owned by the caller.
 */
ELDER_TERMS_API ApplicationSettingsWidgetState *
create_application_settings_widget(ApplicationSettingsWidgetOptions options);

/**
 * Returns a copy of the current application settings draft.
 *
 * @param state Application settings widget state.
 * @returns Current draft, or an empty store when state is null.
 */
ELDER_TERMS_API SettingsStore application_settings_widget_draft_store(
    const ApplicationSettingsWidgetState *state);

/**
 * Checks whether the application settings draft contains an unsaved edit.
 *
 * @param state Application settings widget state.
 * @returns True when a setting changed or an invalid input remains.
 */
ELDER_TERMS_API bool application_settings_widget_is_dirty(
    const ApplicationSettingsWidgetState *state);

/**
 * Checks whether all application settings inputs are valid.
 *
 * @param state Application settings widget state.
 * @returns True when the current draft can be saved.
 */
ELDER_TERMS_API bool application_settings_widget_is_valid(
    const ApplicationSettingsWidgetState *state);

/**
 * Returns the root GTK widget for insertion into a container.
 *
 * @param state Application settings widget state.
 * @returns Root GtkGrid widget, or null when state is null.
 */
ELDER_TERMS_API GtkWidget *application_settings_widget_root(
    ApplicationSettingsWidgetState *state);

/**
 * Releases application settings widget state.
 *
 * @param state Widget state to destroy.
 */
ELDER_TERMS_API void destroy_application_settings_widget(
    ApplicationSettingsWidgetState *state);

} // namespace elder_terms

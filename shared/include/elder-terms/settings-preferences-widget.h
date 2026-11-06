// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <functional>
#include <gtk/gtk.h>
#include <elder-terms/settings.h>

namespace elder_terms {

/** Application, connection-default and named-theme editor pages. */
struct SettingsPreferencesWidget;

/** Inputs owned by a preferences editor. */
struct SettingsPreferencesOptions {
  /** Saved file baselines. */
  SettingsEditSession session;
  /** Saved terminal connection names available for New Window. */
  std::vector<std::string> new_window_connections;
  /** Returns saved and unsaved connection names using a theme ID. */
  std::function<std::vector<std::string>(const std::string &)> references;
  /** Called after draft or validity changes. */
  std::function<void()> changed;
};

/** @param options Editor inputs. @returns Owned editor state. */
ELDER_TERMS_API SettingsPreferencesWidget *create_settings_preferences_widget(SettingsPreferencesOptions options);
/** @param state Live editor. @returns Notebook containing the three edit pages. */
ELDER_TERMS_API GtkWidget *settings_preferences_widget_root(SettingsPreferencesWidget *state);
/** @param state Live editor. @returns Shared save status label for placement below the notebook. */
ELDER_TERMS_API GtkWidget *settings_preferences_widget_status(SettingsPreferencesWidget *state);
/** @param state Live editor. @returns Whether every page can be saved. */
ELDER_TERMS_API bool settings_preferences_widget_is_valid(const SettingsPreferencesWidget *state);
/** @param state Live editor. @returns Whether unsaved edits remain in either file. */
ELDER_TERMS_API bool settings_preferences_widget_is_dirty(const SettingsPreferencesWidget *state);
/** @param state Live editor. @returns Independent save results; failed drafts remain editable. */
ELDER_TERMS_API SettingsEditResult settings_preferences_widget_save(SettingsPreferencesWidget *state);
/** @param state Live editor. @returns Global settings actually saved, excluding pending edits. */
ELDER_TERMS_API SettingsStore settings_preferences_widget_saved_global(const SettingsPreferencesWidget *state);
/** @param state Live editor. @returns Saved global and theme inputs for refreshing connection inheritance. */
ELDER_TERMS_API std::shared_ptr<const SettingsThemeContext> settings_preferences_widget_saved_context(const SettingsPreferencesWidget *state);
/** @param state Editor to release after its GTK container is destroyed. */
ELDER_TERMS_API void destroy_settings_preferences_widget(SettingsPreferencesWidget *state);

}

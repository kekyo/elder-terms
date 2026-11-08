// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <functional>

#include <gtk/gtk.h>

#include <elder-terms/export.h>
#include <elder-terms/ip-scanner.h>
#include <elder-terms/settings.h>

namespace elder_terms {

/**
 * Opaque state for an embeddable settings editor widget.
 */
struct SettingsWidgetState;

/**
 * Selects the settings editor presentation.
 */
enum class SettingsWidgetMode {
  /** Edits one connection while exposing inherited defaults. */
  connection,
  /** Edits global defaults without connection-specific fields. */
  global_defaults,
  /** Edits only inheritable settings in a named theme. */
  named_theme,
};

/** One pending Apply/Save request. Call its functions only on the GTK thread. */
struct SettingsWidgetAction {
  /** True until completion, a later edit, cancellation, or editor destruction.
   * Check before committing asynchronous work; the check remains safe after destruction. */
  std::function<bool()> current;
  /** Completes once. Accepted requests become applied; failures retain the draft
   * and show the supplied message. Stale requests have no effect. */
  std::function<void(bool accepted, std::string message)> complete;
};

/**
 * Requests settings application. The editor remains pending until completion.
 * @param store Copied draft; asynchronous consumers must retain their own copy.
 * @param action Safe request handle; check current before changing application state.
 */
using SettingsWidgetApplyCallback =
    std::function<void(const SettingsStore &store, SettingsWidgetAction action)>;

/**
 * Called when the user cancels settings editing.
 */
using SettingsWidgetCancelCallback = std::function<void()>;

/**
 * Called after the settings draft or its validation state changes.
 */
using SettingsWidgetChangedCallback = std::function<void()>;

/**
 * Creates scanner dependencies when an IP scan dialog is opened.
 *
 * @returns Scanner dependencies for one dialog invocation.
 */
using SettingsWidgetIpScannerDependenciesFactory =
    std::function<IpScannerDependencies()>;

/**
 * Called when the user saves settings from the widget.
 *
 * @param store Draft settings after applying the current widget values.
 * @param action Complete as accepted only after saving and applying both succeed.
 */
using SettingsWidgetSaveCallback =
    SettingsWidgetApplyCallback;

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
  /** Receives draft and validation state changes. */
  SettingsWidgetChangedCallback changed;
};

/**
 * Options used to create a settings editor widget.
 */
struct SettingsWidgetOptions {
  /** Initial store copied into the widget draft state. */
  SettingsStore store;
  /** True when editing settings for an already-running session. */
  bool is_runtime = false;
  /** True when the widget should render its built-in action buttons. */
  bool show_actions = true;
  /** Settings editor presentation. */
  SettingsWidgetMode mode = SettingsWidgetMode::connection;
  /** Prefix used to form stable accessible widget identifiers. */
  std::string id_prefix = "settings";
  /** Optional callbacks emitted by the widget. */
  SettingsWidgetCallbacks callbacks;
  /**
   * Optional scanner dependency factory.
   *
   * @remarks When empty, the widget scans the host machine's IPv4 interfaces.
   */
  SettingsWidgetIpScannerDependenciesFactory ip_scanner_dependencies_factory = {};
};

/**
 * Creates an embeddable settings editor widget.
 *
 * @param options Initial settings and callback behavior.
 * @returns New settings widget state owned by the caller.
 */
ELDER_TERMS_API SettingsWidgetState *
create_settings_widget(SettingsWidgetOptions options);

/**
 * Replaces the widget state with externally updated settings.
 *
 * @param state Settings widget state.
 * @param store Updated settings store copied into applied and draft state.
 *
 * @remarks This is intended for runtime editors whose backing application
 * state can change while the editor is open.
 */
ELDER_TERMS_API void update_settings_widget_store(SettingsWidgetState *state,
                                                   SettingsStore store);

/**
 * Replaces inherited values while preserving connection overrides.
 *
 * @param state Settings widget state.
 * @param fallbacks Updated global defaults store.
 *
 * @remarks Applied and draft stores are both rebased. Explicit values,
 * dirtiness, and the connection-specific General name remain unchanged.
 */
ELDER_TERMS_API void
settings_widget_rebase_fallbacks(SettingsWidgetState *state,
                                 const SettingsStore &fallbacks);

/**
 * Updates the path-derived connection name used by an unset General name.
 *
 * @param state Settings widget state.
 * @param default_connection_name New fallback connection name.
 *
 * @remarks Explicit General name edits are preserved.
 */
ELDER_TERMS_API void settings_widget_set_default_connection_name(
    SettingsWidgetState *state, std::string default_connection_name);

/**
 * Returns a copy of the current settings draft.
 *
 * @param state Settings widget state.
 * @returns Current draft, or an empty store when state is null.
 */
ELDER_TERMS_API SettingsStore
settings_widget_draft_store(const SettingsWidgetState *state);

/**
 * Checks whether the current draft contains a user edit.
 *
 * @param state Settings widget state.
 * @returns True when at least one draft setting is dirty or an invalid raw
 * input has not yet been resolved.
 */
ELDER_TERMS_API bool
settings_widget_is_dirty(const SettingsWidgetState *state);

/**
 * Checks whether all editor inputs can be applied.
 *
 * @param state Settings widget state.
 * @returns True when the current inputs are valid.
 */
ELDER_TERMS_API bool
settings_widget_is_valid(const SettingsWidgetState *state);

/**
 * Shows the General page when displaying settings for a new selection.
 *
 * @param state Widget state, or nullptr for no operation.
 */
ELDER_TERMS_API void settings_widget_show_general_page(SettingsWidgetState *state);

/**
 * Returns the root GTK widget for insertion into a container.
 *
 * @param state Settings widget state.
 * @returns Root GtkBox widget, or nullptr when state is null.
 */
ELDER_TERMS_API GtkWidget *settings_widget_root(SettingsWidgetState *state);

/**
 * Sets read-only information at the right of a runtime editor's action row.
 * @param state Live settings state, or null for no operation.
 * @param text Short status; an empty string hides the label.
 * @param tooltip Complete status and diagnostics for truncated text.
 * @remarks Non-runtime editors do not show this label. Draft values, dirty
 * state and callbacks remain unchanged. Text is copied by GTK.
 */
ELDER_TERMS_API void settings_widget_set_runtime_status(SettingsWidgetState *state,
    const std::string &text, const std::string &tooltip);

/** @param state Live editor or null. @param text Effective background status, without changing the draft. */
ELDER_TERMS_API void settings_widget_set_background_status(SettingsWidgetState *state, const std::string &text);

/**
 * Releases settings widget state.
 *
 * @param state Settings widget state to destroy.
 *
 * @remarks When the root widget has been inserted into a GTK container, the
 * container owns the GTK widget lifetime. Destroy that container before
 * releasing the state, or release the state from the container destroy handler.
 */
ELDER_TERMS_API void destroy_settings_widget(SettingsWidgetState *state);

} // namespace elder_terms

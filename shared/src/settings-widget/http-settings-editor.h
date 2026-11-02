// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <functional>
#include <gtk/gtk.h>
#include <elder-terms/settings.h>

namespace elder_terms {
/** Opaque state owned by the surrounding settings widget. */
struct HttpSettingsEditor;
/**
 * Creates the connection editor with inherited-value controls.
 * @param store Stable address of the parent draft store.
 * @param prefix Accessible ID prefix.
 * @param read_only Whether connection controls are read-only at runtime.
 * @param changed Parent validation and dirty-state callback.
 * @returns Editor state, released using destroy_http_settings_editor().
 */
HttpSettingsEditor *create_http_settings_editor(SettingsStore *store,
    std::string prefix, bool read_only, std::function<void()> changed);
/** @param editor Editor state. @returns Root GTK widget owned by its parent. */
GtkWidget *http_settings_editor_root(HttpSettingsEditor *editor);
/**
 * Synchronizes controls after draft or inherited settings change.
 * @param editor Editor to synchronize.
 * @param preserve_invalid True when rebasing fallbacks should retain unfinished edits.
 */
void sync_http_settings_editor(HttpSettingsEditor *editor, bool preserve_invalid);
/** @param editor Editor state. @returns Whether all visible and inherited values are valid. */
bool http_settings_editor_is_valid(const HttpSettingsEditor *editor);
/** @param editor Editor whose parent widgets have been destroyed. */
void destroy_http_settings_editor(HttpSettingsEditor *editor);
} // namespace elder_terms

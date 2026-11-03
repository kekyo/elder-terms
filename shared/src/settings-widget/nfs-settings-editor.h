// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once
#include <functional>
#include <gtk/gtk.h>
#include <elder-terms/settings.h>

namespace elder_terms {

/** NFS controls owned by the surrounding settings widget. */
struct NfsSettingsEditor;

/**
 * Creates independently inherited NFS controls.
 * @param store Stable address of the parent's draft settings.
 * @param prefix Accessible widget ID prefix.
 * @param runtime Locks endpoint fields while allowing encoding edits for reconnection.
 * @param changed Parent validation and dirty-state callback.
 * @returns Editor to release with destroy_nfs_settings_editor().
 */
NfsSettingsEditor *create_nfs_settings_editor(SettingsStore *store, std::string prefix,
    bool runtime, std::function<void()> changed);

/** @param editor Editor state. @returns Root GTK widget. */
GtkWidget *nfs_settings_editor_root(NfsSettingsEditor *editor);

/**
 * Refreshes controls from effective settings.
 * @param editor Editor state.
 * @param preserve_invalid Preserve unfinished input during inheritance updates.
 */
void sync_nfs_settings_editor(NfsSettingsEditor *editor, bool preserve_invalid);

/** @param editor Editor state. @returns Whether all NFS controls are valid. */
bool nfs_settings_editor_is_valid(const NfsSettingsEditor *editor);

/** @param editor Editor state to release. */
void destroy_nfs_settings_editor(NfsSettingsEditor *editor);

} // namespace elder_terms

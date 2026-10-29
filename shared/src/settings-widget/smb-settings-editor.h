// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <functional>
#include <gtk/gtk.h>
#include <elder-terms/settings.h>

namespace elder_terms {

/** Opaque SMB editor owned by the surrounding settings widget. */
struct SmbSettingsEditor;

/**
 * Creates SMB controls with independently inherited values.
 * @param store Stable address of the parent draft store.
 * @param prefix Accessible ID prefix.
 * @param read_only Whether controls are read-only at runtime.
 * @param changed Parent validation and dirty-state callback.
 * @returns Editor state, released using destroy_smb_settings_editor().
 */
SmbSettingsEditor *create_smb_settings_editor(SettingsStore *store,
    std::string prefix, bool read_only, std::function<void()> changed);

/** @param editor Editor state. @returns Root GTK widget. */
GtkWidget *smb_settings_editor_root(SmbSettingsEditor *editor);

/**
 * Refreshes controls from the effective store.
 * @param editor Editor state.
 * @param preserve_invalid Preserve unfinished text while rebasing defaults.
 */
void sync_smb_settings_editor(SmbSettingsEditor *editor, bool preserve_invalid);

/** @param editor Editor state. @returns Whether all active SMB inputs are valid. */
bool smb_settings_editor_is_valid(const SmbSettingsEditor *editor);

/** @param editor Editor state to release. */
void destroy_smb_settings_editor(SmbSettingsEditor *editor);

} // namespace elder_terms

#pragma once

#include <functional>
#include <gtk/gtk.h>
#include <elder-terms/settings.h>

namespace elder_terms {
/** Opaque state owned by the surrounding settings widget. */
struct WebdavSettingsEditor;
/**
 * Creates the connection editor with inherited-value controls.
 * @param store Stable address of the parent draft store.
 * @param prefix Accessible ID prefix.
 * @param read_only Whether connection controls are read-only at runtime.
 * @param changed Parent validation and dirty-state callback.
 * @returns Editor state, released using destroy_webdav_settings_editor().
 */
WebdavSettingsEditor *create_webdav_settings_editor(SettingsStore *store,
    std::string prefix, bool read_only, std::function<void()> changed);
/** @param editor Editor state. @returns Root GTK widget owned by its parent. */
GtkWidget *webdav_settings_editor_root(WebdavSettingsEditor *editor);
/**
 * Synchronizes controls after draft or inherited settings change.
 * @param editor Editor to synchronize.
 * @param preserve_invalid True when rebasing fallbacks should retain unfinished edits.
 */
void sync_webdav_settings_editor(WebdavSettingsEditor *editor, bool preserve_invalid);
/** @param editor Editor state. @returns Whether all visible and inherited values are valid. */
bool webdav_settings_editor_is_valid(const WebdavSettingsEditor *editor);
/** @param editor Editor whose parent widgets have been destroyed. */
void destroy_webdav_settings_editor(WebdavSettingsEditor *editor);
} // namespace elder_terms

#pragma once

#include <functional>
#include <gtk/gtk.h>
#include <elder-terms/settings.h>

namespace elder_terms {
/** Editor owned by the surrounding settings widget. */
struct SshProxySettingsEditor;
/**
 * Creates the gateway editor.
 * @param store Stable parent draft store.
 * @param prefix Accessible ID prefix.
 * @param read_only Whether connection settings are immutable at runtime.
 * @param global Whether this editor edits defaults for all connection types.
 * @param changed Parent validation/dirty callback.
 * @returns Editor state; destroy after its parent widgets.
 */
SshProxySettingsEditor *create_ssh_proxy_settings_editor(SettingsStore *store,
    std::string prefix, bool read_only, bool global, std::function<void()> changed);
/** @param editor Editor state. @returns Root widget owned by its GTK parent. */
GtkWidget *ssh_proxy_settings_editor_root(SshProxySettingsEditor *editor);
/** @param editor Editor state. @param preserve_invalid Preserve unfinished edits on rebase. */
void sync_ssh_proxy_settings_editor(SshProxySettingsEditor *editor, bool preserve_invalid);
/** @param editor Editor state. @returns Whether the current route can be applied. */
bool ssh_proxy_settings_editor_is_valid(const SshProxySettingsEditor *editor);
/** @param editor State whose parent widgets have been destroyed. */
void destroy_ssh_proxy_settings_editor(SshProxySettingsEditor *editor);
} // namespace elder_terms

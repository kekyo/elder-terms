#pragma once

#include <gtk/gtk.h>

#include <memory>

#include <elder-terms/settings.h>

#include "../terminal-session.h"

namespace elder_terms {

/**
 * Creates a direct libssh terminal session backend.
 *
 * @param terminal VTE terminal widget.
 * @param settings Effective SSH connection settings.
 * @param text_settings Effective terminal text conversion settings.
 * @param callbacks Optional session callbacks.
 * @returns New SSH session backend.
 */
std::unique_ptr<TerminalSession>
create_terminal_ssh_session(GtkWidget *terminal,
                            SshConnectionSettings settings,
                            TerminalTextSettings text_settings,
                            TerminalSessionCallbacks callbacks);

} // namespace elder_terms

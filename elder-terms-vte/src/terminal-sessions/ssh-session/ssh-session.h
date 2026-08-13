#pragma once

#include <gtk/gtk.h>

#include <memory>

#include <elder-terms/settings.h>

#include "../terminal-session.h"
#include "ssh-channel-connection.h"

namespace elder_terms {

/**
 * Creates a direct libssh terminal session backend.
 *
 * @param terminal VTE terminal widget.
 * @param settings Effective SSH connection settings.
 * @param text_settings Effective terminal text conversion settings.
 * @param callbacks Optional session callbacks.
 * @param connection_options Low-level SSH connection options.
 * @returns New SSH session backend.
 */
std::unique_ptr<TerminalSession>
create_terminal_ssh_session(GtkWidget *terminal,
                            SshConnectionSettings settings,
                            TerminalTextSettings text_settings,
                            TerminalSessionCallbacks callbacks,
                            SshChannelConnectionOptions connection_options);

} // namespace elder_terms

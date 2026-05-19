#pragma once

#include <gtk/gtk.h>

#include <memory>

#include <elder-terms/settings/local-session-settings.h>

#include "../terminal-session.h"

namespace elder_terms {

/**
 * Creates a local shell backend session.
 *
 * @param terminal VTE terminal widget receiving the local shell.
 * @param settings Local shell backend settings.
 * @param callbacks Optional callbacks emitted by the session.
 * @returns New local shell backend session.
 */
std::unique_ptr<TerminalSession>
create_terminal_local_shell_session(GtkWidget *terminal,
                                    LocalShellConnectionSettings settings,
                                    TerminalSessionCallbacks callbacks);

} // namespace elder_terms

#pragma once

#include <gtk/gtk.h>

#include <memory>

#include <elder-terms/settings/telnet-settings.h>

#include "../terminal-session.h"

namespace elder_terms {

/**
 * Creates a TELNET backend session.
 *
 * @param terminal VTE terminal widget receiving the TELNET stream.
 * @param settings TELNET backend settings.
 * @param callbacks Optional callbacks emitted by the session.
 * @returns New TELNET backend session.
 */
std::unique_ptr<TerminalSession>
create_terminal_telnet_session(GtkWidget *terminal,
                               TelnetConnectionSettings settings,
                               TerminalSessionCallbacks callbacks);

} // namespace elder_terms

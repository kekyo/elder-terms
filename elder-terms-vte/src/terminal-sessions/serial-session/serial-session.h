#pragma once

#include <gtk/gtk.h>

#include <memory>

#include <elder-terms/settings/serial-settings.h>

#include "../terminal-session.h"

namespace elder_terms {

/**
 * Creates a serial backend session.
 *
 * @param terminal VTE terminal widget receiving the serial stream.
 * @param settings Serial backend settings.
 * @param text_settings Effective text conversion and special-code settings.
 * @param callbacks Optional callbacks emitted by the session.
 * @returns New serial backend session.
 */
std::unique_ptr<TerminalSession>
create_terminal_serial_session(GtkWidget *terminal,
                               SerialConnectionSettings settings,
                               TerminalTextSettings text_settings,
                               TerminalSessionCallbacks callbacks);

} // namespace elder_terms

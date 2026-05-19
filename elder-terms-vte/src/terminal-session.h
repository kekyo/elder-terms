#pragma once

#include <gtk/gtk.h>

#include <elder-terms/settings.h>

#include "terminal-session-callbacks.h"

namespace elder_terms {

/**
 * Opaque state for one terminal connection session.
 */
struct TerminalSessionState;

/**
 * Creates a terminal session from a connection profile.
 *
 * @param terminal VTE terminal widget used by the session.
 * @param profile Selected connection profile.
 * @param callbacks Optional callbacks emitted by the session.
 * @returns New terminal session state owned by the caller.
 */
TerminalSessionState *
create_terminal_session(GtkWidget *terminal, TerminalConnectionProfile profile,
                        TerminalSessionCallbacks callbacks);

/**
 * Starts the configured terminal session backend.
 *
 * @param state Session state created by create_terminal_session.
 * @returns True when the backend start request was accepted.
 */
bool start_terminal_session(TerminalSessionState *state);

/**
 * Requests the configured terminal session backend to stop.
 *
 * @param state Session state created by create_terminal_session.
 */
void stop_terminal_session(TerminalSessionState *state);

/**
 * Notifies the session backend that the VTE grid size changed.
 *
 * @param state Session state created by create_terminal_session.
 * @param columns Current VTE column count.
 * @param rows Current VTE row count.
 */
void resize_terminal_session(TerminalSessionState *state, glong columns,
                             glong rows);

/**
 * Applies runtime-editable connection settings to the current backend.
 *
 * @param state Session state created by create_terminal_session.
 * @param profile Updated connection profile.
 */
void apply_terminal_session_connection_profile(
    TerminalSessionState *state, const TerminalConnectionProfile &profile);

/**
 * Releases backend resources and deletes the session state.
 *
 * @param state Session state to destroy.
 */
void destroy_terminal_session(TerminalSessionState *state);

} // namespace elder_terms

#pragma once

#include <gtk/gtk.h>

#include <string>

#include <elder-terms/settings.h>

#include "terminal-session-callbacks.h"
#include "terminal-transfer.h"

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
 * Returns whether the active backend supports X/Y/ZMODEM transfers.
 *
 * @param state Session state created by create_terminal_session.
 * @returns True when transfers can be requested.
 */
bool terminal_session_supports_transfer(const TerminalSessionState *state);

/**
 * Returns whether the active backend is transferring a file.
 *
 * @param state Session state created by create_terminal_session.
 * @returns True while a transfer is active.
 */
bool terminal_session_transfer_in_progress(const TerminalSessionState *state);

/**
 * Starts one X/Y/ZMODEM transfer on the active backend.
 *
 * @param state Session state created by create_terminal_session.
 * @param request Transfer request.
 * @returns True when the request was accepted.
 */
bool start_terminal_session_transfer(TerminalSessionState *state,
                                     TerminalTransferRequest request);

/**
 * Returns the title for a terminal session state.
 *
 * @param state Session state created by create_terminal_session.
 * @returns Current backend title, or the application title when no backend
 * session exists.
 */
std::string terminal_session_title(const TerminalSessionState *state);

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

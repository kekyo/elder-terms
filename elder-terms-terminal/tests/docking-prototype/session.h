#pragma once
#include <gtk/gtk.h>

typedef struct PilotSession PilotSession;

/**
 * Creates a session backed by a local shell.
 * @param label Session letter printed by the fixture shell.
 * @return Owned session. Release it with pilot_session_close().
 */
PilotSession *pilot_session_open(char label);
/**
 * @param session Live pilot session.
 * @return Borrowed terminal widget retained across dock and undock.
 */
GtkWidget *pilot_session_widget(PilotSession *session);
/**
 * Stops I/O, terminates the fixture shell and releases the session.
 * @param session Session detached from its GtkStack.
 */
void pilot_session_close(PilotSession *session);

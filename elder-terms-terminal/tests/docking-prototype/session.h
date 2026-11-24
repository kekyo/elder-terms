#pragma once
#include <gtk/gtk.h>

typedef struct PilotSession PilotSession;

/** Notifies the tab owner that its PTY has closed. */
typedef void (*PilotShellEnded)(gpointer data);

/**
 * Creates a session backed by a local shell.
 * @param label Session letter used in diagnostic output.
 * @param fixture Use the numbered-output shell fixture for gesture tests.
 * @param ended Called on the GTK thread when the PTY closes.
 * @param data Tab passed to ended.
 * @return Owned session. Release it with pilot_session_close().
 */
PilotSession *pilot_session_open(char label, gboolean fixture, PilotShellEnded ended, gpointer data);
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

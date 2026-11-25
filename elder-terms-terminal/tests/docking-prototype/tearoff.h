#pragma once

#include <gtk/gtk.h>

/** One tab's native pointer gesture, owned by its button. */
typedef struct TearoffGesture TearoffGesture;

/**
 * Moves the button's tab into a new window after an outside release.
 * @param context Callback context supplied to tearoff_gesture_bind().
 */
typedef void (*TearoffUndock)(void *context);

/**
 * Binds a tab button to a native pointer gesture.
 * @param button Existing tab button.
 * @param undock Called after the pointer is released outside that toplevel.
 * @param context Passed to undock when the tab is released outside the window.
 * @return Gesture owned by button. Do not free the returned pointer.
 */
TearoffGesture *tearoff_gesture_bind(
    GtkWidget *button, TearoffUndock undock, void *context);

/**
 * Enables the native pointer gesture when the tab has no GTK dock peer.
 * @param gesture Bound gesture.
 * @param enabled Whether to handle pointer and Escape events.
 */
void tearoff_gesture_set_enabled(TearoffGesture *gesture, gboolean enabled);

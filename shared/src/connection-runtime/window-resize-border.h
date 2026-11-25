// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <gtk/gtk.h>

namespace elder_terms {

/** Starts the visible header-border resize before a child handles its own drag gesture.
 * @param window Owning top-level window with installed resize borders.
 * @param button Native button event delivered to the header or one of its children.
 * @return True when the window manager received a resize request.
 */
bool begin_window_header_resize(GtkWidget *window, GdkEventButton *button);

/**
 * Makes visible connection-window borders act as native resize controls.
 * @param window Borrowed owning window.
 * @param start Borrowed event box at the logical start of the content.
 * @param end Borrowed event box at the logical end of the content.
 * @param bottom Borrowed event box below the content in compact mode.
 * @remarks Visibility changes restore the theme's ordinary resize margins when
 * borders are hidden. Call again when a different page supplies the window's borders.
 * Geometry and scale are read from GTK at each event.
 */
void install_window_resize_borders(GtkWindow *window, GtkWidget *start,
                                   GtkWidget *end, GtkWidget *bottom);

} // namespace elder_terms

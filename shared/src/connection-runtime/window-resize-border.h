// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <gtk/gtk.h>

namespace elder_terms {

/**
 * Makes visible connection-window borders act as native resize controls.
 * @param window Borrowed owning window.
 * @param start Borrowed event box at the logical start of the content.
 * @param end Borrowed event box at the logical end of the content.
 * @param bottom Borrowed event box below the content in compact mode.
 * @remarks Visibility changes restore the theme's ordinary resize margins when
 * borders are hidden. Geometry and scale are read from GTK at each event.
 */
void install_window_resize_borders(GtkWindow *window, GtkWidget *start,
                                   GtkWidget *end, GtkWidget *bottom);

} // namespace elder_terms

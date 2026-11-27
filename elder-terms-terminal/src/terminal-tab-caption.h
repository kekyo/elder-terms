// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

#pragma once
#include <gtk/gtk.h>

namespace elder_terms {

/** Creates a single-line tab label that scrolls on changes to an overflowing title.
 * @param title Source label containing the session's complete current title.
 * @returns A floating GtkLabel with GTK's normal ellipsis when idle.
 */
GtkWidget *create_terminal_tab_caption(GtkWidget *title);

}

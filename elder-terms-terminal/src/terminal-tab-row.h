// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

#pragma once
#include "main-window.h"

namespace elder_terms {

/** Widgets used for tab selection, dragging and closing. */
struct TerminalTabRow {
  GtkWidget *row;   /**< List row owning the buttons. */
  GtkWidget *pick;  /**< Selection button and drag source. */
  GtkWidget *close; /**< Close button shown while the row is hovered. */
};

/** @param page Session supplying the current title, colors and icon.
 * @returns A floating list row with its two child buttons.
 */
TerminalTabRow create_terminal_tab_row(MainWindow &page);
/** @param page Session whose effective exterior color has changed. */
void update_terminal_tab_colors(MainWindow &page);
}

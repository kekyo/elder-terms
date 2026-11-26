// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

#pragma once
#include "main-window.h"

namespace elder_terms {

/** A live terminal page. Its connection belongs to the application. */
struct TerminalTab;
/** UI callbacks invoked on the GTK dispatcher. */
struct TerminalTabActions {
  /** Rebinds the page to its containing window and full content box. */
  std::function<void(GtkWidget *, GtkWidget *, bool)> host;
  /** Closes this session including its connection. */
  std::function<void()> close;
  /** Reports whether this page can currently leave its tray. */
  std::function<bool()> movable;
  /** Reports whether this page is ready for a cross-process handoff. */
  std::function<bool()> transferable;
  /** Matches the configured undock accelerator. */
  std::function<bool(guint, GdkModifierType)> undock_key;
  /** Publishes a drag offer for the ordered pages and selected page. */
  std::function<std::string(const std::vector<TerminalTab *> &, TerminalTab *)> offer;
  /** Receives a peer offer on this page's tray. */
  std::function<void(std::string, std::function<void(bool)>)> receive;
};
/** Installs a single page before the builder window is first shown. */
TerminalTab *create_terminal_tab(MainWindow *window, std::string label, TerminalTabActions actions);
/** Removes the tab after its session is stopped or ownership is transferred. */
void destroy_terminal_tab(TerminalTab *tab);
/** Selects the page and focuses its terminal. */
void select_terminal_tab(TerminalTab *tab);
/** Moves an ordered group and selects active at the destination. */
bool dock_terminal_tabs(const std::vector<TerminalTab *> &pages, TerminalTab *active, TerminalTab *destination);
/** Splits a page into its own window when it shares a tray. */
bool undock_terminal_tab(TerminalTab *tab);
/** Updates the native tear-off gesture when other processes gain or lose windows. */
void set_terminal_tab_external_peers(bool available);
/** Returns whether a native drop can currently be received at the addressed tray. */
bool can_receive_terminal_window_drop(guint32 xid);
/** Starts receiving a native drop if its addressed tray still exists and is available. */
bool receive_terminal_window_drop(guint32 xid, const std::string &token);
}

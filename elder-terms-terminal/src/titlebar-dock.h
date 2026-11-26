// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
#pragma once
#include <gtk/gtk.h>
#include <functional>
#include <string>

namespace elder_terms {
/** Observes native titlebar moves on supported displays. Calls drop with the
 * destination bus name and native window ID on a primary-button drop.
 * Returns false when the header needs the explicit GTK drag handle. */
bool install_titlebar_dock(GtkWidget *window, const char *bus_name,
    std::function<bool()> has_peer, std::function<void(const std::string &, guint32)> drop);
/** Returns the mapped window's X11 ID, or zero on other displays. */
guint32 titlebar_dock_window_id(GtkWidget *window);
}

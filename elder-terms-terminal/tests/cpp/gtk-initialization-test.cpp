// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/gtk-initialization.h"
#include <gtk/gtk.h>
#include <cstdio>
#include <string_view>

int main(int argc, char **argv) {
  const bool explicit_setting = argc > 1 && std::string_view(argv[1]) == "override";
  if (explicit_setting) {
    if (!g_setenv("GDK_RENDERING", "similar", TRUE)) return 1;
  } else {
    g_unsetenv("GDK_RENDERING");
  }
  elder_terms::initialize_terminal_gtk(&argc, &argv);
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_widget_realize(window);
  auto *surface = gdk_window_create_similar_surface(gtk_widget_get_window(window),
      CAIRO_CONTENT_COLOR_ALPHA, 64, 64);
  const auto expected = explicit_setting ? CAIRO_SURFACE_TYPE_XLIB : CAIRO_SURFACE_TYPE_IMAGE;
  const bool passed = surface && cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS &&
      cairo_surface_get_type(surface) == expected;
  if (!passed) std::fprintf(stderr, "GTK backing images must avoid X11 synchronization by default and respect overrides\n");
  cairo_surface_destroy(surface);
  gtk_widget_destroy(window);
  return passed ? 0 : 1;
}

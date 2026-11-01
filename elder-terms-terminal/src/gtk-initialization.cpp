// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "gtk-initialization.h"
#include <gtk/gtk.h>

namespace elder_terms {

void initialize_terminal_gtk(int *argc, char ***argv) {
  // GTK 3 otherwise composites Xlib backing pixmaps into the GL window through
  // glXWaitX, blocking input on X11. Image backing surfaces avoid that round
  // trip while GtkGLArea keeps rendering the terminal with OpenGL.
  // https://docs.gtk.org/gtk3/running.html#environment-variables
  // Set this before GTK starts threads, and preserve explicit user overrides.
  if (!g_setenv("GDK_RENDERING", "image", FALSE))
    g_error("Could not configure GTK backing images");
  gtk_init(argc, argv);
}

}

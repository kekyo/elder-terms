// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <stdio.h>
#include <string.h>

// Observe the resize direction GTK sends to the window manager before moving
// the pointer. The window manager handles this request asynchronously.
int main(int argc, char **argv) {
  Display *display = XOpenDisplay(NULL);
  if (!display) return 1;
  if (argc == 1) {
    XFixesCursorImage *cursor = XFixesGetCursorImage(display);
    puts(cursor && cursor->name ? cursor->name : "");
    if (cursor) XFree(cursor);
    XCloseDisplay(display);
    return 0;
  }
  if (argc != 2 || strcmp(argv[1], "resize") != 0) return 1;
  const Atom resize = XInternAtom(display, "_NET_WM_MOVERESIZE", False);
  XSelectInput(display, DefaultRootWindow(display), SubstructureNotifyMask);
  XSync(display, False);
  setvbuf(stdout, NULL, _IOLBF, 0);
  puts("ready");
  for (;;) {
    XEvent event;
    XNextEvent(display, &event);
    if (event.type == ClientMessage && event.xclient.message_type == resize)
      printf("%ld\n", event.xclient.data.l[2]);
  }
}

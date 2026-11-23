// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <stdio.h>

int main(void) {
  Display *display = XOpenDisplay(NULL);
  if (!display) return 1;
  XFixesCursorImage *cursor = XFixesGetCursorImage(display);
  puts(cursor && cursor->name ? cursor->name : "");
  if (cursor) XFree(cursor);
  XCloseDisplay(display);
  return 0;
}

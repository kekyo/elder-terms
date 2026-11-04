// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/titlebar-effects.h"
#include <cassert>
#include <cstdio>

using namespace elder_terms;

static void painted(GtkWidget *widget) {
  bool done = false;
  auto *clock = gtk_widget_get_frame_clock(widget);
  const auto id = g_signal_connect_after(clock, "after-paint", G_CALLBACK(+[](GdkFrameClock *, gpointer data) {
    *static_cast<bool *>(data) = true;
  }), &done);
  gtk_widget_queue_draw(widget);
  while (!done) g_main_context_iteration(nullptr, TRUE);
  g_signal_handler_disconnect(clock, id);
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *header = gtk_header_bar_new();
  gtk_window_set_titlebar(GTK_WINDOW(window), header);
  gtk_container_add(GTK_CONTAINER(window), gtk_label_new("Terminal body"));
  gtk_window_set_default_size(GTK_WINDOW(window), 400, 200);
  install_titlebar_effects(header);
  install_titlebar_effects(header);
  gtk_widget_show_all(window);
  painted(header);
  const auto height = gtk_widget_get_allocated_height(header);
  assert(!titlebar_effects_running(header));
  gtk_widget_set_state_flags(header, GTK_STATE_FLAG_BACKDROP, FALSE);
  notify_titlebar_bell(header);
  assert(titlebar_effects_running(header));
  painted(header);
  assert(gtk_widget_get_allocated_height(header) == height);
  while (titlebar_effects_running(header)) g_main_context_iteration(nullptr, TRUE);
  assert(gtk_widget_get_allocated_height(header) == height);

  notify_titlebar_title(header, "Initial");
  assert(!titlebar_effects_running(header));
  notify_titlebar_title(header, "Changed");
  assert(titlebar_effects_running(header));
  gtk_widget_hide(window);
  assert(!titlebar_effects_running(header));
  notify_titlebar_bell(header);
  notify_titlebar_title(header, "Hidden change");
  assert(!titlebar_effects_running(header));
  gtk_widget_show(window);
  painted(header);
  notify_titlebar_title(header, "Hidden change");
  assert(!titlebar_effects_running(header));
  notify_titlebar_title(header, "Shown change");
  assert(titlebar_effects_running(header));
  g_object_set(gtk_widget_get_settings(header), "gtk-enable-animations", FALSE, nullptr);
  assert(!titlebar_effects_running(header));
  notify_titlebar_bell(header);
  notify_titlebar_title(header, "Reduced motion");
  assert(!titlebar_effects_running(header));
  g_object_set(gtk_widget_get_settings(header), "gtk-enable-animations", TRUE, nullptr);
  assert(!titlebar_effects_running(header));
  notify_titlebar_bell(header);
  assert(titlebar_effects_running(header));
  set_titlebar_effects_enabled(header, false, true);
  assert(!titlebar_effects_running(header));
  notify_titlebar_title(header, "Title still enabled");
  assert(titlebar_effects_running(header));
  set_titlebar_effects_enabled(header, true, false);
  assert(!titlebar_effects_running(header));
  notify_titlebar_bell(header);
  assert(titlebar_effects_running(header));
  g_object_ref(header);
  gtk_widget_destroy(window);
  assert(!titlebar_effects_running(header));
  notify_titlebar_bell(header);
  notify_titlebar_title(header, "Destroyed");
  assert(!titlebar_effects_running(header));
  g_object_unref(header);
  std::puts("PASS backdrop, bounded frames, hidden updates, remap, reduced motion, independent preferences and destruction");
}

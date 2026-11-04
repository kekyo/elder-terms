// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../../shared/src/connection-runtime/titlebar-effects.h"
#include <cassert>
#include <cstdio>

using namespace elder_terms;

static bool reject_background_probe = false;
extern "C" cairo_surface_t *__real_cairo_image_surface_create(cairo_format_t, int, int);
extern "C" cairo_surface_t *__wrap_cairo_image_surface_create(cairo_format_t format, int width, int height) {
  if (reject_background_probe && width == 1 && height == 1) {
    reject_background_probe = false;
    return __real_cairo_image_surface_create(format, -1, -1);
  }
  return __real_cairo_image_surface_create(format, width, height);
}

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
  auto *other = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *other_header = gtk_header_bar_new();
  gtk_window_set_titlebar(GTK_WINDOW(other), other_header);
  install_titlebar_effects(other_header);
  gtk_widget_show_all(other);
  gtk_widget_show_all(window);
  painted(header);
  const auto height = gtk_widget_get_allocated_height(header);
  assert(!titlebar_effects_running(header));
  gtk_widget_set_state_flags(header, GTK_STATE_FLAG_BACKDROP, FALSE);
  notify_titlebar_bell(header);
  assert(titlebar_effects_running(header));
  assert(!titlebar_effects_running(other_header));
  painted(header);
  assert(gtk_widget_get_allocated_height(header) == height);
  while (titlebar_effects_running(header)) g_main_context_iteration(nullptr, TRUE);
  assert(gtk_widget_get_allocated_height(header) == height);

  if (argc > 1) {
    notify_titlebar_receive(header);
    gtk_window_iconify(GTK_WINDOW(window));
    while (!(gdk_window_get_state(gtk_widget_get_window(window)) & GDK_WINDOW_STATE_ICONIFIED))
      g_main_context_iteration(nullptr, TRUE);
    assert(!titlebar_effects_running(header));
    notify_titlebar_bell(header);
    notify_titlebar_receive(header);
    assert(!titlebar_effects_running(header));
    gtk_window_deiconify(GTK_WINDOW(window));
    while (!gtk_widget_get_mapped(header) ||
        (gdk_window_get_state(gtk_widget_get_window(window)) & GDK_WINDOW_STATE_ICONIFIED))
      g_main_context_iteration(nullptr, TRUE);
    painted(header);
    assert(!titlebar_effects_running(header));
  }

  // Installing an idle header must not start any animation.
  assert(!titlebar_effects_running(other_header));
  set_titlebar_effect_strengths(header, TitlebarEffectStrength::medium, TitlebarEffectStrength::disabled);
  set_titlebar_effect_strengths(header, TitlebarEffectStrength::medium, TitlebarEffectStrength::medium);
  notify_titlebar_receive(header);
  assert(titlebar_effects_running(header));
  reject_background_probe = true;
  while (reject_background_probe) g_main_context_iteration(nullptr, TRUE);
  gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Title after rendering failure");
  notify_titlebar_receive(header);
  assert(std::string(gtk_header_bar_get_title(GTK_HEADER_BAR(header))) == "Title after rendering failure");
  gtk_widget_hide(window);
  assert(!titlebar_effects_running(header));
  notify_titlebar_bell(header);
  notify_titlebar_receive(header);
  assert(!titlebar_effects_running(header));
  gtk_widget_show(window);
  painted(header);
  assert(!titlebar_effects_running(header));
  notify_titlebar_receive(header);
  assert(titlebar_effects_running(header));
  g_object_set(gtk_widget_get_settings(header), "gtk-application-prefer-dark-theme", TRUE, nullptr);
  painted(header);
  assert(titlebar_effects_running(header) && !titlebar_effects_running(other_header));
  g_object_set(gtk_widget_get_settings(header), "gtk-application-prefer-dark-theme", FALSE, nullptr);
  gtk_window_resize(GTK_WINDOW(window), 500, 250);
  painted(header);
  g_object_set(gtk_widget_get_settings(header), "gtk-enable-animations", FALSE, nullptr);
  assert(!titlebar_effects_running(header));
  notify_titlebar_bell(header);
  notify_titlebar_receive(header);
  assert(!titlebar_effects_running(header));
  g_object_set(gtk_widget_get_settings(header), "gtk-enable-animations", TRUE, nullptr);
  assert(!titlebar_effects_running(header));
  notify_titlebar_bell(header);
  assert(titlebar_effects_running(header));
  set_titlebar_effect_strengths(header, TitlebarEffectStrength::disabled, TitlebarEffectStrength::medium);
  assert(!titlebar_effects_running(header));
  notify_titlebar_receive(header);
  assert(titlebar_effects_running(header));
  set_titlebar_effect_strengths(header, TitlebarEffectStrength::medium, TitlebarEffectStrength::disabled);
  assert(!titlebar_effects_running(header));
  notify_titlebar_bell(header);
  assert(titlebar_effects_running(header));
  g_object_ref(header);
  gtk_widget_destroy(window);
  assert(!titlebar_effects_running(header));
  notify_titlebar_bell(header);
  notify_titlebar_receive(header);
  assert(!titlebar_effects_running(header));
  g_object_unref(header);
  gtk_widget_destroy(other);
  std::puts("PASS backdrop, bounded frames, hidden updates, remap, reduced motion, independent preferences and destruction");
}

// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

#include "../../src/terminal-tabs.h"
#include "../../src/terminal-runtime.h"
#include <gdk/gdkx.h>
#include <cassert>
#include <cstdio>

using namespace elder_terms;

static GtkWidget *descendant(GtkWidget *widget, GType type) {
  if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type)) return widget;
  if (!GTK_IS_CONTAINER(widget)) return nullptr;
  auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
  GtkWidget *found = nullptr;
  for (auto *child = children; child && !found; child = child->next)
    found = descendant(GTK_WIDGET(child->data), type);
  g_list_free(children);
  return found;
}

static void paint(GtkWidget *widget) {
  auto *clock = gtk_widget_get_frame_clock(widget);
  bool done = false;
  auto signal = g_signal_connect_after(clock, "after-paint", G_CALLBACK(+[](GdkFrameClock *, gpointer data) {
    *static_cast<bool *>(data) = true;
  }), &done);
  gtk_widget_queue_draw(widget);
  while (!done) g_main_context_iteration(nullptr, TRUE);
  g_signal_handler_disconnect(clock, signal);
}

static GdkPoint window_point(GtkWidget *widget, int x, int y) {
  GdkPoint point{};
  assert(gtk_widget_translate_coordinates(widget, gtk_widget_get_toplevel(widget), x, y, &point.x, &point.y));
  return point;
}

static void point_at(GtkWidget *widget, int x, int y) {
  const auto point = window_point(widget, x, y);
  auto *native = gtk_widget_get_window(gtk_widget_get_toplevel(widget));
  auto *display = GDK_WINDOW_XDISPLAY(native);
  XWarpPointer(display, None, GDK_WINDOW_XID(native), 0, 0, 0, 0, point.x, point.y);
  XSync(display, False);
  paint(widget);
}

static unsigned pixel(GtkWidget *widget, int x, int y) {
  const auto point = window_point(widget, x, y);
  auto *native = gtk_widget_get_window(gtk_widget_get_toplevel(widget));
  auto *image = gdk_pixbuf_get_from_window(native, point.x, point.y, 1, 1);
  assert(image);
  const auto *rgb = gdk_pixbuf_read_pixels(image);
  const unsigned color = (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
  g_object_unref(image);
  return color;
}

static TerminalTab *tab_for(MainWindow &page, const char *name) {
  set_main_window_connection_phase(&page, TerminalSessionConnectionPhase::connected);
  set_main_window_title(&page, name);
  TerminalTabActions actions{};
  actions.movable = [] { return true; };
  actions.transferable = actions.movable;
  actions.host = [&page](GtkWidget *window, GtkWidget *body, bool selected) {
    page.window = window;
    page.selected = selected;
    terminal_runtime_set_window(page.runtime, GTK_WINDOW(window), GTK_BOX(body), selected);
  };
  return create_terminal_tab(&page, true, std::move(actions));
}

extern "C" int __wrap_main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto first = load_main_window(TerminalAmbiguousWidth::narrow);
  auto second = load_main_window(TerminalAmbiguousWidth::narrow);
  assert(first && second);
  set_main_window_colors(&*first, {RgbColor{0x7a, 0x22, 0x22}, {}});
  set_main_window_colors(&*second, {RgbColor{0x22, 0x44, 0x8a}, {}});
  auto *a = tab_for(*first, "First");
  auto *b = tab_for(*second, "Second");
  assert(dock_terminal_tabs({b}, b, a));
  gtk_window_set_default_size(GTK_WINDOW(first->window), 720, 460);
  gtk_widget_show_all(first->window);
  auto *list = GTK_LIST_BOX(descendant(first->window, GTK_TYPE_LIST_BOX));
  assert(list);
  auto *row = GTK_WIDGET(gtk_list_box_get_row_at_index(list, 0));
  auto *caption = GTK_LABEL(descendant(row, GTK_TYPE_LABEL));
  assert(caption);
  point_at(second->root_box, 250, 150);
  set_main_window_title(&*first, "Current session title");
  paint(row);
  assert(std::string(gtk_label_get_text(caption)) == "Current session title");
  assert(pixel(row, 15, 3) == 0x6c2422);
  const auto full_width = gtk_widget_get_allocated_width(GTK_WIDGET(caption));
  const auto height = gtk_widget_get_allocated_height(row);
  auto *selected = GTK_WIDGET(gtk_list_box_get_row_at_index(list, 1));
  const auto blue = pixel(selected, 15, 5);
  int gap = 0;
  for (int y = height - 4; y < height + 8; ++y) {
    const auto color = pixel(row, 15, y);
    if (color != 0x6c2422 && color != blue) ++gap;
  }
  assert(gap == 2);
  assert(pixel(selected, 0, height / 2) != blue);
  assert(pixel(row, 0, height / 2) == pixel(row, 15, 3));
  point_at(row, 65, height / 2);
  assert(pixel(row, 15, 3) == 0x732a28);
  assert(gtk_widget_get_allocated_width(GTK_WIDGET(caption)) < full_width);
  assert(gtk_widget_get_allocated_height(row) == height);
  const auto hover_width = gtk_widget_get_allocated_width(GTK_WIDGET(caption));
  // Pick a title at the actual font's width boundary.
  std::string boundary;
  int text_width = 0;
  auto *layout = gtk_widget_create_pango_layout(row, "");
  while (text_width <= hover_width) {
    boundary += 'M';
    pango_layout_set_text(layout, boundary.c_str(), -1);
    pango_layout_get_pixel_size(layout, &text_width, nullptr);
  }
  g_object_unref(layout);
  assert(text_width <= full_width);
  set_main_window_title(&*first, boundary);
  paint(row);
  assert(pango_layout_is_ellipsized(gtk_label_get_layout(caption)));
  point_at(row, gtk_widget_get_allocated_width(row) - 10, height / 2);
  assert(gtk_widget_get_allocated_width(GTK_WIDGET(caption)) == hover_width);
  point_at(second->root_box, 250, 150);
  assert(!pango_layout_is_ellipsized(gtk_label_get_layout(caption)));
  // Switch the inactive page from a custom exterior to its GTK header color.
  auto *theme = gtk_css_provider_new();
  gtk_css_provider_load_from_data(theme, "* {background: white;}", -1, nullptr);
  gtk_style_context_add_provider(gtk_widget_get_style_context(first->header_bar),
      GTK_STYLE_PROVIDER(theme), GTK_STYLE_PROVIDER_PRIORITY_USER);
  set_main_window_colors(&*first, {});
  paint(row);
  assert(pixel(row, 15, 3) == 0xebebeb);
  gtk_style_context_remove_provider(gtk_widget_get_style_context(first->header_bar), GTK_STYLE_PROVIDER(theme));
  g_object_unref(theme);
  set_main_window_colors(&*first, {RgbColor{0x7a, 0x22, 0x22}, {}});
  destroy_terminal_runtime(second->runtime);
  second->runtime = nullptr;
  destroy_terminal_tab(b);
  destroy_terminal_runtime(first->runtime);
  first->runtime = nullptr;
  destroy_terminal_tab(a);
  release_main_window(&*second);
  release_main_window(&*first);
  std::puts("PASS docked tab title, color and full-width hover layout");
  return 0;
}

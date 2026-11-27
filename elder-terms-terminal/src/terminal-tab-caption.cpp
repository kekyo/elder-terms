// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

#include "terminal-tab-caption.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace elder_terms {

static constexpr gint64 leading_pause_us = 300 * G_TIME_SPAN_MILLISECOND;
static constexpr gint64 trailing_pause_us = 450 * G_TIME_SPAN_MILLISECOND;
static constexpr double pixels_per_second = 80;

struct TabCaption {
  GtkWidget *label;
  std::string latest;
  PangoLayout *scrolling = nullptr;
  int text_width = 0;
  double offset = 0;
  gint64 last_step = 0;
  gint64 reached_end = 0;
  guint tick = 0;
  bool pending = false;

  ~TabCaption() { g_clear_object(&scrolling); }
};

static bool can_scroll(GtkWidget *label) {
  if (!gtk_widget_get_mapped(label)) return false;
  gboolean enabled;
  g_object_get(gtk_widget_get_settings(label), "gtk-enable-animations", &enabled, nullptr);
  auto *window = gtk_widget_get_window(gtk_widget_get_toplevel(label));
  return enabled && window && !(gdk_window_get_state(window) & GDK_WINDOW_STATE_ICONIFIED);
}

static void stop_scrolling(TabCaption &caption) {
  if (!caption.tick) return;
  gtk_widget_remove_tick_callback(caption.label, caption.tick);
  caption.tick = 0;
  caption.pending = false;
  g_clear_object(&caption.scrolling);
  gtk_widget_queue_draw(caption.label);
}

static bool begin_pass(TabCaption &caption, gint64 now) {
  // Keep GTK's label and accessibility text current while the saved layout
  // finishes displaying the previous title in its original font and direction.
  auto *layout = pango_layout_copy(gtk_label_get_layout(GTK_LABEL(caption.label)));
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
  pango_layout_set_width(layout, -1);
  pango_layout_get_pixel_size(layout, &caption.text_width, nullptr);
  if (caption.text_width <= gtk_widget_get_allocated_width(caption.label)) {
    g_object_unref(layout);
    return false;
  }
  caption.scrolling = layout;
  caption.offset = 0;
  caption.last_step = now + leading_pause_us;
  caption.reached_end = 0;
  return true;
}

static gboolean advance(GtkWidget *label, GdkFrameClock *clock, gpointer data) {
  auto &caption = *static_cast<TabCaption *>(data);
  const auto now = gdk_frame_clock_get_frame_time(clock);
  const int distance = std::max(0, caption.text_width - gtk_widget_get_allocated_width(label));
  const auto elapsed = std::max<gint64>(0, now - caption.last_step);
  caption.last_step = std::max(now, caption.last_step);
  caption.offset = std::min<double>(distance, caption.offset + pixels_per_second * elapsed / G_TIME_SPAN_SECOND);
  gtk_widget_queue_draw(label);
  if (caption.offset < distance) {
    caption.reached_end = 0;
    return G_SOURCE_CONTINUE;
  }
  if (!caption.reached_end) caption.reached_end = now;
  if (distance && now - caption.reached_end < trailing_pause_us) return G_SOURCE_CONTINUE;

  g_clear_object(&caption.scrolling);
  if (caption.pending) {
    caption.pending = false;
    if (begin_pass(caption, now)) return G_SOURCE_CONTINUE;
  }
  caption.tick = 0;
  return G_SOURCE_REMOVE;
}

static void title_changed(GtkWidget *label, GParamSpec *, TabCaption *caption) {
  const auto *title = gtk_label_get_text(GTK_LABEL(label));
  if (caption->latest == title) return;
  caption->latest = title;
  if (caption->scrolling) caption->pending = true;
  else if (can_scroll(label) && begin_pass(*caption, g_get_monotonic_time()))
    caption->tick = gtk_widget_add_tick_callback(label, advance, caption, nullptr);
}

static gboolean draw_caption(GtkWidget *label, cairo_t *cr, TabCaption *caption) {
  if (!caption->scrolling) return FALSE;
  const int width = gtk_widget_get_allocated_width(label);
  const int distance = std::max(0, caption->text_width - width);
  int offset = std::min(distance, static_cast<int>(std::round(caption->offset)));
  const auto *line = pango_layout_get_line_readonly(caption->scrolling, 0);
  if (line && line->resolved_dir == PANGO_DIRECTION_RTL) offset = distance - offset;
  GtkAllocation allocation;
  gtk_widget_get_allocation(label, &allocation);
  int y;
  gtk_label_get_layout_offsets(GTK_LABEL(label), nullptr, &y);
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, width, gtk_widget_get_allocated_height(label));
  cairo_clip(cr);
  // GTK reports the layout origin in its GdkWindow; ::draw uses label-local coordinates.
  gtk_render_layout(gtk_widget_get_style_context(label), cr, -offset, y - allocation.y, caption->scrolling);
  cairo_restore(cr);
  return TRUE;
}

static gboolean window_state_changed(GtkWidget *, GdkEventWindowState *event, GtkWidget *label) {
  if (event->new_window_state & GDK_WINDOW_STATE_ICONIFIED)
    stop_scrolling(*static_cast<TabCaption *>(g_object_get_data(G_OBJECT(label), "tab-caption")));
  return FALSE;
}

GtkWidget *create_terminal_tab_caption(GtkWidget *title) {
  auto *label = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
  g_object_bind_property(title, "label", label, "label", G_BINDING_SYNC_CREATE);
  auto *caption = new TabCaption{label, gtk_label_get_text(GTK_LABEL(label))};
  g_object_set_data_full(G_OBJECT(label), "tab-caption", caption,
      +[](gpointer data) { delete static_cast<TabCaption *>(data); });
  g_signal_connect(label, "notify::label", G_CALLBACK(title_changed), caption);
  g_signal_connect(label, "draw", G_CALLBACK(draw_caption), caption);
  g_signal_connect(label, "unmap", G_CALLBACK(+[](GtkWidget *, TabCaption *caption) {
    stop_scrolling(*caption);
  }), caption);
  g_signal_connect_object(gtk_widget_get_settings(label), "notify::gtk-enable-animations",
      G_CALLBACK(+[](GObject *, GParamSpec *, GtkWidget *label) {
        if (!can_scroll(label))
          stop_scrolling(*static_cast<TabCaption *>(g_object_get_data(G_OBJECT(label), "tab-caption")));
      }), label, G_CONNECT_DEFAULT);
  g_signal_connect(label, "hierarchy-changed", G_CALLBACK(+[](GtkWidget *label, GtkWidget *previous, gpointer) {
    if (GTK_IS_WINDOW(previous))
      g_signal_handlers_disconnect_by_func(previous, reinterpret_cast<gpointer>(window_state_changed), label);
    auto *host = gtk_widget_get_toplevel(label);
    if (GTK_IS_WINDOW(host))
      g_signal_connect_object(host, "window-state-event", G_CALLBACK(window_state_changed), label, G_CONNECT_DEFAULT);
  }), nullptr);
  return label;
}

}

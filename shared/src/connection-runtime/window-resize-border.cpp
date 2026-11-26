// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "window-resize-border.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace elder_terms {

static constexpr const char *border_edge_key = "connection-resize-edge";
static constexpr const char *resize_border_css =
    // GTK uses decoration margins for invisible resize grips. Keep the theme's
    // painted outline and shadow, but receive drags on the visible frame.
    "window.connection-side-borders > decoration { margin-left: 0; margin-right: 0; margin-top: 0; }"
    "window.connection-bottom-border > decoration { margin-bottom: 0; }";

static GdkWindowState required_edges(GdkWindowEdge edge) {
  switch (edge) {
  case GDK_WINDOW_EDGE_WEST: return GDK_WINDOW_STATE_LEFT_RESIZABLE;
  case GDK_WINDOW_EDGE_EAST: return GDK_WINDOW_STATE_RIGHT_RESIZABLE;
  case GDK_WINDOW_EDGE_NORTH: return GDK_WINDOW_STATE_TOP_RESIZABLE;
  case GDK_WINDOW_EDGE_NORTH_WEST:
    return static_cast<GdkWindowState>(GDK_WINDOW_STATE_LEFT_RESIZABLE | GDK_WINDOW_STATE_TOP_RESIZABLE);
  case GDK_WINDOW_EDGE_NORTH_EAST:
    return static_cast<GdkWindowState>(GDK_WINDOW_STATE_RIGHT_RESIZABLE | GDK_WINDOW_STATE_TOP_RESIZABLE);
  case GDK_WINDOW_EDGE_SOUTH_WEST:
    return static_cast<GdkWindowState>(GDK_WINDOW_STATE_LEFT_RESIZABLE | GDK_WINDOW_STATE_BOTTOM_RESIZABLE);
  case GDK_WINDOW_EDGE_SOUTH_EAST:
    return static_cast<GdkWindowState>(GDK_WINDOW_STATE_RIGHT_RESIZABLE | GDK_WINDOW_STATE_BOTTOM_RESIZABLE);
  default: return GDK_WINDOW_STATE_BOTTOM_RESIZABLE;
  }
}

static std::optional<GdkWindowEdge> allowed_edge(GtkWidget *widget,
    GdkWindowEdge edge, GdkWindowEdge corner) {
  auto *toplevel = gtk_widget_get_toplevel(widget);
  if (!GTK_IS_WINDOW(toplevel) || !gtk_window_get_resizable(GTK_WINDOW(toplevel))) return std::nullopt;
  auto *native = gtk_widget_get_window(toplevel);
  if (!native) return std::nullopt;
  const auto state = gdk_window_get_state(native);
  if (state & (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN)) return std::nullopt;
  const auto constrained = state & (GDK_WINDOW_STATE_TILED | GDK_WINDOW_STATE_TOP_TILED |
      GDK_WINDOW_STATE_RIGHT_TILED | GDK_WINDOW_STATE_BOTTOM_TILED | GDK_WINDOW_STATE_LEFT_TILED);
  const auto allowed = [state, constrained](GdkWindowEdge edge) {
    const auto required = required_edges(edge);
    return !constrained || (state & required) == required;
  };
  if (allowed(corner)) return corner;
  return allowed(edge) ? std::optional{edge} : std::nullopt;
}

static std::optional<GdkWindowEdge> resize_edge(GtkWidget *border, double x, double y) {
  auto edge = static_cast<GdkWindowEdge>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(border), border_edge_key)) - 1);
  if (gtk_widget_get_direction(border) == GTK_TEXT_DIR_RTL) {
    if (edge == GDK_WINDOW_EDGE_WEST) edge = GDK_WINDOW_EDGE_EAST;
    else if (edge == GDK_WINDOW_EDGE_EAST) edge = GDK_WINDOW_EDGE_WEST;
  }
  const auto width = gtk_widget_get_allocated_width(border);
  const auto height = gtk_widget_get_allocated_height(border);
  if (x < 0 || y < 0 || x >= width || y >= height) return std::nullopt;
  auto corner = edge;
  if (edge == GDK_WINDOW_EDGE_SOUTH) {
    if (x < std::min(16, width / 2)) corner = GDK_WINDOW_EDGE_SOUTH_WEST;
    else if (x >= width - std::min(16, width / 2)) corner = GDK_WINDOW_EDGE_SOUTH_EAST;
  } else if (y >= height - std::min(16, height / 2)) {
    corner = edge == GDK_WINDOW_EDGE_WEST ? GDK_WINDOW_EDGE_SOUTH_WEST : GDK_WINDOW_EDGE_SOUTH_EAST;
  }
  return allowed_edge(border, edge, corner);
}

static void set_resize_cursor(GdkWindow *native, std::optional<GdkWindowEdge> edge) {
  if (!native) return;
  const char *name = nullptr;
  if (edge) {
    switch (*edge) {
    case GDK_WINDOW_EDGE_WEST:
    case GDK_WINDOW_EDGE_EAST: name = "ew-resize"; break;
    case GDK_WINDOW_EDGE_NORTH_EAST:
    case GDK_WINDOW_EDGE_SOUTH_WEST: name = "nesw-resize"; break;
    case GDK_WINDOW_EDGE_NORTH_WEST:
    case GDK_WINDOW_EDGE_SOUTH_EAST: name = "nwse-resize"; break;
    default: name = "ns-resize"; break;
    }
  }
  auto *cursor = name ? gdk_cursor_new_from_name(gdk_window_get_display(native), name) : nullptr;
  gdk_window_set_cursor(native, cursor);
  if (cursor) g_object_unref(cursor);
}

static std::optional<GdkWindowEdge> header_edge(GtkWidget *window, GtkWidget *border,
                                                 double x, double y) {
  GtkWidget *header = gtk_window_get_titlebar(GTK_WINDOW(window));
  int hx, hy;
  if (!header || !gtk_widget_get_visible(border) ||
      !gtk_widget_translate_coordinates(window, header, static_cast<int>(x), static_cast<int>(y), &hx, &hy))
    return std::nullopt;
  x = hx;
  y = hy;
  const int width = gtk_widget_get_allocated_width(header);
  const int height = gtk_widget_get_allocated_height(header);
  if (x < 0 || y < 0 || x >= width || y >= height) return std::nullopt;
  const int band = gtk_widget_get_allocated_width(border);
  const bool left = x < std::min(band, width / 2);
  const bool right = x >= width - std::min(band, width / 2);
  const bool top = y < std::min(band, height / 2);
  if (!left && !right && !top) return std::nullopt;
  const auto edge = left ? GDK_WINDOW_EDGE_WEST : right ? GDK_WINDOW_EDGE_EAST : GDK_WINDOW_EDGE_NORTH;
  auto corner = edge;
  if (top && (left || x < std::min(16, width / 2))) corner = GDK_WINDOW_EDGE_NORTH_WEST;
  if (top && (right || x >= width - std::min(16, width / 2))) corner = GDK_WINDOW_EDGE_NORTH_EAST;
  return allowed_edge(header, edge, corner);
}

bool begin_window_header_resize(GtkWidget *window, GdkEventButton *button) {
  if (button->type != GDK_BUTTON_PRESS || button->button != GDK_BUTTON_PRIMARY) return false;
  auto *border = GTK_WIDGET(g_object_get_data(G_OBJECT(window), "connection-start-border"));
  auto *native = gtk_widget_get_window(window);
  if (!border || !native) return false;
  gint x, y;
  gdk_window_get_origin(native, &x, &y);
  const auto edge = header_edge(window, border, button->x_root - x, button->y_root - y);
  if (!edge) return false;
  gdk_window_begin_resize_drag_for_device(native, *edge, gdk_event_get_device(reinterpret_cast<GdkEvent *>(button)),
      button->button, static_cast<int>(button->x_root), static_cast<int>(button->y_root), button->time);
  return true;
}

static void install_header_resize(GtkWidget *window) {
  // GtkWindow offers WM events here before dispatching its own titlebar move gesture.
  g_signal_connect(window, "event", G_CALLBACK(+[](GtkWidget *widget, GdkEvent *event, gpointer) -> gboolean {
    return event->type == GDK_BUTTON_PRESS && begin_window_header_resize(widget, &event->button);
  }), nullptr);
  auto *motion = gtk_event_controller_motion_new(window);
  gtk_event_controller_set_propagation_phase(motion, GTK_PHASE_CAPTURE);
  g_signal_connect(motion, "motion", G_CALLBACK(+[](GtkEventControllerMotion *controller,
      double x, double y, gpointer) {
    auto *window = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    auto *side = GTK_WIDGET(g_object_get_data(G_OBJECT(window), "connection-start-border"));
    if (side) set_resize_cursor(gtk_widget_get_window(window), header_edge(window, side, x, y));
  }), nullptr);
  g_signal_connect(motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion *controller, gpointer) {
    auto *window = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    set_resize_cursor(gtk_widget_get_window(window), std::nullopt);
  }), nullptr);
  g_object_set_data_full(G_OBJECT(window), "connection-header-motion", motion, g_object_unref);
}

static void update_resize_margin(GtkWidget *border, GParamSpec *, gpointer) {
  auto *toplevel = gtk_widget_get_toplevel(border);
  if (!GTK_IS_WINDOW(toplevel) || gtk_widget_in_destruction(toplevel)) return;
  const auto edge = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(border), border_edge_key)) - 1;
  const auto *name = edge == GDK_WINDOW_EDGE_SOUTH ? "connection-bottom-border" : "connection-side-borders";
  const auto *active = g_object_get_data(G_OBJECT(toplevel), edge == GDK_WINDOW_EDGE_SOUTH ?
      "connection-bottom-widget" : "connection-start-border");
  if (active != border) return;
  auto *context = gtk_widget_get_style_context(toplevel);
  if (gtk_widget_get_visible(border)) gtk_style_context_add_class(context, name);
  else gtk_style_context_remove_class(context, name);
}

void install_window_resize_borders(GtkWindow *window, GtkWidget *start,
                                   GtkWidget *end, GtkWidget *bottom) {
  if (g_object_get_data(G_OBJECT(window), "connection-start-border") == start) return;
  g_object_set_data_full(G_OBJECT(window), "connection-start-border", g_object_ref(start), g_object_unref);
  g_object_set_data_full(G_OBJECT(window), "connection-bottom-widget", g_object_ref(bottom), g_object_unref);
  if (!g_object_get_data(G_OBJECT(window), "connection-header-motion")) {
    auto *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, resize_border_css, -1, nullptr);
    gtk_style_context_add_provider(gtk_widget_get_style_context(GTK_WIDGET(window)),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    install_header_resize(GTK_WIDGET(window));
  }
  if (g_object_get_data(G_OBJECT(start), border_edge_key)) {
    update_resize_margin(start, nullptr, nullptr);
    update_resize_margin(bottom, nullptr, nullptr);
    return;
  }
  for (const auto &[border, edge] : {std::pair{start, GDK_WINDOW_EDGE_WEST},
      std::pair{end, GDK_WINDOW_EDGE_EAST}, std::pair{bottom, GDK_WINDOW_EDGE_SOUTH}}) {
    g_object_set_data(G_OBJECT(border), border_edge_key, GINT_TO_POINTER(edge + 1));
    gtk_widget_add_events(border, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK | GDK_ENTER_NOTIFY_MASK);
    g_signal_connect(border, "enter-notify-event", G_CALLBACK(+[](GtkWidget *widget, GdkEventCrossing *event, gpointer) -> gboolean {
      set_resize_cursor(gtk_widget_get_window(widget), resize_edge(widget, event->x, event->y));
      return FALSE;
    }), nullptr);
    g_signal_connect(border, "motion-notify-event", G_CALLBACK(+[](GtkWidget *widget, GdkEventMotion *event, gpointer) -> gboolean {
      set_resize_cursor(gtk_widget_get_window(widget), resize_edge(widget, event->x, event->y));
      return FALSE;
    }), nullptr);
    g_signal_connect(border, "button-press-event", G_CALLBACK(+[](GtkWidget *widget, GdkEventButton *event, gpointer) -> gboolean {
      if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_PRIMARY) return FALSE;
      const auto edge = resize_edge(widget, event->x, event->y);
      if (!edge) return FALSE;
      gtk_window_begin_resize_drag(GTK_WINDOW(gtk_widget_get_toplevel(widget)), *edge,
          event->button, static_cast<int>(event->x_root), static_cast<int>(event->y_root), event->time);
      return TRUE;
    }), nullptr);
    if (border != end) {
      g_signal_connect(border, "notify::visible", G_CALLBACK(update_resize_margin), nullptr);
      update_resize_margin(border, nullptr, nullptr);
    }
  }
}

} // namespace elder_terms

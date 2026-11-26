// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
#include "titlebar-dock.h"
#include "dock-feedback.h"
#include <gdk/gdkx.h>
#include <glib-unix.h>
#include <X11/Xatom.h>
#include <X11/Xproto.h>
#include <X11/extensions/record.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>
#include <cstring>
#include <memory>
#include <utility>

namespace elder_terms {

struct TitlebarDock {
  GtkWidget *window;
  std::string bus_name;
  std::function<bool()> has_peer;
  TitlebarDockProbe probe;
  std::function<void(const std::string &, guint32)> drop;
  Display *control;
  Atom marker, moveresize;
  GtkGesture *press;
  Display *stream = nullptr;
  XRecordContext context = 0;
  guint source = 0;
  Window xid = None, root = None;
  int x = 0, y = 0;
  KeyCode escape = 0;
  bool moving = false, finished = false;
  guint tick = 0;
  GtkWidget *badge = nullptr;
  Window hover_target = None;
  std::shared_ptr<bool> accepted{};
};

guint32 titlebar_dock_window_id(GtkWidget *window) {
  auto *native = gtk_widget_get_window(window);
  return native && GDK_IS_X11_WINDOW(native) ? gdk_x11_window_get_xid(native) : 0;
}

// Use the actual server stack, including popup windows and other applications.
// A WM can reparent the client inside a frame several windows deep.
static Window outer_frame(Display *display, Window root, Window client) {
  Window parent, tree_root, *children;
  unsigned count;
  while (XQueryTree(display, client, &tree_root, &parent, &children, &count)) {
    XFree(children);
    if (parent == root) return client;
    if (!parent) break;
    client = parent;
  }
  return None;
}

static bool contains_pointer(TitlebarDock *dock, Window child) {
  XWindowAttributes attr;
  int x, y;
  Window ignored;
  // KWin can still have its invisible fullscreen grab window mapped at release.
  if (!XGetWindowAttributes(dock->control, child, &attr) ||
      attr.map_state != IsViewable || attr.c_class == InputOnly ||
      !XTranslateCoordinates(dock->control, dock->root, child, dock->x, dock->y, &x, &y, &ignored) ||
      x < 0 || y < 0 || x >= attr.width || y >= attr.height) return false;
  int count = 0, ordering = 0;
  auto *rects = XShapeGetRectangles(dock->control, child, ShapeInput, &count, &ordering);
  bool hit = false;
  for (int i = 0; i < count && !hit; ++i) {
    const auto &r = rects[i];
    hit = x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
  }
  XFree(rects);
  return hit;
}

static std::pair<std::string, Window> drop_target(TitlebarDock *dock) {
  const auto source_frame = outer_frame(dock->control, dock->root, dock->xid);
  Window root, parent, *children;
  unsigned count;
  if (!source_frame || !XQueryTree(dock->control, dock->root, &root, &parent, &children, &count)) return {};
  Window target = None;
  for (unsigned i = count; i && !target; --i) {
    const auto child = children[i - 1];
    if (child != source_frame && contains_pointer(dock, child)) target = child;
  }
  XFree(children);
  while (target) {
    Atom type = None;
    int format;
    unsigned long length, extra;
    unsigned char *data = nullptr;
    const int status = XGetWindowProperty(dock->control, target, dock->marker, 0, 64, False, XA_STRING,
        &type, &format, &length, &extra, &data);
    if (status != Success) return {};
    std::string name;
    if (type == XA_STRING && format == 8 && !extra && length)
      name.assign(reinterpret_cast<const char *>(data), length);
    XFree(data);
    if (type == XA_STRING && (name.empty() || g_dbus_is_unique_name(name.c_str()))) return {name, target};
    Window next;
    int x, y;
    if (!XTranslateCoordinates(dock->control, dock->root, target, dock->x, dock->y, &x, &y, &next)) break;
    target = next;
  }
  return {};
}

static std::pair<std::string, Window> target_at_pointer(TitlebarDock *dock) {
  auto *display = gtk_widget_get_display(dock->window);
  // Other clients can destroy a window between our queries to the X server.
  gdk_x11_display_error_trap_push(display);
  auto target = drop_target(dock);
  gdk_x11_display_error_trap_pop_ignored(display);
  return target;
}

static void update_badge(TitlebarDock *dock) {
  if (!dock->accepted || !*dock->accepted) {
    if (dock->badge) gtk_widget_hide(dock->badge);
    return;
  }
  // The WM owns the cursor during a native move. Place the same glyph used
  // by GTK tab drags alongside it, with an empty input region.
  const int scale = gtk_widget_get_scale_factor(dock->window);
  if (!dock->badge) {
    dock->badge = gtk_window_new(GTK_WINDOW_POPUP);
    auto *popup = GTK_WINDOW(dock->badge);
    gtk_window_set_screen(popup, gtk_widget_get_screen(dock->window));
    gtk_window_set_type_hint(popup, GDK_WINDOW_TYPE_HINT_DND);
    gtk_window_set_accept_focus(popup, FALSE);
    auto *surface = dock_feedback_surface(DOCK_FEEDBACK_DOCK, scale);
    auto *image = gtk_image_new_from_surface(surface);
    gtk_container_add(GTK_CONTAINER(popup), image);
    gtk_widget_show(image);
    auto *shape = gdk_cairo_region_create_from_surface(surface);
    gtk_widget_shape_combine_region(dock->badge, shape);
    cairo_region_destroy(shape);
    cairo_surface_destroy(surface);
    auto *empty = cairo_region_create();
    gtk_widget_input_shape_combine_region(dock->badge, empty);
    cairo_region_destroy(empty);
  }
  int x = dock->x / scale, y = dock->y / scale;
  auto *monitor = gdk_display_get_monitor_at_point(gtk_widget_get_display(dock->window), x, y);
  if (monitor) {
    GdkRectangle bounds;
    gdk_monitor_get_geometry(monitor, &bounds);
    constexpr int offset = 12;
    constexpr int total = offset + DOCK_FEEDBACK_SIZE;
    x += x + total <= bounds.x + bounds.width ? offset : -total;
    y += y + total <= bounds.y + bounds.height ? offset : -total;
  }
  gtk_window_move(GTK_WINDOW(dock->badge), x, y);
  gtk_widget_show(dock->badge);
}

static gboolean update_hover(GtkWidget *, GdkFrameClock *, gpointer data) {
  auto *dock = static_cast<TitlebarDock *>(data);
  dock->tick = 0;
  const auto [name, target] = target_at_pointer(dock);
  if (target != dock->hover_target) {
    dock->hover_target = target;
    dock->accepted.reset();
    update_badge(dock);
    if (target) {
      dock->accepted = std::make_shared<bool>(false);
      // Ignore delayed peer replies after leave, release or window destruction.
      dock->probe(name, target, [dock, current = std::weak_ptr(dock->accepted)](bool ready) {
        if (auto accepted = current.lock()) {
          *accepted = ready;
          if (!dock->tick) update_badge(dock);
        }
      });
    }
  } else update_badge(dock);
  return G_SOURCE_REMOVE;
}

// During a native move the WM owns the pointer grab. RECORD orders our own
// EWMH move request, physical motion, Escape and button release on one stream.
// Button device events only guarantee detail/time; motion carries root X/Y.
static void record_event(XPointer data, XRecordInterceptData *packet) {
  auto *dock = reinterpret_cast<TitlebarDock *>(data);
  if (!dock->finished && packet->category == XRecordFromClient && !packet->client_swapped &&
      packet->data_len * 4 == sizeof(xSendEventReq)) {
    xSendEventReq request;
    std::memcpy(&request, packet->data, sizeof(request));
    const auto &message = request.event.u.clientMessage;
    if (request.reqType == X_SendEvent && request.event.u.u.type == ClientMessage &&
        message.window == dock->xid && message.u.l.type == dock->moveresize) {
      dock->moving = message.u.l.longs2 == 8 && message.u.l.longs3 == Button1;
    }
  }
  if (packet->category == XRecordFromServer) {
    for (size_t pos = 0; !dock->finished && pos + sizeof(xEvent) <= packet->data_len * 4; pos += sizeof(xEvent)) {
      xEvent event;
      std::memcpy(&event, packet->data + pos, sizeof(event));
      const auto &input = event.u.keyButtonPointer;
      if (event.u.u.type == MotionNotify) {
        dock->root = input.root;
        dock->x = input.rootX;
        dock->y = input.rootY;
      } else if (event.u.u.type == KeyPress && event.u.u.detail == dock->escape) {
        dock->moving = false;
        dock->finished = true;
      } else if (event.u.u.type == ButtonRelease && event.u.u.detail == Button1) {
        dock->finished = true;
      }
    }
  }
  XRecordFreeData(packet);
}

static void stop_recording(TitlebarDock *dock) {
  if (dock->tick) gtk_widget_remove_tick_callback(dock->window, std::exchange(dock->tick, 0));
  dock->hover_target = None;
  dock->accepted.reset();
  if (dock->badge) gtk_widget_destroy(std::exchange(dock->badge, nullptr));
  if (dock->source) g_source_remove(std::exchange(dock->source, 0));
  if (dock->context) {
    XRecordFreeContext(dock->control, std::exchange(dock->context, 0));
    XSync(dock->control, False);
  }
  if (dock->stream) XCloseDisplay(std::exchange(dock->stream, nullptr));
}

static gboolean read_move(gint, GIOCondition condition, gpointer data) {
  auto *dock = static_cast<TitlebarDock *>(data);
  if (condition & G_IO_IN) {
    XRecordProcessReplies(dock->stream);
    if (!dock->finished && !(condition & (G_IO_HUP | G_IO_ERR))) {
      if (dock->moving && dock->root && !dock->tick)
        dock->tick = gtk_widget_add_tick_callback(dock->window, update_hover, dock, nullptr);
      return G_SOURCE_CONTINUE;
    }
  }
  dock->source = 0;
  stop_recording(dock);
  if (dock->finished && dock->moving && dock->root) {
    // The recipient also rechecks the addressed tray at release.
    const auto [name, target] = target_at_pointer(dock);
    if (target) {
      // A same-process dock may destroy this source window immediately.
      auto drop = dock->drop;
      drop(name, target);
    }
  }
  return G_SOURCE_REMOVE;
}

static void header_pressed(GtkGestureMultiPress *, gint presses, gdouble x, gdouble y, gpointer data) {
  auto *dock = static_cast<TitlebarDock *>(data);
  stop_recording(dock);
  auto *header = gtk_window_get_titlebar(GTK_WINDOW(dock->window));
  int hx, hy;
  if (presses != 1 || !dock->has_peer() || !header ||
      !gtk_widget_translate_coordinates(dock->window, header, x, y, &hx, &hy) ||
      hx < 0 || hy < 0 || hx >= gtk_widget_get_allocated_width(header) ||
      hy >= gtk_widget_get_allocated_height(header)) return;
  dock->xid = titlebar_dock_window_id(dock->window);
  dock->moving = dock->finished = false;
  dock->root = None;
  dock->stream = XOpenDisplay(DisplayString(dock->control));
  if (!dock->stream) return;
  XRecordClientSpec client = dock->xid;
  XRecordRange range{};
  range.core_requests = {X_SendEvent, X_SendEvent};
  range.device_events = {KeyPress, MotionNotify};
  auto *ranges = &range;
  dock->context = XRecordCreateContext(dock->control, 0, &client, 1, &ranges, 1);
  XSync(dock->control, False);
  if (!dock->context || !XRecordEnableContextAsync(dock->stream, dock->context,
      record_event, reinterpret_cast<XPointer>(dock))) {
    stop_recording(dock);
    return;
  }
  dock->source = g_unix_fd_add(ConnectionNumber(dock->stream),
      static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP), read_move, dock);
}

bool install_titlebar_dock(GtkWidget *window, const char *bus_name, std::function<bool()> has_peer, TitlebarDockProbe probe,
    std::function<void(const std::string &, guint32)> drop) {
  auto *gdk = gtk_widget_get_display(window);
  if (!GDK_IS_X11_DISPLAY(gdk)) return false;
  auto *display = gdk_x11_display_get_xdisplay(gdk);
  int major, minor;
  int opcode, first_event, first_error;
  if (!XQueryExtension(display, RECORD_NAME, &opcode, &first_event, &first_error) ||
      !XRecordQueryVersion(display, &major, &minor) || !XShapeQueryVersion(display, &major, &minor) ||
      (major == 1 && minor < 1) ||
      !gdk_x11_screen_supports_net_wm_hint(gtk_widget_get_screen(window),
          gdk_atom_intern_static_string("_NET_WM_MOVERESIZE"))) return false;
  auto *dock = new TitlebarDock{window, bus_name ? bus_name : "", std::move(has_peer), std::move(probe), std::move(drop),
      display, XInternAtom(display, "_ELDER_TERMS_DOCK_WINDOW", False),
      XInternAtom(display, "_NET_WM_MOVERESIZE", False), gtk_gesture_multi_press_new(window)};
  dock->escape = XKeysymToKeycode(display, XK_Escape);
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dock->press), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(dock->press), GTK_PHASE_CAPTURE);
  g_signal_connect(dock->press, "pressed", G_CALLBACK(header_pressed), dock);
  g_signal_connect(window, "map", G_CALLBACK(+[](GtkWidget *w, gpointer data) {
    auto *dock = static_cast<TitlebarDock *>(data);
    XChangeProperty(dock->control, titlebar_dock_window_id(w), dock->marker, XA_STRING, 8, PropModeReplace,
        reinterpret_cast<const unsigned char *>(dock->bus_name.data()), dock->bus_name.size());
  }), dock);
  g_signal_connect(window, "unmap", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    stop_recording(static_cast<TitlebarDock *>(data));
  }), dock);
  g_object_set_data_full(G_OBJECT(window), "terminal-titlebar-dock", dock, +[](gpointer data) {
    auto *dock = static_cast<TitlebarDock *>(data);
    stop_recording(dock);
    g_object_unref(dock->press);
    delete dock;
  });
  return true;
}
}

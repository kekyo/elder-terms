// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

#include "terminal-tabs.h"
#include "terminal-dock-bus.h"
#include "titlebar-dock.h"
#include "tab-tearoff.h"
#include "terminal-background.h"
#include "../../shared/src/connection-runtime/window-resize-border.h"
#include <algorithm>
#include <memory>
#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

struct TerminalTray {
  GtkWidget *window, *body, *frame_row, *splitter, *sidebar, *list, *pages, *headers;
  std::vector<TerminalTab *> tabs;
  TerminalTab *active = nullptr;
  bool native_title_drag = false;
  TerminalTab *framed = nullptr;
  guint16 undock_keycode = 0;
};
struct TerminalTab {
  MainWindow *page;
  TerminalTabActions actions;
  TerminalTray *tray = nullptr;
  GtkWidget *row = nullptr, *pick = nullptr;
  TearoffGesture *gesture = nullptr;
};
static std::vector<TerminalTray *> trays;
static bool external_peers = false;

static TerminalTray *tray_by_xid(guint32 xid) {
  for (auto *tray : trays)
    if (gtk_widget_get_mapped(tray->window) && titlebar_dock_window_id(tray->window) == xid) return tray;
  return nullptr;
}

bool receive_terminal_window_drop(guint32 xid, const std::string &token) {
  auto *target = tray_by_xid(xid);
  if (!target || !target->active || !target->active->actions.movable()) return false;
  target->active->actions.receive(token, [](bool) {});
  return true;
}
static GtkTargetEntry dock_targets[] = {
    {const_cast<char *>("application/x-elder-terms-terminal-tabs"), 0, 1},
    {const_cast<char *>("application/x-rootwindow-drop"), 0, 2}};

static void unparent(GtkWidget *child) {
  if (auto *parent = gtk_widget_get_parent(child)) gtk_container_remove(GTK_CONTAINER(parent), child);
}

static void move_border(GtkWidget *border, GtkWidget *parent, int position, bool at_end) {
  g_object_ref(border);
  unparent(border);
  if (at_end) gtk_box_pack_end(GTK_BOX(parent), border, FALSE, TRUE, 0);
  else gtk_box_pack_start(GTK_BOX(parent), border, FALSE, TRUE, 0);
  if (position >= 0) gtk_box_reorder_child(GTK_BOX(parent), border, position);
  g_object_unref(border);
}

static void restore_frame(TerminalTray *tray) {
  auto *tab = tray->framed;
  if (!tab) return;
  auto *page = tab->page;
  move_border(page->frame_start_border, page->root_box, 0, false);
  move_border(page->frame_end_border, page->root_box, -1, true);
  move_border(page->frame_bottom_border, gtk_widget_get_parent(page->status_bar), 2, false);
  tray->framed = nullptr;
}

static void frame_page(TerminalTray *tray, TerminalTab *tab) {
  if (tray->framed == tab) return;
  restore_frame(tray);
  auto *page = tab->page;
  move_border(page->frame_start_border, tray->frame_row, 0, false);
  move_border(page->frame_end_border, tray->frame_row, -1, true);
  move_border(page->frame_bottom_border, tray->body, -1, true);
  tray->framed = tab;
}

static void update_drags() {
  const bool peers = external_peers || trays.size() > 1;
  for (auto *tray : trays) for (auto *tab : tray->tabs) {
    gtk_drag_source_set(tab->pick, peers ? GDK_BUTTON1_MASK : static_cast<GdkModifierType>(0),
        dock_targets, 2, GDK_ACTION_MOVE);
    tearoff_gesture_set_enabled(tab->gesture, !peers);
  }
}

void set_terminal_tab_external_peers(bool available) {
  if (available == external_peers) return;
  external_peers = available;
  update_drags();
}

static void refresh(TerminalTray *tray) {
  if (tray->tabs.empty()) {
    std::erase(trays, tray);
    gtk_widget_destroy(tray->window);
    g_object_unref(tray->window);
    delete tray;
    return;
  }
  auto *active = tray->active;
  gtk_widget_set_visible(tray->sidebar, tray->tabs.size() > 1);
  frame_page(tray, active);
  for (auto *tab : tray->tabs) tab->actions.host(tray->window, tray->body, tab == active);
  gtk_stack_set_visible_child(GTK_STACK(tray->headers), active->page->header_bar);
  gtk_stack_set_visible_child(GTK_STACK(tray->pages), active->page->root_box);
  install_window_resize_borders(GTK_WINDOW(tray->window), active->page->frame_start_border,
      active->page->frame_end_border, active->page->frame_bottom_border);
  gtk_list_box_select_row(GTK_LIST_BOX(tray->list), GTK_LIST_BOX_ROW(active->row));
  gtk_window_set_title(GTK_WINDOW(tray->window), gtk_label_get_text(GTK_LABEL(active->page->header_title_label)));
  focus_main_window_terminal_if_interactive(active->page);
}
void select_terminal_tab(TerminalTab *tab) {
  if (!tab || !tab->tray) return;
  tab->tray->active = tab;
  refresh(tab->tray);
}

static void remove_page(TerminalTab *tab) {
  auto *tray = tab->tray;
  if (tray->framed == tab) restore_frame(tray);
  const auto pos = std::find(tray->tabs.begin(), tray->tabs.end(), tab);
  const auto next = tray->tabs.erase(pos);
  if (tray->active == tab)
    tray->active = next != tray->tabs.end() ? *next : tray->tabs.empty() ? nullptr : tray->tabs.back();
  tray->undock_keycode = 0;
  gtk_list_box_unselect_row(GTK_LIST_BOX(tray->list), GTK_LIST_BOX_ROW(tab->row));
  unparent(tab->row);
  unparent(tab->page->root_box);
  unparent(tab->page->header_bar);
  tab->tray = nullptr;
}
static void add_page(TerminalTray *tray, TerminalTab *tab) {
  tray->tabs.push_back(tab);
  tab->tray = tray;
  gtk_container_add(GTK_CONTAINER(tray->list), tab->row);
  gtk_container_add(GTK_CONTAINER(tray->headers), tab->page->header_bar);
  gtk_container_add(GTK_CONTAINER(tray->pages), tab->page->root_box);
}

static void move_pages(const std::vector<TerminalTab *> &pages, TerminalTab *active, TerminalTray *target) {
  std::vector<TerminalTray *> sources;
  for (auto *tab : pages) {
    if (std::find(sources.begin(), sources.end(), tab->tray) == sources.end()) sources.push_back(tab->tray);
    remove_page(tab);
    add_page(target, tab);
  }
  target->active = active;
  refresh(target);
  for (auto *source : sources) refresh(source);
  update_drags();
}
bool dock_terminal_tabs(const std::vector<TerminalTab *> &pages, TerminalTab *active, TerminalTab *destination) {
  if (pages.empty() || !destination || !destination->tray ||
      std::find(pages.begin(), pages.end(), active) == pages.end()) return false;
  for (auto *tab : pages)
    if (!tab || !tab->tray || tab->tray == destination->tray || !tab->actions.movable()) return false;
  move_pages(pages, active, destination->tray);
  return true;
}

static void install_drop(GtkWidget *widget) {
  const bool files = gtk_drag_dest_get_target_list(widget) != nullptr;
  gtk_drag_dest_set(widget, GTK_DEST_DEFAULT_MOTION, dock_targets, 1, static_cast<GdkDragAction>(GDK_ACTION_MOVE | (files ? GDK_ACTION_COPY : 0)));
  if (files) gtk_drag_dest_add_uri_targets(widget);
  g_signal_connect(widget, "drag-drop", G_CALLBACK(+[](GtkWidget *widget, GdkDragContext *ctx,
      gint, gint, guint time, gpointer) -> gboolean {
    const auto target = gtk_drag_dest_find_target(widget, ctx, nullptr);
    if (target == GDK_NONE) return FALSE;
    gtk_drag_get_data(widget, ctx, target, time);
    return TRUE;
  }), nullptr);
  g_signal_connect(widget, "drag-data-received", G_CALLBACK(+[](GtkWidget *widget, GdkDragContext *ctx,
      gint, gint, GtkSelectionData *data, guint, guint time, gpointer) {
    if (gtk_selection_data_get_target(data) != gdk_atom_intern_static_string(dock_targets[0].target)) return;
    auto *tray = static_cast<TerminalTray *>(g_object_get_data(G_OBJECT(gtk_widget_get_toplevel(widget)), "terminal-tray"));
    const auto count = gtk_selection_data_get_length(data);
    if (!tray->active || count <= 0 || count > 256) { gtk_drag_finish(ctx, FALSE, FALSE, time); return; }
    auto *source = gtk_drag_get_source_widget(ctx);
    auto *tab = source ? static_cast<TerminalTab *>(g_object_get_data(G_OBJECT(source), "terminal-drag-tab")) : nullptr;
    if (tab) {
      bool row = source == tab->pick;
      const auto pages = row ? std::vector<TerminalTab *>{tab} : tab->tray->tabs;
      const bool accepted = dock_terminal_tabs(pages, row ? tab : tab->tray->active, tray->active);
      gtk_drag_finish(ctx, accepted, FALSE, time);
    } else {
      g_object_ref(ctx);
      tray->active->actions.receive(std::string(reinterpret_cast<const char *>(gtk_selection_data_get_data(data)), count),
          [ctx, time](bool ok) { gtk_drag_finish(ctx, ok, FALSE, time); g_object_unref(ctx); });
    }
  }), nullptr);
}

static TerminalTray *make_tray(GtkWidget *window) {
  auto *tray = new TerminalTray{};
  tray->window = GTK_WIDGET(g_object_ref(window));
  tray->body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  tray->frame_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  tray->splitter = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_position(GTK_PANED(tray->splitter), 220);
  gtk_paned_set_wide_handle(GTK_PANED(tray->splitter), TRUE);
  tray->sidebar = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tray->sidebar), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(tray->sidebar, 180, -1);
  gtk_widget_set_no_show_all(tray->sidebar, TRUE);
  tray->list = gtk_list_box_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(tray->list), "sidebar");
  gtk_container_add(GTK_CONTAINER(tray->sidebar), tray->list);
  tray->headers = gtk_stack_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(tray->headers), "terminal-tray-headers");
  tray->pages = gtk_stack_new();
  for (auto *stack : {tray->headers, tray->pages}) gtk_stack_set_homogeneous(GTK_STACK(stack), FALSE);
  gtk_paned_pack1(GTK_PANED(tray->splitter), tray->sidebar, FALSE, FALSE);
  gtk_paned_pack2(GTK_PANED(tray->splitter), tray->pages, TRUE, FALSE);
  gtk_box_pack_start(GTK_BOX(tray->frame_row), tray->splitter, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(tray->body), tray->frame_row, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(window), tray->body);
  gtk_window_set_titlebar(GTK_WINDOW(window), tray->headers);
  gtk_widget_show_all(tray->body);
  gtk_widget_show_all(tray->list);
  gtk_widget_show(tray->headers);
  g_object_set_data(G_OBJECT(window), "terminal-tray", tray);
  auto *bus = terminal_dock_connection();
  tray->native_title_drag = install_titlebar_dock(window, bus ? g_dbus_connection_get_unique_name(bus) : nullptr,
      [] { return external_peers || trays.size() > 1; },
      [tray](const std::string &name, guint32 xid) {
        if (!tray->active) return;
        if (auto *target = tray_by_xid(xid)) {
          if (target != tray && target->active && target->active->actions.movable()) {
            const auto pages = tray->tabs;
            dock_terminal_tabs(pages, tray->active, target->active);
          }
        } else if (!name.empty()) {
          const auto token = tray->active->actions.offer(tray->tabs, tray->active);
          if (!token.empty()) send_terminal_window_drop(name, xid, token);
        }
      });
  install_drop(tray->headers);
  install_drop(tray->body);
  g_signal_connect(window, "delete-event", G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer data) -> gboolean {
    auto tabs = static_cast<TerminalTray *>(data)->tabs;
    for (auto *tab : tabs) tab->actions.close();
    return TRUE;
  }), tray);
  g_signal_connect_after(window, "focus-in-event", G_CALLBACK(+[](GtkWidget *, GdkEventFocus *, gpointer data) -> gboolean {
    auto *tab = static_cast<TerminalTray *>(data)->active;
    if (tab) focus_main_window_terminal_if_interactive(tab->page);
    return FALSE;
  }), tray);
  g_signal_connect(window, "key-press-event", G_CALLBACK(+[](GtkWidget *, GdkEventKey *event, gpointer data) -> gboolean {
    auto *tray = static_cast<TerminalTray *>(data);
    if (tray->tabs.size() < 2 || !tray->active->actions.undock_key(event->keyval,
        static_cast<GdkModifierType>(event->state & gtk_accelerator_get_default_mod_mask()))) return FALSE;
    tray->undock_keycode = event->hardware_keycode;
    return TRUE;
  }), tray);
  g_signal_connect(window, "key-release-event", G_CALLBACK(+[](GtkWidget *, GdkEventKey *event, gpointer data) -> gboolean {
    auto *tray = static_cast<TerminalTray *>(data);
    if (!tray->undock_keycode || event->hardware_keycode != tray->undock_keycode) return FALSE;
    tray->undock_keycode = 0;
    return undock_terminal_tab(tray->active);
  }), tray);
  g_signal_connect(window, "focus-out-event", G_CALLBACK(+[](GtkWidget *, GdkEventFocus *, gpointer data) -> gboolean {
    static_cast<TerminalTray *>(data)->undock_keycode = 0;
    return FALSE;
  }), tray);
  trays.push_back(tray);
  return tray;
}

bool undock_terminal_tab(TerminalTab *tab) {
  if (!tab || !tab->tray || tab->tray->tabs.size() < 2 || !tab->actions.movable()) return false;
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  prepare_terminal_background_window(window);
  int width, height;
  gtk_window_get_size(GTK_WINDOW(tab->tray->window), &width, &height);
  gtk_window_set_default_size(GTK_WINDOW(window), width - gtk_widget_get_allocated_width(tab->tray->sidebar), height);
  auto *tray = make_tray(window);
  move_pages({tab}, tab, tray);
  gtk_widget_show(window);
  gtk_window_present(GTK_WINDOW(window));
  return true;
}

static void deferred_undock(gpointer data) {
  auto *row = static_cast<GtkWidget *>(data);
  g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer data) -> gboolean {
    auto *tab = static_cast<TerminalTab *>(g_object_get_data(G_OBJECT(data), "terminal-drag-tab"));
    undock_terminal_tab(tab);
    return G_SOURCE_REMOVE;
  }, g_object_ref(row), g_object_unref);
}

static void bind_source(GtkWidget *widget, TerminalTab *tab, bool row) {
  g_object_set_data(G_OBJECT(widget), "terminal-drag-tab", tab);
  if (!row) gtk_drag_source_set(widget, GDK_BUTTON1_MASK, dock_targets, 1, GDK_ACTION_MOVE);
  g_signal_connect(widget, "drag-begin", G_CALLBACK(+[](GtkWidget *, GdkDragContext *ctx, gpointer) {
    gtk_drag_set_icon_name(ctx, "utilities-terminal", 8, 8);
  }), nullptr);
  g_signal_connect(widget, "drag-data-get", G_CALLBACK(+[](GtkWidget *source, GdkDragContext *ctx,
      GtkSelectionData *data, guint info, guint, gpointer context) {
    auto *tab = static_cast<TerminalTab *>(context);
    if (info == 2) {
      if (source == tab->pick && gdk_drag_context_get_protocol(ctx) == GDK_DRAG_PROTO_ROOTWIN)
        deferred_undock(source);
      return;
    }
    const bool row = source == tab->pick;
    auto token = tab->actions.offer(row ? std::vector<TerminalTab *>{tab} : tab->tray->tabs, row ? tab : tab->tray->active);
    gtk_selection_data_set(data, gtk_selection_data_get_target(data), 8,
        reinterpret_cast<const guchar *>(token.data()), token.size());
  }), tab);
  if (row) g_signal_connect(widget, "drag-failed", G_CALLBACK(+[](GtkWidget *source, GdkDragContext *ctx,
      GtkDragResult reason, gpointer context) -> gboolean {
    if (reason != GTK_DRAG_RESULT_NO_TARGET || gdk_drag_context_get_protocol(ctx) != GDK_DRAG_PROTO_XDND) return TRUE;
    auto *tab = static_cast<TerminalTab *>(context);
    auto *native = gtk_widget_get_window(tab->tray->window);
    int x; int y;
    gdk_window_get_device_position(native, gdk_drag_context_get_device(ctx), &x, &y, nullptr);
    if (x < 0 || y < 0 || x >= gdk_window_get_width(native) || y >= gdk_window_get_height(native))
      deferred_undock(source);
    return TRUE;
  }), tab);
}

static GtkWidget *dock_handle(GtkWidget *child, TerminalTab *tab) {
  auto *button = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(button), FALSE);
  gtk_event_box_set_above_child(GTK_EVENT_BOX(button), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(button), "terminal-dock-handle");
  gtk_widget_set_tooltip_text(button, _("Drag to dock all tabs in another terminal window"));
  // The label or icon is already owned by its builder or parent.
  g_object_ref(child);
  unparent(child);
  gtk_container_add(GTK_CONTAINER(button), child);
  g_object_unref(child);
  g_signal_connect(button, "button-press-event", G_CALLBACK(+[](GtkWidget *handle,
      GdkEventButton *event, gpointer) -> gboolean {
    return event->button == GDK_BUTTON_PRIMARY && begin_window_header_resize(gtk_widget_get_toplevel(handle), event);
  }), nullptr);
  bind_source(button, tab, false);
  gtk_widget_show_all(button);
  return button;
}

TerminalTab *create_terminal_tab(MainWindow *page, std::string label, TerminalTabActions actions) {
  auto *tab = new TerminalTab{page, std::move(actions)};
  auto *window = page->window;
  // Keep the complete per-session chrome and page alive across container changes.
  g_object_ref(page->header_bar);
  g_object_ref(page->root_box);
  gtk_window_set_titlebar(GTK_WINDOW(window), nullptr);
  unparent(page->root_box);
  auto *tray = make_tray(window);
  install_drop(page->terminal_attachment);
  if (!tray->native_title_drag) {
    auto *handle = dock_handle(page->header_connection_icon, tab);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(page->header_bar), handle);
  }
  tab->row = gtk_list_box_row_new();
  g_object_ref_sink(tab->row);
  auto *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  tab->pick = gtk_button_new();
  auto *caption = gtk_label_new(label.c_str());
  gtk_label_set_xalign(GTK_LABEL(caption), 0);
  gtk_label_set_ellipsize(GTK_LABEL(caption), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(caption), 12);
  auto *contents = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  auto *icon = gtk_image_new();
  g_object_bind_property(page->header_connection_icon, "surface", icon, "surface", G_BINDING_SYNC_CREATE);
  gtk_box_pack_start(GTK_BOX(contents), icon, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(contents), caption, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(tab->pick), contents);
  auto *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
  gtk_widget_set_tooltip_text(close, _("Close"));
  gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
  gtk_button_set_relief(GTK_BUTTON(tab->pick), GTK_RELIEF_NONE);
  gtk_style_context_add_class(gtk_widget_get_style_context(close), "close-tab");
  gtk_box_pack_start(GTK_BOX(line), tab->pick, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(line), close, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(tab->row), line);
  tab->gesture = tearoff_gesture_bind(tab->pick, deferred_undock, tab->pick);
  bind_source(tab->pick, tab, true);
  g_signal_connect_swapped(tab->pick, "clicked", G_CALLBACK(select_terminal_tab), tab);
  g_signal_connect(close, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
    static_cast<TerminalTab *>(data)->actions.close();
  }), tab);
  static GtkCssProvider *style = nullptr;
  if (!style) {
    style = gtk_css_provider_new();
    gtk_css_provider_load_from_data(style,
      ".terminal-tray-headers.titlebar {padding:0; margin:0; border:0; min-width:0; min-height:0; background:none;}"
      ".terminal-dock-handle {padding:0; border:0; min-width:0; min-height:0; background:none; box-shadow:none;}"
      "row .close-tab {opacity:0; padding:2px;} row:hover .close-tab, row .close-tab:focus {opacity:1;}", -1, nullptr);
    gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(window),
        GTK_STYLE_PROVIDER(style), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
  gtk_widget_show_all(tab->row);
  gtk_widget_show(page->header_bar);
  gtk_widget_show(page->root_box);
  add_page(tray, tab);
  tray->active = tab;
  refresh(tray);
  update_drags();
  return tab;
}
void destroy_terminal_tab(TerminalTab *tab) {
  if (!tab) return;
  auto *tray = tab->tray;
  remove_page(tab);
  g_object_set_data(G_OBJECT(tab->pick), "terminal-drag-tab", nullptr);
  g_object_unref(tab->row);
  refresh(tray);
  update_drags();
  g_object_unref(tab->page->root_box);
  g_object_unref(tab->page->header_bar);
  delete tab;
}
}

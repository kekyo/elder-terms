// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "tray-backend.h"

#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <cairo.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static GtkWidget *find_connections_item(GtkWidget *widget, const char *name) {
  if (GTK_IS_MENU_ITEM(widget)) {
    const char *label = gtk_menu_item_get_label(GTK_MENU_ITEM(widget));
    if (label != nullptr && std::strcmp(label, name) == 0)
      return widget;
  }
  if (!GTK_IS_CONTAINER(widget)) return nullptr;
  GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
  GtkWidget *result = nullptr;
  for (GList *child = children; child != nullptr && result == nullptr;
       child = child->next)
    result = find_connections_item(GTK_WIDGET(child->data), name);
  g_list_free(children);
  return result;
}

static GtkWidget *find_connections_item(const char *name = "Connections") {
  GList *windows = gtk_window_list_toplevels();
  GtkWidget *result = nullptr;
  for (GList *window = windows; window != nullptr && result == nullptr;
       window = window->next)
    result = find_connections_item(GTK_WIDGET(window->data), name);
  g_list_free(windows);
  return result;
}

static std::vector<GtkWidget *> submenu_items(GtkWidget *connections_item) {
  GtkWidget *submenu = gtk_menu_item_get_submenu(
      GTK_MENU_ITEM(connections_item));
  require(GTK_IS_MENU(submenu), "Connections must own a GTK submenu");
  GList *children = gtk_container_get_children(GTK_CONTAINER(submenu));
  std::vector<GtkWidget *> result;
  for (GList *child = children; child != nullptr; child = child->next)
    result.push_back(GTK_WIDGET(child->data));
  g_list_free(children);
  return result;
}

struct Row {
  GtkWidget *image;
  GtkWidget *label;
};

static Row row_widgets(GtkWidget *menu_item) {
  GtkWidget *row = gtk_bin_get_child(GTK_BIN(menu_item));
  require(GTK_IS_BOX(row), "A connection item must contain a row");
  GList *children = gtk_container_get_children(GTK_CONTAINER(row));
  require(g_list_length(children) == 2,
          "A connection row must show an image and label");
  auto *image = GTK_WIDGET(children->data);
  auto *label = GTK_WIDGET(children->next->data);
  g_list_free(children);
  require(GTK_IS_IMAGE(image) && GTK_IS_LABEL(label),
          "A connection row must use GTK image and label widgets");
  return {image, label};
}

struct Availability {
  bool changed = false;
  std::optional<cardio::promise_source<void>> event;
};

static cardio::promise<void> wait_for_availability_async(
    Availability &availability) {
  while (!availability.changed) {
    availability.event.emplace();
    co_await availability.event->get_promise();
    availability.event.reset();
  }
}

static cardio::promise<void> exercise_async(
    GApplication *application, cardio::dispatcher &dispatcher,
    cardio::dispatcher_group_glib &group, std::exception_ptr &failure) {
  elder_terms::TrayBackendState *backend = nullptr;
  try {
    Availability availability;
    std::vector<std::string> opened;
    std::vector<std::string> presented;
    backend = elder_terms::create_tray_backend({
        .application = application,
        .dispatcher = &dispatcher,
        .identifier = "elder-terms-test",
        .title = "elder-terms test",
        .icon_name = "elder-terms",
        .callbacks = {
            .activate = {},
            .application_settings = {},
            .about = {},
            .quit = {},
            .open_connection = [&opened](
                const std::string &key,
                const elder_terms::TrayActivationContext &) {
              opened.push_back(key);
            },
            .show_window = [&presented](const std::string &key, const elder_terms::TrayActivationContext &) { presented.push_back(key); },
            .availability_changed = [&availability](
                elder_terms::TrayBackendAvailabilityState) {
              availability.changed = true;
              if (availability.event.has_value())
                (void)availability.event->try_resolve();
            },
        },
    });
    co_await wait_for_availability_async(availability);
    require(elder_terms::tray_backend_kind(backend) ==
                elder_terms::TrayBackendKind::xembed,
            "X11 without a StatusNotifier watcher must use XEmbed");

    elder_terms::update_tray_connections(backend, {
        {"alpha", "Alpha_under", "Ctrl+Alt+X", "default"},
        {"beta", "Beta", "", "default"},
    });
    GtkWidget *connections_item = find_connections_item();
    require(connections_item != nullptr,
            "The XEmbed tray menu must contain Connections");
    auto items = submenu_items(connections_item);
    require(items.size() == 2, "The submenu must show both connections");
    Row first = row_widgets(items.front());
    require(std::strcmp(gtk_label_get_text(GTK_LABEL(first.label)),
                        "Alpha_under (Ctrl+Alt+X)") == 0,
            "GTK must display the literal name and shortcut");
    require(gtk_image_get_storage_type(GTK_IMAGE(first.image)) ==
                GTK_IMAGE_SURFACE,
            "A default connection icon must be visible immediately");
    cairo_surface_t *surface = nullptr;
    g_object_get(G_OBJECT(first.image), "surface", &surface, nullptr);
    require(surface != nullptr, "The GTK image must expose its surface");
    double x_scale = 0.0;
    double y_scale = 0.0;
    cairo_surface_get_device_scale(surface, &x_scale, &y_scale);
    require(x_scale == gtk_widget_get_scale_factor(first.image) &&
                y_scale == x_scale,
            "The GTK icon surface must match the display scale");
    if (g_strcmp0(g_getenv("GDK_SCALE"), "2") == 0)
      require(x_scale == 2.0,
              "A 2x display must receive a 2x icon surface");
    cairo_surface_destroy(surface);
    gtk_menu_item_activate(GTK_MENU_ITEM(items.front()));
    require(opened.size() == 1 && opened.front() == "alpha",
            "Selecting the first submenu item must open its connection");

    GtkWidget *submenu = gtk_menu_item_get_submenu(
        GTK_MENU_ITEM(connections_item));
    GtkWidget *anchor = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_show(anchor);
    gtk_menu_popup_at_widget(GTK_MENU(submenu), anchor,
                             GDK_GRAVITY_SOUTH_WEST,
                             GDK_GRAVITY_NORTH_WEST, nullptr);
    require(gtk_widget_get_mapped(submenu),
            "The submenu must be displayed for live-update verification");
    elder_terms::update_tray_connections(backend, {
        {"gamma", "Gamma", "", "default"},
    });
    items = submenu_items(connections_item);
    require(gtk_widget_get_mapped(submenu) && items.size() == 1 &&
                std::strcmp(gtk_label_get_text(
                                GTK_LABEL(row_widgets(items.front()).label)),
                            "Gamma") == 0,
            "A displayed GTK submenu must reflect a replacement snapshot");
    gtk_menu_item_activate(GTK_MENU_ITEM(items.front()));
    require(opened.size() == 2 && opened.back() == "gamma",
            "A refreshed GTK submenu must open its current key");

    elder_terms::update_tray_connections(backend, {});
    items = submenu_items(connections_item);
    require(items.size() == 1 &&
                !gtk_widget_get_sensitive(items.front()) &&
                std::strcmp(gtk_menu_item_get_label(
                                GTK_MENU_ITEM(items.front())),
                            "No saved connections") == 0,
            "An empty GTK submenu must show a disabled explanation");
    GtkWidget *windows_item = find_connections_item("Open windows");
    require(windows_item != nullptr, "The XEmbed menu must contain Open windows");
    elder_terms::update_tray_windows(backend, {{"launch-a", "Alpha (Terminal)", "default", false}});
    auto windows = submenu_items(windows_item);
    require(windows.size() == 1 && !gtk_widget_get_sensitive(windows.front()), "An unready window cannot be presented");
    gtk_menu_item_activate(GTK_MENU_ITEM(windows.front()));
    require(presented.empty(), "An unready window must ignore activation");
    elder_terms::update_tray_windows(backend, {{"launch-a", "Alpha (Terminal)", "default", true}});
    windows = submenu_items(windows_item);
    require(gtk_widget_get_sensitive(windows.front()), "Ready windows can be presented");
    require(gtk_image_get_storage_type(GTK_IMAGE(row_widgets(windows.front()).image)) == GTK_IMAGE_SURFACE, "Tracked windows have connection icons");
    gtk_menu_item_activate(GTK_MENU_ITEM(windows.front()));
    require(presented == std::vector<std::string>{"launch-a"}, "Presentation uses the launch identity");
    elder_terms::update_tray_windows(backend, {});
    windows = submenu_items(windows_item);
    require(windows.size() == 1 && !gtk_widget_get_sensitive(windows.front()), "Exited windows leave a disabled empty state");
    gtk_menu_popdown(GTK_MENU(submenu));
    gtk_widget_destroy(anchor);
    elder_terms::destroy_tray_backend(backend);
    backend = nullptr;
  } catch (...) {
    failure = std::current_exception();
    elder_terms::destroy_tray_backend(backend);
  }
  group.shutdown();
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  GApplication *application = g_application_new(
      "net.kekyo.elder-terms.XEmbedTest", G_APPLICATION_DEFAULT_FLAGS);
  GError *error = nullptr;
  if (!g_application_register(application, nullptr, &error)) {
    std::cerr << error->message << '\n';
    g_error_free(error);
    g_object_unref(application);
    g_test_dbus_down(bus);
    g_object_unref(bus);
    return 1;
  }
  std::exception_ptr failure;
  {
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib_auto dispatcher(group);
    auto task = exercise_async(application, dispatcher, group, failure);
    dispatcher.park();
  }
  g_object_unref(application);
  g_test_dbus_down(bus);
  g_object_unref(bus);
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &exception) {
      std::cerr << exception.what() << '\n';
      return 1;
    }
  }
  return 0;
}

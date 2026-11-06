// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "tray-backend.h"

#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gio/gio.h>

static constexpr char watcher_name[] = "org.kde.StatusNotifierWatcher";
static constexpr char watcher_path[] = "/StatusNotifierWatcher";
static constexpr char watcher_xml[] = R"XML(
<node><interface name="org.kde.StatusNotifierWatcher">
  <method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
  <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
  <signal name="StatusNotifierHostRegistered"/>
  <signal name="StatusNotifierHostUnregistered"/>
</interface></node>
)XML";

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

struct Watcher {
  GDBusConnection *connection = nullptr;
  guint object_id = 0;
  bool host_registered = false;
  bool hold_registration = false;
  GDBusMethodInvocation *pending_registration = nullptr;
  std::vector<std::string> items;
  cardio::promise_source<void> host_queried;
  cardio::promise_source<void> registration_requested;

  ~Watcher() {
    if (pending_registration != nullptr) {
      g_dbus_method_invocation_return_value(pending_registration, nullptr);
      g_object_unref(pending_registration);
    }
    if (object_id != 0) {
      g_dbus_connection_unregister_object(connection, object_id);
    }
    g_clear_object(&connection);
  }
};

static void on_register_item(GDBusConnection *, const gchar *sender,
                             const gchar *, const gchar *, const gchar *,
                             GVariant *parameters,
                             GDBusMethodInvocation *invocation,
                             gpointer user_data) {
  auto *watcher = static_cast<Watcher *>(user_data);
  const char *path = nullptr;
  g_variant_get(parameters, "(&s)", &path);
  watcher->items.push_back(std::string(sender) + path);
  if (watcher->hold_registration) {
    watcher->pending_registration =
        G_DBUS_METHOD_INVOCATION(g_object_ref(invocation));
  } else {
    g_dbus_method_invocation_return_value(invocation, nullptr);
  }
  (void)watcher->registration_requested.try_resolve();
}

static GVariant *on_get_host(GDBusConnection *, const gchar *, const gchar *,
                             const gchar *, const gchar *, GError **,
                             gpointer user_data) {
  auto *watcher = static_cast<Watcher *>(user_data);
  (void)watcher->host_queried.try_resolve();
  return g_variant_new_boolean(watcher->host_registered);
}

static std::unique_ptr<Watcher> create_watcher(const char *address,
                                              bool host_registered) {
  auto watcher = std::make_unique<Watcher>();
  watcher->host_registered = host_registered;
  GError *error = nullptr;
  watcher->connection = g_dbus_connection_new_for_address_sync(
      address,
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, &error);
  if (error != nullptr) {
    const std::string message = error->message;
    g_error_free(error);
    throw std::runtime_error(message);
  }
  GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(watcher_xml, nullptr);
  const GDBusInterfaceVTable vtable = {
      on_register_item, on_get_host, nullptr, {nullptr},
  };
  watcher->object_id = g_dbus_connection_register_object(
      watcher->connection, watcher_path, info->interfaces[0], &vtable,
      watcher.get(), nullptr, nullptr);
  g_dbus_node_info_unref(info);
  require(watcher->object_id != 0, "Cannot export test watcher");
  return watcher;
}

static cardio::promise<GVariant *> call_async(
    GDBusConnection *connection, const char *destination, const char *path,
    const char *interface_name, const char *method, GVariant *parameters) {
  co_return co_await cardio::gio::submit<GVariant *>(
      [=](GCancellable *cancellable, GAsyncReadyCallback callback,
          gpointer user_data) {
        g_dbus_connection_call(connection, destination, path, interface_name,
                               method, parameters, nullptr,
                               G_DBUS_CALL_FLAGS_NONE, -1, cancellable,
                               callback, user_data);
      },
      [connection](GObject *, GAsyncResult *result, GError **error) {
        return g_dbus_connection_call_finish(connection, result, error);
      });
}

static int find_layout_id(GVariant *node, const std::string &label) {
  gint id = -1;
  GVariant *properties = nullptr;
  GVariant *children = nullptr;
  g_variant_get(node, "(i@a{sv}@av)", &id, &properties, &children);
  const char *actual = nullptr;
  const bool found = g_variant_lookup(properties, "label", "&s", &actual) &&
                     label == actual;
  g_variant_unref(properties);
  if (found) {
    g_variant_unref(children);
    return id;
  }
  const auto length = g_variant_n_children(children);
  for (gsize index = 0; index < length; ++index) {
    GVariant *wrapped = g_variant_get_child_value(children, index);
    GVariant *child = g_variant_get_variant(wrapped);
    const int result = find_layout_id(child, label);
    g_variant_unref(child);
    g_variant_unref(wrapped);
    if (result >= 0) {
      g_variant_unref(children);
      return result;
    }
  }
  g_variant_unref(children);
  return -1;
}

static cardio::promise<void> claim_watcher_async(Watcher &watcher) {
  GVariant *reply = co_await call_async(
      watcher.connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", watcher_name,
          1U | 2U | 4U)); // Allow and request replacement without queuing.
  guint result = 0;
  g_variant_get(reply, "(u)", &result);
  g_variant_unref(reply);
  require(result == 1U, "Test watcher must own its name");
}

static cardio::promise<void> release_watcher_async(Watcher &watcher) {
  GVariant *reply = co_await call_async(
      watcher.connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "ReleaseName",
      g_variant_new("(s)", watcher_name));
  g_variant_unref(reply);
}

struct Availability {
  elder_terms::TrayBackendAvailabilityState current =
      elder_terms::TrayBackendAvailabilityState::pending;
  std::optional<cardio::promise_source<void>> changed;
};

struct MenuSignals {
  int updates = 0;
  std::optional<cardio::promise_source<void>> changed;
};

static void on_menu_layout_updated(GDBusConnection *, const gchar *,
                                   const gchar *, const gchar *,
                                   const gchar *, GVariant *,
                                   gpointer user_data) {
  auto *signals = static_cast<MenuSignals *>(user_data);
  ++signals->updates;
  if (signals->changed.has_value())
    (void)signals->changed->try_resolve();
}

static cardio::promise<void> wait_for_menu_updates_async(
    MenuSignals &signals, int expected) {
  while (signals.updates < expected) {
    signals.changed.emplace();
    co_await signals.changed->get_promise();
    signals.changed.reset();
  }
}

static cardio::promise<void> wait_for_availability_async(
    Availability &availability, elder_terms::TrayBackendAvailabilityState expected) {
  while (availability.current != expected) {
    availability.changed.emplace();
    co_await availability.changed->get_promise();
  }
}

static void set_host_registered(Watcher &watcher, bool registered,
                                 bool properties_signal) {
  watcher.host_registered = registered;
  if (properties_signal) {
    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&changed, "{sv}", "IsStatusNotifierHostRegistered",
                          g_variant_new_boolean(registered));
    g_dbus_connection_emit_signal(
        watcher.connection, nullptr, watcher_path,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(sa{sv}as)", watcher_name, &changed, nullptr), nullptr);
  } else {
    g_dbus_connection_emit_signal(watcher.connection, nullptr, watcher_path,
        watcher_name, registered ? "StatusNotifierHostRegistered"
                                 : "StatusNotifierHostUnregistered",
        nullptr, nullptr);
  }
}

static cardio::promise<void> exercise_async(
    std::string scenario, GApplication *application,
    cardio::dispatcher &dispatcher, const char *address,
    cardio::dispatcher_group_glib &group, std::exception_ptr &failure) {
  using State = elder_terms::TrayBackendAvailabilityState;
  Availability availability;
  elder_terms::TrayBackendState *backend = nullptr;
  std::vector<std::string> opened_connections;
  try {
    auto first = create_watcher(address, scenario != "late-host");
    if (scenario != "late-watcher" && scenario != "destroy-waiting") {
      co_await claim_watcher_async(*first);
    }
    backend = elder_terms::create_tray_backend({
        .application = application,
        .dispatcher = &dispatcher,
        .identifier = "elder-terms",
        .title = "elder-terms",
        .icon_name = "elder-terms",
        .callbacks = {
            .activate = {}, .application_settings = {}, .about = {}, .quit = {},
            .open_connection = [&opened_connections](
                const std::string &key,
                const elder_terms::TrayActivationContext &) {
              opened_connections.push_back(key);
            },
            .availability_changed = [&availability](State value) {
              availability.current = value;
              if (availability.changed.has_value()) {
                (void)availability.changed->try_resolve();
              }
            },
        },
    });

    if (scenario == "late-watcher" || scenario == "destroy-waiting") {
      co_await wait_for_availability_async(availability, State::unavailable);
      if (scenario == "destroy-waiting") {
        elder_terms::destroy_tray_backend(backend);
        backend = nullptr;
      }
      co_await claim_watcher_async(*first);
    } else if (scenario == "late-host") {
      co_await first->host_queried.get_promise();
      co_await wait_for_availability_async(availability, State::unavailable);
      require(first->items.empty(), "An unready host must not receive an item");
      set_host_registered(*first, true, false);
    }

    if (backend != nullptr) {
      std::cerr << "Waiting for tray registration: " << scenario << '\n';
      co_await wait_for_availability_async(availability, State::available);
      require(first->items.size() == 1, "The tray must register once with the host");
      require(elder_terms::tray_backend_kind(backend) ==
                  elder_terms::TrayBackendKind::status_notifier_item,
              "The recovered tray must use StatusNotifierItem");
    }

    if (scenario == "connections") {
      elder_terms::update_tray_connections(backend, {
          {"first", "First_connection", "Ctrl+Alt+X", "default"},
          {"second", "Second", "", "default"},
      });
      const char *destination = g_dbus_connection_get_unique_name(
          g_application_get_dbus_connection(application));
      GVariant *layout = co_await call_async(
          first->connection, destination, "/StatusNotifierMenu",
          "com.canonical.dbusmenu", "GetLayout",
          g_variant_new("(ii@as)", 0, -1,
                        g_variant_new_strv(nullptr, 0)));
      const std::string printed = g_variant_print(layout, TRUE);
      require(printed.find("Connections") != std::string::npos,
              "The menu must have a Connections submenu");
      require(printed.find("First__connection (Ctrl+Alt+X)") !=
                  std::string::npos,
              "Connection names must remain literal and show shortcuts");
      require(printed.find("Second") != std::string::npos,
              "The submenu must show every connection");
      guint revision = 0;
      GVariant *root = nullptr;
      g_variant_get(layout, "(u@(ia{sv}av))", &revision, &root);
      const int first_id = find_layout_id(
          root, "First__connection (Ctrl+Alt+X)");
      require(first_id > 0, "The first connection must have an ID");
      g_variant_unref(root);
      g_variant_unref(layout);

      const gchar *label_property[] = {"label"};
      GVariant *filtered = co_await call_async(
          first->connection, destination, "/StatusNotifierMenu",
          "com.canonical.dbusmenu", "GetLayout",
          g_variant_new("(ii@as)", 5, 0,
                        g_variant_new_strv(label_property, 1)));
      GVariant *submenu = nullptr;
      g_variant_get(filtered, "(u@(ia{sv}av))", &revision, &submenu);
      gint submenu_id = 0;
      GVariant *submenu_properties = nullptr;
      GVariant *submenu_children = nullptr;
      g_variant_get(submenu, "(i@a{sv}@av)", &submenu_id,
                    &submenu_properties, &submenu_children);
      const char *submenu_label = nullptr;
      require(submenu_id == 5 &&
                  g_variant_lookup(submenu_properties, "label", "&s",
                                   &submenu_label) &&
                  std::strcmp(submenu_label, "Connections") == 0 &&
                  g_variant_n_children(submenu_children) == 0 &&
                  g_variant_lookup_value(submenu_properties, "enabled",
                                         nullptr) == nullptr,
              "GetLayout must honor parent, depth, and property filter");
      g_variant_unref(submenu_children);
      g_variant_unref(submenu_properties);
      g_variant_unref(submenu);
      g_variant_unref(filtered);

      const gint32 requested_id = first_id;
      const gchar *icon_property[] = {"icon-data"};
      GVariant *group = co_await call_async(
          first->connection, destination, "/StatusNotifierMenu",
          "com.canonical.dbusmenu", "GetGroupProperties",
          g_variant_new("(@ai@as)",
              g_variant_new_fixed_array(G_VARIANT_TYPE_INT32,
                                        &requested_id, 1, sizeof(requested_id)),
              g_variant_new_strv(icon_property, 1)));
      GVariant *group_items = nullptr;
      g_variant_get(group, "(@a(ia{sv}))", &group_items);
      require(g_variant_n_children(group_items) == 1,
              "GetGroupProperties must include a requested connection");
      GVariant *group_item = g_variant_get_child_value(group_items, 0);
      gint group_id = 0;
      GVariant *group_properties = nullptr;
      g_variant_get(group_item, "(i@a{sv})", &group_id, &group_properties);
      GVariant *png = g_variant_lookup_value(group_properties, "icon-data",
                                               G_VARIANT_TYPE("ay"));
      gsize png_size = 0;
      const auto *png_bytes = png == nullptr ? nullptr :
          static_cast<const guint8 *>(g_variant_get_fixed_array(png, &png_size, 1));
      require(group_id == first_id && png_bytes != nullptr && png_size > 8 &&
                  std::memcmp(png_bytes, "\x89PNG\r\n\x1a\n", 8) == 0,
              "Connection icon-data must be PNG bytes");
      const std::vector<guint8> default_png(png_bytes,
                                             png_bytes + png_size);
      g_variant_unref(png);
      g_variant_unref(group_properties);
      g_variant_unref(group_item);
      g_variant_unref(group_items);
      g_variant_unref(group);

      // An unrelated refresh may arrive after the host reads the menu and
      // before it dispatches the user's selection.
      elder_terms::update_tray_connections(backend, {
          {"first", "First_connection", "Ctrl+Alt+X", "default"},
          {"second", "Renamed second", "", "default"},
      });
      GVariant *event = co_await call_async(
          first->connection, destination, "/StatusNotifierMenu",
          "com.canonical.dbusmenu", "Event",
          g_variant_new("(isvu)", first_id, "clicked",
                        g_variant_new_string(""), 42U));
      g_variant_unref(event);
      require(opened_connections.size() == 1 &&
                  opened_connections.front() == "first",
              "Clicking a connection must open the matching key");

      elder_terms::update_tray_connections(backend, {
          {"third", "Third", "", "default"},
      });
      layout = co_await call_async(
          first->connection, destination, "/StatusNotifierMenu",
          "com.canonical.dbusmenu", "GetLayout",
          g_variant_new("(ii@as)", 0, -1,
                        g_variant_new_strv(nullptr, 0)));
      guint newer_revision = 0;
      g_variant_get(layout, "(u@(ia{sv}av))", &newer_revision, &root);
      require(newer_revision > revision &&
                  find_layout_id(root, "Third") > first_id &&
                  find_layout_id(root, "First__connection (Ctrl+Alt+X)") < 0,
              "Replacing connections must revise the menu without reusing IDs");
      g_variant_unref(root);
      g_variant_unref(layout);
      event = co_await call_async(
          first->connection, destination, "/StatusNotifierMenu",
          "com.canonical.dbusmenu", "Event",
          g_variant_new("(isvu)", first_id, "clicked",
                        g_variant_new_string(""), 43U));
      g_variant_unref(event);
      require(opened_connections.size() == 1,
              "A stale menu ID must never open another connection");

      MenuSignals signals;
      const guint subscription = g_dbus_connection_signal_subscribe(
          first->connection, destination, "com.canonical.dbusmenu",
          "LayoutUpdated", "/StatusNotifierMenu", nullptr,
          G_DBUS_SIGNAL_FLAGS_NONE, on_menu_layout_updated,
          &signals, nullptr);
      GVariant *barrier = co_await call_async(
          first->connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
          "org.freedesktop.DBus", "GetId", nullptr);
      g_variant_unref(barrier);
      elder_terms::update_tray_connections(backend, {
          {"emoji", "Emoji icon", "", "noto:emoji_u1f44d"},
      });
      co_await wait_for_menu_updates_async(signals, 2);
      g_dbus_connection_signal_unsubscribe(first->connection, subscription);
      layout = co_await call_async(
          first->connection, destination, "/StatusNotifierMenu",
          "com.canonical.dbusmenu", "GetLayout",
          g_variant_new("(ii@as)", 0, -1,
                        g_variant_new_strv(nullptr, 0)));
      g_variant_get(layout, "(u@(ia{sv}av))", &newer_revision, &root);
      const int emoji_id = find_layout_id(root, "Emoji icon");
      require(emoji_id > first_id,
              "The loaded preset must remain in the current menu");
      g_variant_unref(root);
      g_variant_unref(layout);
      const gint32 requested_emoji = emoji_id;
      group = co_await call_async(
          first->connection, destination, "/StatusNotifierMenu",
          "com.canonical.dbusmenu", "GetGroupProperties",
          g_variant_new("(@ai@as)",
              g_variant_new_fixed_array(G_VARIANT_TYPE_INT32,
                                        &requested_emoji, 1,
                                        sizeof(requested_emoji)),
              g_variant_new_strv(icon_property, 1)));
      g_variant_get(group, "(@a(ia{sv}))", &group_items);
      group_item = g_variant_get_child_value(group_items, 0);
      g_variant_get(group_item, "(i@a{sv})", &group_id,
                    &group_properties);
      png = g_variant_lookup_value(group_properties, "icon-data",
                                   G_VARIANT_TYPE("ay"));
      png_bytes = png == nullptr ? nullptr :
          static_cast<const guint8 *>(g_variant_get_fixed_array(png, &png_size, 1));
      require(group_id == emoji_id && png_bytes != nullptr &&
                  std::vector<guint8>(png_bytes, png_bytes + png_size) !=
                      default_png,
              "An asynchronously loaded preset must replace the default PNG");
      g_variant_unref(png);
      g_variant_unref(group_properties);
      g_variant_unref(group_item);
      g_variant_unref(group_items);
      g_variant_unref(group);
    }

    if (scenario == "restart") {
      elder_terms::update_tray_connections(backend, {
          {"retained", "Retained connection", "Ctrl+Alt+R", "default"},
      });
      co_await release_watcher_async(*first);
      co_await wait_for_availability_async(availability, State::unavailable);
      auto second = create_watcher(address, true);
      co_await claim_watcher_async(*second);
      co_await wait_for_availability_async(availability, State::available);
      require(second->items.size() == 1, "The restarted host must receive the item");
      const char *destination = g_dbus_connection_get_unique_name(
          g_application_get_dbus_connection(application));
      GVariant *layout = co_await call_async(
          second->connection, destination, "/StatusNotifierMenu",
          "com.canonical.dbusmenu", "GetLayout",
          g_variant_new("(ii@as)", 0, -1,
                        g_variant_new_strv(nullptr, 0)));
      GVariant *root = nullptr;
      guint revision = 0;
      g_variant_get(layout, "(u@(ia{sv}av))", &revision, &root);
      require(find_layout_id(root, "Retained connection (Ctrl+Alt+R)") > 0,
              "A returning host must receive the current connection menu");
      g_variant_unref(root);
      g_variant_unref(layout);
    } else if (scenario == "host-signals" || scenario == "host-properties") {
      const bool properties = scenario == "host-properties";
      set_host_registered(*first, false, properties);
      co_await wait_for_availability_async(availability, State::unavailable);
      set_host_registered(*first, true, properties);
      co_await wait_for_availability_async(availability, State::available);
      require(first->items.size() == 2, "The returning host must receive the item");
    } else if (scenario == "replace-registering" || scenario == "destroy-registering") {
      // Restart discovery with its registration reply held by the old owner.
      co_await release_watcher_async(*first);
      co_await wait_for_availability_async(availability, State::unavailable);
      first->hold_registration = true;
      first->registration_requested = cardio::promise_source<void>();
      co_await claim_watcher_async(*first);
      co_await first->registration_requested.get_promise();
      require(availability.current != State::available,
              "A held registration must not mark the tray available");
      if (scenario == "destroy-registering") {
        elder_terms::destroy_tray_backend(backend);
        backend = nullptr;
      }
      auto second = create_watcher(address, true);
      co_await claim_watcher_async(*second);
      if (backend != nullptr) {
        co_await wait_for_availability_async(availability, State::available);
        require(second->items.size() == 1,
                "A replacement must register despite the old held reply");
      }
      g_dbus_method_invocation_return_value(first->pending_registration, nullptr);
      g_clear_object(&first->pending_registration);
      // A round trip ensures the old reply is delivered before test teardown.
      GVariant *reply = co_await call_async(first->connection,
          "org.freedesktop.DBus", "/org/freedesktop/DBus",
          "org.freedesktop.DBus", "GetId", nullptr);
      g_variant_unref(reply);
      require(backend == nullptr ||
                  elder_terms::tray_backend_availability(backend) == State::available,
              "The old reply must not overwrite the replacement's state");
    }

    elder_terms::destroy_tray_backend(backend);
    backend = nullptr;
    co_await release_watcher_async(*first);
  } catch (...) {
    failure = std::current_exception();
    elder_terms::destroy_tray_backend(backend);
  }
  group.shutdown();
}

int main(int argc, char **argv) {
  if (argc != 2) return 1;
  GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  GApplication *application = g_application_new(
      "net.kekyo.elder-terms.TrayTest", G_APPLICATION_DEFAULT_FLAGS);
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
    auto task = exercise_async(argv[1], application, dispatcher,
        g_test_dbus_get_bus_address(bus), group, failure);
    dispatcher.park();
  }
  g_object_unref(application);
  g_test_dbus_down(bus);
  g_object_unref(bus);
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &exception) {
      std::cerr << exception.what() << '\n';
    }
    return 1;
  }
  std::cout << "tray watcher lifecycle: " << argv[1] << " PASS\n";
  return 0;
}

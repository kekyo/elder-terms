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

    if (scenario == "restart") {
      co_await release_watcher_async(*first);
      co_await wait_for_availability_async(availability, State::unavailable);
      auto second = create_watcher(address, true);
      co_await claim_watcher_async(*second);
      co_await wait_for_availability_async(availability, State::available);
      require(second->items.size() == 1, "The restarted host must receive the item");
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

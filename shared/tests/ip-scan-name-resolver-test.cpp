#include "../src/ip-scan-name-resolver.h"

#include <sys/socket.h>

#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace elder_terms_ip_scan_name_resolver_test {

static constexpr const char *service_name = "org.freedesktop.resolve1";
static constexpr const char *object_path = "/org/freedesktop/resolve1";
static constexpr const char *interface_xml = R"xml(
<node><interface name="org.freedesktop.resolve1.Manager">
  <method name="ResolveAddress">
    <arg type="i" direction="in"/><arg type="i" direction="in"/>
    <arg type="ay" direction="in"/><arg type="t" direction="in"/>
    <arg type="a(is)" direction="out"/><arg type="t" direction="out"/>
  </method>
</interface></node>)xml";

static void expect(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct ServiceState {
  bool valid_arguments = true;
  std::vector<guint64> protocols;
  std::vector<std::string> senders;
  std::string error_name;
  cardio::cancellation_source *cancel_when_called = nullptr;
  GDBusMethodInvocation *pending = nullptr;

  ~ServiceState() {
    if (pending != nullptr) {
      g_dbus_method_invocation_return_dbus_error(
          pending, "org.freedesktop.resolve1.NoSuchRR", "test complete");
    }
  }
};

static void handle_method(GDBusConnection *, const gchar *sender,
                           const gchar *, const gchar *, const gchar *,
                           GVariant *parameters,
                           GDBusMethodInvocation *invocation, gpointer data) {
  auto *state = static_cast<ServiceState *>(data);
  gint32 interface_index = -1;
  gint32 family = -1;
  guint64 flags = 0;
  GVariantIter *raw_bytes = nullptr;
  g_variant_get(parameters, "(iiayt)", &interface_index, &family, &raw_bytes,
                &flags);
  auto bytes = std::unique_ptr<GVariantIter, decltype(&g_variant_iter_free)>(
      raw_bytes, g_variant_iter_free);
  std::vector<guint8> address;
  guint8 byte = 0;
  while (g_variant_iter_next(bytes.get(), "y", &byte)) {
    address.push_back(byte);
  }
  state->valid_arguments = state->valid_arguments && interface_index == 0 &&
      family == AF_INET && address == std::vector<guint8>({192, 0, 2, 25}) &&
      (flags == 2 || flags == 8);
  state->protocols.push_back(flags);
  state->senders.emplace_back(sender);
  if (state->cancel_when_called != nullptr) {
    // The invocation is owned until a reply is returned after cancellation.
    state->pending = invocation;
    (void)state->cancel_when_called->cancel();
    return;
  }
  if (!state->error_name.empty()) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, state->error_name.c_str(), "injected lookup failure");
    return;
  }
  GVariantBuilder names;
  g_variant_builder_init(&names, G_VARIANT_TYPE("a(is)"));
  g_variant_builder_add(&names, "(is)", 3,
                        flags == 8 ? "other.local" : "other");
  g_variant_builder_add(&names, "(is)", 2,
                        flags == 8 ? "printer.local" : "printer");
  g_dbus_method_invocation_return_value(
      invocation, g_variant_new("(a(is)t)", &names, flags));
}

static cardio::promise<void> change_name_owner_async(
    GDBusConnection *connection, bool acquire) {
  auto *reply = co_await cardio::gio::submit<GVariant *>(
      [connection, acquire](GCancellable *cancellation,
                             GAsyncReadyCallback callback, gpointer data) {
        g_dbus_connection_call(
            connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", acquire ? "RequestName" : "ReleaseName",
            acquire ? g_variant_new("(su)", service_name, 0U)
                    : g_variant_new("(s)", service_name),
            G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000,
            cancellation, callback, data);
      },
      [connection](GObject *, GAsyncResult *result, GError **error) {
        return g_dbus_connection_call_finish(connection, result, error);
      });
  guint32 status = 0;
  g_variant_get(reply, "(u)", &status);
  g_variant_unref(reply);
  expect(status == 1, "could not change fake resolver ownership");
}

struct RegisteredObject {
  GDBusConnection *connection;
  guint id;

  ~RegisteredObject() {
    if (id != 0) {
      (void)g_dbus_connection_unregister_object(connection, id);
    }
  }
};

static cardio::promise<void> check_dbus_lookup_async() {
  using Source = elder_terms::IpScanNameSource;
  auto *raw_connection = co_await cardio::gio::submit<GDBusConnection *>(
      [](GCancellable *cancellation, GAsyncReadyCallback callback,
          gpointer data) {
        g_bus_get(G_BUS_TYPE_SESSION, cancellation, callback, data);
      },
      [](GObject *, GAsyncResult *result, GError **error) {
        return g_bus_get_finish(result, error);
      });
  auto connection = std::shared_ptr<GDBusConnection>(raw_connection,
                                                     g_object_unref);
  // This is a test-only bus; its shutdown must not terminate the test runner.
  g_dbus_connection_set_exit_on_close(connection.get(), FALSE);
  auto info = std::unique_ptr<GDBusNodeInfo, decltype(&g_dbus_node_info_unref)>(
      g_dbus_node_info_new_for_xml(interface_xml, nullptr),
      g_dbus_node_info_unref);
  expect(info != nullptr, "invalid fake resolver introspection");
  ServiceState service;
  const GDBusInterfaceVTable vtable{handle_method, nullptr, nullptr, {}};
  RegisteredObject registered{
      connection.get(),
      g_dbus_connection_register_object(connection.get(), object_path,
          info->interfaces[0], &vtable, &service, nullptr, nullptr),
  };
  expect(registered.id != 0, "could not register fake resolver");
  co_await change_name_owner_async(connection.get(), true);

  constexpr std::uint32_t address = UINT32_C(0xc0000219);
  auto lookup = elder_terms_ip_scan::create_multicast_name_lookup(
      G_BUS_TYPE_SESSION);
  const auto mdns = co_await lookup(address, Source::mdns, {});
  expect(mdns.size() == 2 && mdns[0].name == "other.local" &&
             mdns[0].interface_index == 3 && mdns[1].name == "printer.local" &&
             mdns[1].interface_index == 2,
         "mDNS response lost names or interface associations");
  const auto llmnr = co_await lookup(address, Source::llmnr, {});
  expect(llmnr.size() == 2 && llmnr[1].name == "printer",
         "LLMNR response did not preserve a single-label name");
  expect(service.valid_arguments && service.protocols ==
             std::vector<guint64>({8, 2}),
         "ResolveAddress arguments or protocol filters were incorrect");
  expect(service.senders.size() == 2 && service.senders[0] == service.senders[1],
         "one scan did not share its D-Bus connection");

  service.error_name = "org.freedesktop.resolve1.NoSuchRR";
  expect((co_await lookup(address, Source::mdns, {})).empty(),
         "missing name did not return an empty result");
  service.error_name.clear();
  expect(!(co_await lookup(address, Source::mdns, {})).empty(),
         "one missing name disabled the resolver for the entire scan");

  cardio::cancellation_source cancellation;
  service.cancel_when_called = &cancellation;
  bool canceled = false;
  try {
    (void)co_await lookup(address, Source::mdns,
                         cancellation.get_cancellation());
  } catch (const cardio::canceled_exception &) {
    canceled = true;
  }
  expect(canceled && service.pending != nullptr,
         "D-Bus lookup did not propagate caller cancellation");
  service.cancel_when_called = nullptr;
  g_dbus_method_invocation_return_dbus_error(
      service.pending, "org.freedesktop.resolve1.NoSuchRR", "late reply");
  service.pending = nullptr;

  co_await change_name_owner_async(connection.get(), false);
  auto unavailable = elder_terms_ip_scan::create_multicast_name_lookup(
      G_BUS_TYPE_SESSION);
  expect((co_await unavailable(address, Source::mdns, {})).empty(),
         "missing resolver service did not return an empty result");
  co_await change_name_owner_async(connection.get(), true);
  expect((co_await unavailable(address, Source::llmnr, {})).empty(),
         "service absence was not cached for the scan");
  auto next_scan = elder_terms_ip_scan::create_multicast_name_lookup(
      G_BUS_TYPE_SESSION);
  expect(!(co_await next_scan(address, Source::llmnr, {})).empty(),
         "a new scan did not discover the restored service");
  co_await change_name_owner_async(connection.get(), false);
}

static cardio::promise<void> run_async(cardio::dispatcher_group_glib *group,
                                      std::exception_ptr *error) {
  try {
    co_await check_dbus_lookup_async();
  } catch (...) {
    *error = std::current_exception();
  }
  group->shutdown();
}

} // namespace elder_terms_ip_scan_name_resolver_test

int main() {
  auto bus = std::shared_ptr<GTestDBus>(g_test_dbus_new(G_TEST_DBUS_NONE),
                                       g_object_unref);
  g_test_dbus_up(bus.get());
  std::exception_ptr error;
  {
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib dispatcher(group);
    auto task = elder_terms_ip_scan_name_resolver_test::run_async(&group, &error);
    dispatcher.park();
  }
  g_test_dbus_down(bus.get());
  if (error != nullptr) {
    try {
      std::rethrow_exception(error);
    } catch (const std::exception &exception) {
      std::cerr << exception.what() << '\n';
    }
    return 1;
  }
  return 0;
}

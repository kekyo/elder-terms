#include "ip-scan-name-resolver.h"

#include <sys/socket.h>

#include <memory>
#include <utility>

namespace elder_terms_ip_scan {

static constexpr const char *service_name = "org.freedesktop.resolve1";
static constexpr const char *object_path = "/org/freedesktop/resolve1";
static constexpr const char *interface_name = "org.freedesktop.resolve1.Manager";

struct ResolverState {
  GBusType bus_type = G_BUS_TYPE_SYSTEM;
  std::shared_ptr<GDBusConnection> connection;
  cardio::primitives::mutex initialization_mutex;
  bool ready = false;
  bool unavailable = false;
};

static cardio::promise<std::shared_ptr<GDBusConnection>>
connect_resolver_async(const std::shared_ptr<ResolverState> &state,
                       cardio::cancellation cancellation) {
  auto lock_promise = state->initialization_mutex.lock(cancellation);
  auto lock = std::move(co_await lock_promise);
  if (state->unavailable) {
    co_return nullptr;
  }
  if (state->ready) {
    co_return state->connection;
  }
  try {
    if (state->connection == nullptr) {
      auto *connection = co_await cardio::gio::submit<GDBusConnection *>(
          [state](GCancellable *signal, GAsyncReadyCallback callback,
                    gpointer data) {
            g_bus_get(state->bus_type, signal, callback, data);
          },
          [](GObject *, GAsyncResult *result, GError **error) {
            auto *connection = g_bus_get_finish(result, error);
            if (connection != nullptr) {
              // Production uses the system bus, not the desktop session bus.
              // Losing an optional resolver must not terminate the application.
              g_dbus_connection_set_exit_on_close(connection, FALSE);
            }
            return connection;
          }, cancellation);
      state->connection = std::shared_ptr<GDBusConnection>(connection,
                                                           g_object_unref);
    }
    // A running resolver need not have a D-Bus activation file. In particular,
    // StartServiceByName can fail before checking whether it already has an
    // owner, so check ownership before requesting activation.
    auto *owner_reply = co_await cardio::gio::submit<GVariant *>(
        [state](GCancellable *signal, GAsyncReadyCallback callback,
                 gpointer data) {
          g_dbus_connection_call(
              state->connection.get(), "org.freedesktop.DBus",
              "/org/freedesktop/DBus", "org.freedesktop.DBus",
              "NameHasOwner", g_variant_new("(s)", service_name),
              G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, -1,
              signal, callback, data);
        },
        [state](GObject *, GAsyncResult *result, GError **error) {
          return g_dbus_connection_call_finish(state->connection.get(), result,
                                               error);
        }, cancellation);
    gboolean has_owner = FALSE;
    g_variant_get(owner_reply, "(b)", &has_owner);
    g_variant_unref(owner_reply);
    if (has_owner) {
      state->ready = true;
      co_return state->connection;
    }
    auto *reply = co_await cardio::gio::submit<GVariant *>(
        [state](GCancellable *signal, GAsyncReadyCallback callback,
                  gpointer data) {
          g_dbus_connection_call(
              state->connection.get(), "org.freedesktop.DBus",
              "/org/freedesktop/DBus", "org.freedesktop.DBus",
              "StartServiceByName", g_variant_new("(su)", service_name, 0U),
              G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1,
              signal, callback, data);
        },
        [state](GObject *, GAsyncResult *result, GError **error) {
          return g_dbus_connection_call_finish(state->connection.get(), result,
                                                error);
        }, cancellation);
    guint32 status = 0;
    g_variant_get(reply, "(u)", &status);
    g_variant_unref(reply);
    state->ready = status == 1 || status == 2;
    state->unavailable = !state->ready;
    co_return state->ready ? state->connection : nullptr;
  } catch (const cardio::gio::gio_error &) {
    state->unavailable = true;
    co_return nullptr;
  }
}

static cardio::promise<std::vector<elder_terms::IpScanNameCandidate>>
lookup_multicast_name_async(std::shared_ptr<ResolverState> state,
                            std::uint32_t address,
                            elder_terms::IpScanNameSource source,
                            cardio::cancellation cancellation) {
  using Source = elder_terms::IpScanNameSource;
  std::vector<elder_terms::IpScanNameCandidate> names;
  cancellation.throw_if_cancellation_requested();
  if (source != Source::mdns && source != Source::llmnr) {
    co_return names;
  }
  auto connection = co_await connect_resolver_async(state, cancellation);
  if (connection == nullptr) {
    co_return names;
  }
  const guint64 protocol = source == Source::mdns ? UINT64_C(1) << 3
                                                 : UINT64_C(1) << 1;
  try {
    auto *raw_reply = co_await cardio::gio::submit<GVariant *>(
        [connection, address, protocol](GCancellable *signal,
                                         GAsyncReadyCallback callback,
                                         gpointer data) {
          GVariantBuilder bytes;
          g_variant_builder_init(&bytes, G_VARIANT_TYPE("ay"));
          for (int shift = 24; shift >= 0; shift -= 8) {
            g_variant_builder_add(&bytes, "y",
                                   static_cast<guint8>(address >> shift));
          }
          g_dbus_connection_call(
              connection.get(), service_name, object_path, interface_name,
              "ResolveAddress", g_variant_new("(iiayt)", 0, AF_INET, &bytes,
                                                protocol),
              G_VARIANT_TYPE("(a(is)t)"), G_DBUS_CALL_FLAGS_NONE, -1,
              signal, callback, data);
        },
        [connection](GObject *, GAsyncResult *result, GError **error) {
          return g_dbus_connection_call_finish(connection.get(), result, error);
        }, cancellation);
    auto reply = std::unique_ptr<GVariant, decltype(&g_variant_unref)>(
        raw_reply, g_variant_unref);
    GVariantIter *raw_candidates = nullptr;
    guint64 result_flags = 0;
    g_variant_get(reply.get(), "(a(is)t)", &raw_candidates, &result_flags);
    auto candidates =
        std::unique_ptr<GVariantIter, decltype(&g_variant_iter_free)>(
            raw_candidates, g_variant_iter_free);
    if ((result_flags & protocol) == 0) {
      co_return names;
    }
    gint32 interface_index = 0;
    const gchar *name = nullptr;
    while (g_variant_iter_next(candidates.get(), "(i&s)", &interface_index,
                               &name)) {
      names.push_back({name, interface_index});
    }
  } catch (const cardio::gio::gio_error &error) {
    if ((error.domain() == G_DBUS_ERROR &&
         (error.code() == G_DBUS_ERROR_SERVICE_UNKNOWN ||
          error.code() == G_DBUS_ERROR_NAME_HAS_NO_OWNER)) ||
        (error.domain() == G_IO_ERROR && error.code() == G_IO_ERROR_CLOSED)) {
      state->unavailable = true;
    }
  }
  co_return names;
}

MulticastLookup create_multicast_name_lookup(GBusType bus_type) {
  auto state = std::make_shared<ResolverState>();
  state->bus_type = bus_type;
  return [state](std::uint32_t address, elder_terms::IpScanNameSource source,
                 cardio::cancellation cancellation) {
    return lookup_multicast_name_async(state, address, source, cancellation);
  };
}

} // namespace elder_terms_ip_scan

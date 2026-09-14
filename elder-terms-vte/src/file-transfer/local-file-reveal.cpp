#include "local-file-reveal.h"

#include <fcntl.h>
#include <gio/gio.h>
#include <memory>
#include <stdexcept>
#include <utility>

namespace elder_terms {

static constexpr const char *portal_name = "org.freedesktop.portal.Desktop";
static constexpr const char *portal_path = "/org/freedesktop/portal/desktop";
static constexpr const char *request_interface = "org.freedesktop.portal.Request";
using RevealVariant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
using RevealString = std::unique_ptr<gchar, decltype(&g_free)>;

// Only explicit unavailability or launch failure permits another launch.
// NoReply, timeouts and disconnected calls may already have opened a window.
static void allow_desktop_fallback(GError **error) {
  if (!error || !*error) return;
  RevealString name(g_dbus_error_get_remote_error(*error), g_free);
  if (name && g_str_equal(name.get(), "org.freedesktop.portal.Error.Cancelled")) {
    g_clear_error(error);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
        "File manager request cancelled");
    return;
  }
  if (g_error_matches(*error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
      g_error_matches(*error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
      g_error_matches(*error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD) ||
      g_error_matches(*error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE) ||
      g_error_matches(*error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT) ||
      g_error_matches(*error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED) ||
      g_error_matches(*error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED) ||
      g_error_matches(*error, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_EXEC_FAILED) ||
      g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) ||
      (name && (g_str_equal(name.get(), "org.freedesktop.portal.Error.NotFound") ||
                g_str_equal(name.get(), "org.freedesktop.portal.Error.Failed")))) {
    g_clear_error(error);
  }
}

struct RevealResponse {
  cardio::promise_source<guint> source;
};

struct RevealObservation {
  GDBusConnection *connection;
  guint response_id = 0;
  guint owner_id = 0;
  gulong closed_id = 0;

  ~RevealObservation() {
    if (response_id) g_dbus_connection_signal_unsubscribe(connection, response_id);
    if (owner_id) g_dbus_connection_signal_unsubscribe(connection, owner_id);
    if (closed_id) g_signal_handler_disconnect(connection, closed_id);
  }
};

static void free_reveal_response(gpointer data) {
  delete static_cast<std::shared_ptr<RevealResponse> *>(data);
}

static void on_reveal_response(GDBusConnection *, const gchar *, const gchar *,
    const gchar *, const gchar *, GVariant *parameters, gpointer data) {
  auto state = *static_cast<std::shared_ptr<RevealResponse> *>(data);
  if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})"))) return;
  guint response = 0;
  GVariant *results = nullptr;
  g_variant_get(parameters, "(u@a{sv})", &response, &results);
  g_variant_unref(results);
  (void)state->source.try_resolve(response);
}

static void subscribe_reveal_response(RevealObservation &observation,
    const std::string &handle, const std::shared_ptr<RevealResponse> &state) {
  if (observation.response_id) {
    g_dbus_connection_signal_unsubscribe(observation.connection, observation.response_id);
  }
  observation.response_id = g_dbus_connection_signal_subscribe(
      observation.connection, portal_name, request_interface, "Response",
      handle.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_reveal_response,
      new std::shared_ptr<RevealResponse>(state), free_reveal_response);
}

static cardio::promise<void> close_reveal_request_async(
    GDBusConnection *connection, const std::string &handle) {
  try {
    RevealVariant reply(co_await cardio::gio::submit<GVariant *>(
        [connection, &handle](GCancellable *signal, GAsyncReadyCallback callback, gpointer data) {
          g_dbus_connection_call(connection, portal_name, handle.c_str(),
              request_interface, "Close", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE,
              -1, signal, callback, data);
        },
        [connection](GObject *, GAsyncResult *result, GError **error) {
          return g_dbus_connection_call_finish(connection, result, error);
        }), g_variant_unref);
  } catch (const cardio::gio::gio_error &) {
    // A completed request or vanished portal no longer has a request to close.
  }
}

static cardio::promise<bool> reveal_using_portal_async(
    GDBusConnection *connection, const std::string &path,
    const std::string &parent_window, cardio::cancellation cancellation) {
  // Never turn a selected link into its target, or block on a special file.
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0) co_return false;
  auto descriptors = std::shared_ptr<GUnixFDList>(
      g_unix_fd_list_new_from_array(&descriptor, 1), g_object_unref);
  RevealString uuid(g_uuid_string_random(), g_free);
  std::string token = std::string("elder_terms_reveal_") + uuid.get();
  for (auto &character : token) if (character == '-') character = '_';
  std::string sender(g_dbus_connection_get_unique_name(connection) + 1);
  for (auto &character : sender) if (character == '.') character = '_';
  std::string handle = std::string(portal_path) + "/request/" + sender + "/" + token;
  auto state = std::make_shared<RevealResponse>();
  auto response = state->source.get_promise();
  RevealObservation observation{connection};
  // Subscribe before calling OpenDirectory: a fast portal can respond first.
  subscribe_reveal_response(observation, handle, state);
  observation.owner_id = g_dbus_connection_signal_subscribe(connection,
      "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
      "/org/freedesktop/DBus", portal_name, G_DBUS_SIGNAL_FLAGS_NONE,
      [](GDBusConnection *, const gchar *, const gchar *, const gchar *,
          const gchar *, GVariant *parameters, gpointer data) {
        auto state = *static_cast<std::shared_ptr<RevealResponse> *>(data);
        const gchar *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
        g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
        if (*old_owner && !g_str_equal(old_owner, new_owner)) {
          (void)state->source.try_reject(std::runtime_error("File manager portal disconnected"));
        }
      }, new std::shared_ptr<RevealResponse>(state), free_reveal_response);
  observation.closed_id = g_signal_connect_data(connection, "closed",
      G_CALLBACK(+[](GDBusConnection *, gboolean, GError *, gpointer data) {
        auto state = *static_cast<std::shared_ptr<RevealResponse> *>(data);
        (void)state->source.try_reject(std::runtime_error("Desktop session bus disconnected"));
      }), new std::shared_ptr<RevealResponse>(state),
      +[](gpointer data, GClosure *) { free_reveal_response(data); }, G_CONNECT_DEFAULT);
  auto registration = cancellation.on_cancellation_requested([state] {
    (void)state->source.try_cancel();
  });
  std::exception_ptr failure;
  bool interaction_ended = false;
  try {
    RevealVariant reply(co_await cardio::gio::submit<GVariant *>(
        [connection, &parent_window, &token, descriptors](GCancellable *signal,
            GAsyncReadyCallback callback, gpointer data) {
          GVariantBuilder options;
          g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
          g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
          g_dbus_connection_call_with_unix_fd_list(connection, portal_name, portal_path,
              "org.freedesktop.portal.OpenURI", "OpenDirectory",
              g_variant_new("(sha{sv})", parent_window.c_str(), 0, &options),
              G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, descriptors.get(),
              signal, callback, data);
        },
        [connection](GObject *, GAsyncResult *result, GError **error) {
          auto *reply = g_dbus_connection_call_with_unix_fd_list_finish(connection, nullptr, result, error);
          allow_desktop_fallback(error);
          return reply;
        }, cancellation), g_variant_unref);
    if (!reply) {
      (void)state->source.try_resolve(0);
      co_return false;
    }
    const gchar *returned_handle = nullptr;
    g_variant_get(reply.get(), "(&o)", &returned_handle);
    if (handle != returned_handle) {
      handle = returned_handle;
      subscribe_reveal_response(observation, handle, state);
    }
    const guint result = co_await response;
    interaction_ended = true;
    cancellation.throw_if_cancellation_requested();
    // Response 1 is user cancellation. Response 2 is ambiguous: report it,
    // but do not start another application after the interaction has ended.
    if (result > 1) throw std::runtime_error("The file manager request did not complete");
    co_return true;
  } catch (...) {
    failure = std::current_exception();
  }
  registration.reset();
  (void)state->source.try_resolve(0);
  // Close is deliberately awaited without the already-cancelled window token.
  if (!interaction_ended) co_await close_reveal_request_async(connection, handle);
  std::rethrow_exception(failure);
}

cardio::promise<bool> try_reveal_local_file_async(
    std::string path, std::string parent_window, cardio::cancellation cancellation) {
  std::shared_ptr<GDBusConnection> connection;
  try {
    connection.reset(co_await cardio::gio::submit<GDBusConnection *>(
        [](GCancellable *signal, GAsyncReadyCallback callback, gpointer data) {
          g_bus_get(G_BUS_TYPE_SESSION, signal, callback, data);
        },
        [](GObject *, GAsyncResult *result, GError **error) {
          return g_bus_get_finish(result, error);
        }, cancellation), g_object_unref);
  } catch (const cardio::gio::gio_error &) {
    co_return false;
  }
  g_dbus_connection_set_exit_on_close(connection.get(), FALSE);
  if (co_await reveal_using_portal_async(connection.get(), path, parent_window, cancellation)) co_return true;
  cancellation.throw_if_cancellation_requested();
  auto file = std::shared_ptr<GFile>(g_file_new_for_path(path.c_str()), g_object_unref);
  RevealString uri(g_file_get_uri(file.get()), g_free);
  RevealVariant reply(co_await cardio::gio::submit<GVariant *>(
      [connection, &uri](GCancellable *signal, GAsyncReadyCallback callback, gpointer data) {
        const gchar *uris[] = {uri.get(), nullptr};
        g_dbus_connection_call(connection.get(), "org.freedesktop.FileManager1",
            "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1", "ShowItems",
            g_variant_new("(^ass)", uris, ""), G_VARIANT_TYPE("()"),
            G_DBUS_CALL_FLAGS_NONE, -1, signal, callback, data);
      },
      [connection](GObject *, GAsyncResult *result, GError **error) {
        auto *reply = g_dbus_connection_call_finish(connection.get(), result, error);
        allow_desktop_fallback(error);
        return reply;
      }, cancellation), g_variant_unref);
  co_return reply != nullptr;
}

} // namespace elder_terms

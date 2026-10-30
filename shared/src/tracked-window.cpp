// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/tracked-window.h>
#include <elder-terms/modal-dialog.h>

#include <gio/gio.h>
#include <gdk/gdkx.h>

#include <memory>
#include <utility>

namespace elder_terms {

constexpr char object_path[] = "/net/kekyo/ElderTerms/TrackedWindow";
constexpr char interface_name[] = "net.kekyo.ElderTerms.TrackedWindow";
constexpr char introspection_xml[] =
    "<node><interface name='net.kekyo.ElderTerms.TrackedWindow'>"
    "<method name='Present'><arg type='u' direction='in'/>"
    "<arg type='s' direction='in'/></method>"
    "<signal name='Ready'><arg type='s'/></signal>"
    "</interface></node>";

struct TrackedWindowState {
  GtkWidget *window = nullptr;
  GDBusConnection *connection = nullptr;
  guint registration = 0;
  std::string launcher_bus;
  TrackedWindowPresenter presenter;
};

static void on_present(GDBusConnection *, const gchar *sender, const gchar *,
                const gchar *, const gchar *, GVariant *parameters,
                GDBusMethodInvocation *invocation, gpointer user_data) {
  auto *state = static_cast<TrackedWindowState *>(user_data);
  if (state->launcher_bus != sender) {
    g_dbus_method_invocation_return_dbus_error(invocation,
        "net.kekyo.ElderTerms.Error.NotLauncher", "Only the launcher may present this window");
    return;
  }
  if (state->window == nullptr || gtk_widget_in_destruction(state->window)) {
    g_dbus_method_invocation_return_dbus_error(invocation,
        "net.kekyo.ElderTerms.Error.Closed", "The window has closed");
    return;
  }
  guint32 timestamp = 0;
  const gchar *token = nullptr;
  g_variant_get(parameters, "(u&s)", &timestamp, &token);
  state->presenter(state->window, timestamp, token);
  g_dbus_method_invocation_return_value(invocation, nullptr);
}

static const GDBusInterfaceVTable vtable = {on_present, nullptr, nullptr, {nullptr}};

static void on_window_destroy(GtkWidget *, gpointer user_data) {
  auto *state = static_cast<TrackedWindowState *>(user_data);
  state->window = nullptr;
  g_dbus_connection_unregister_object(state->connection, state->registration);
  g_object_unref(state->connection);
  delete state;
}

bool attach_tracked_window(GtkWidget *window, TrackedWindowPresenter presenter) {
  const char *id = g_getenv("ELDER_TERMS_LAUNCH_ID");
  const char *launcher_bus = g_getenv("ELDER_TERMS_LAUNCHER_BUS");
  if (window == nullptr || !GTK_IS_WINDOW(window) || !gtk_widget_get_visible(window) ||
      id == nullptr || !g_uuid_string_is_valid(id) || launcher_bus == nullptr ||
      launcher_bus[0] != ':') return false;

  GError *error = nullptr;
  GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (connection == nullptr) {
    if (error != nullptr) g_error_free(error);
    return false;
  }
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(introspection_xml, &error);
  if (node == nullptr) {
    if (error != nullptr) g_error_free(error);
    g_object_unref(connection);
    return false;
  }
  auto state = std::make_unique<TrackedWindowState>();
  state->window = window;
  state->connection = connection;
  state->launcher_bus = launcher_bus;
  state->presenter = std::move(presenter);
  state->registration = g_dbus_connection_register_object(
      connection, object_path, node->interfaces[0], &vtable,
      state.get(), nullptr, &error);
  g_dbus_node_info_unref(node);
  if (state->registration == 0) {
    if (error != nullptr) g_error_free(error);
    g_object_unref(connection);
    return false;
  }
  if (!g_dbus_connection_emit_signal(connection, launcher_bus, object_path,
          interface_name, "Ready", g_variant_new("(s)", id), &error)) {
    if (error != nullptr) g_error_free(error);
    g_dbus_connection_unregister_object(connection, state->registration);
    g_object_unref(connection);
    return false;
  }
  g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), state.release());
  return true;
}

void present_tracked_window(GtkWidget *window, std::uint32_t timestamp,
                            const std::string &activation_token) {
  if (window == nullptr || !GTK_IS_WINDOW(window) ||
      gtk_widget_in_destruction(window)) return;
  if (!activation_token.empty())
    gtk_window_set_startup_id(GTK_WINDOW(window), activation_token.c_str());
  if (timestamp == 0 && gtk_widget_get_realized(window)) {
    GdkWindow *gdk_window = gtk_widget_get_window(window);
    if (gdk_window != nullptr && GDK_IS_X11_WINDOW(gdk_window)) {
      const auto mask = gdk_window_get_events(gdk_window);
      if ((mask & GDK_PROPERTY_CHANGE_MASK) == 0)
        gdk_window_set_events(gdk_window,
            static_cast<GdkEventMask>(mask | GDK_PROPERTY_CHANGE_MASK));
      timestamp = gdk_x11_get_server_time(gdk_window);
    }
  }
  present_modal_dialog(window, timestamp == 0 ? gtk_get_current_event_time() : timestamp);
}

} // namespace elder_terms

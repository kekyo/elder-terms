// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/tracked-window.h>

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

#include <string>
#include <unistd.h>

struct DockFixture {
  GtkWidget *window = nullptr;
  GDBusConnection *connection = nullptr;
  std::string session;
  bool receiver = false;
};

static void on_dock_present(GDBusConnection *, const gchar *, const gchar *,
    const gchar *, const gchar *, GVariant *parameters,
    GDBusMethodInvocation *invocation, gpointer user_data) {
  auto *fixture = static_cast<DockFixture *>(user_data);
  const char *session = nullptr;
  guint timestamp = 0;
  const char *token = nullptr;
  g_variant_get(parameters, "(&su&s)", &session, &timestamp, &token);
  if (fixture->session != session) {
    g_dbus_method_invocation_return_dbus_error(invocation,
        "net.kekyo.ElderTerms.Error.UnknownTab", "Unknown tab");
    return;
  }
  if (const char *capture = g_getenv("ELDER_TERMS_TRACKED_WINDOW_CAPTURE")) {
    gchar *previous = nullptr;
    g_file_get_contents(capture, &previous, nullptr, nullptr);
    const std::string next = std::string(previous == nullptr ? "" : previous) + "tab-present\n";
    g_file_set_contents(capture, next.c_str(), -1, nullptr);
    g_free(previous);
  }
  elder_terms::present_tracked_window(fixture->window, timestamp, token);
  g_dbus_method_invocation_return_value(invocation, nullptr);
}

static void on_tab_name_acquired(GDBusConnection *, const gchar *, gpointer user_data) {
  auto *fixture = static_cast<DockFixture *>(user_data);
  if (fixture->receiver) return;
  const char *id = g_getenv("ELDER_TERMS_LAUNCH_ID");
  const char *launcher = g_getenv("ELDER_TERMS_LAUNCHER_BUS");
  if (!id || !launcher) return;
  g_dbus_connection_emit_signal(fixture->connection, launcher,
      "/net/kekyo/ElderTerms/Dock", "net.kekyo.ElderTerms.Dock", "Tracked",
      g_variant_new("(ss)", id, fixture->session.c_str()), nullptr);
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  const auto *session = g_getenv("ELDER_TERMS_TRACKED_TAB_SESSION");
  const bool receiver = g_getenv("ELDER_TERMS_TRACKED_TAB_RECEIVER") != nullptr;
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), receiver ? "Tracked tab receiver" : "Tracked window fixture");
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
  gtk_widget_show_all(window);
  if (const char *pid_path = g_getenv("ELDER_TERMS_TRACKED_WINDOW_PID")) {
    const std::string pid = std::to_string(getpid());
    g_file_set_contents(pid_path, pid.c_str(), -1, nullptr);
  }
  if (!receiver) elder_terms::attach_tracked_window(window,
      [](GtkWidget *widget, std::uint32_t time, const std::string &token) {
        const char *capture = g_getenv("ELDER_TERMS_TRACKED_WINDOW_CAPTURE");
        if (capture != nullptr) {
          gchar *previous = nullptr;
          g_file_get_contents(capture, &previous, nullptr, nullptr);
          const std::string next = std::string(previous == nullptr ? "" : previous) + "present\n";
          g_file_set_contents(capture, next.c_str(), -1, nullptr);
          g_free(previous);
        }
        elder_terms::present_tracked_window(widget, time, token);
      });
  DockFixture dock{window, nullptr, session ? session : "", receiver};
  if (session != nullptr && g_uuid_string_is_valid(session)) {
    dock.connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    const char xml[] = "<node><interface name='net.kekyo.ElderTerms.Dock'>"
        "<method name='Present'><arg type='s' direction='in'/>"
        "<arg type='u' direction='in'/><arg type='s' direction='in'/></method>"
        "</interface></node>";
    auto *node = g_dbus_node_info_new_for_xml(xml, nullptr);
    const GDBusInterfaceVTable vtable{on_dock_present, nullptr, nullptr, {nullptr}};
    g_dbus_connection_register_object(dock.connection, "/net/kekyo/ElderTerms/Dock",
        node->interfaces[0], &vtable, &dock, nullptr, nullptr);
    g_dbus_node_info_unref(node);
    std::string name = "net.kekyo.ElderTerms.Tab.t";
    for (const char *cursor = session; *cursor; ++cursor)
      if (*cursor != '-') name.push_back(*cursor);
    g_bus_own_name_on_connection(dock.connection, name.c_str(),
        receiver ? static_cast<GBusNameOwnerFlags>(G_BUS_NAME_OWNER_FLAGS_REPLACE |
            G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT) :
            G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT,
        on_tab_name_acquired, nullptr, &dock, nullptr);
  }
  gtk_main();
  return 0;
}

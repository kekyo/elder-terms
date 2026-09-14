#include <gio/gio.h>
#include <unistd.h>

#include <fstream>
#include <string>

static constexpr const char *xml = R"xml(<node>
<interface name="org.freedesktop.portal.OpenURI">
 <method name="OpenDirectory"><arg type="s" direction="in"/><arg type="h" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
</interface>
<interface name="org.freedesktop.FileManager1">
 <method name="ShowItems"><arg type="as" direction="in"/><arg type="s" direction="in"/></method>
</interface>
<interface name="org.freedesktop.portal.Request"><method name="Close"/><signal name="Response"><arg type="u"/><arg type="a{sv}"/></signal></interface>
<interface name="org.example.ElderTerms.FileManagerFixture">
 <method name="Respond"><arg type="u" direction="in"/></method>
 <method name="Barrier"/>
</interface></node>)xml";

struct Fixture {
  GDBusConnection *connection;
  GDBusNodeInfo *info;
  std::string mode;
  std::string log;
  std::string request;
  std::string sender;
};

static void record(Fixture *fixture, const std::string &line) {
  std::ofstream(fixture->log, std::ios::app) << line << '\n';
}

static void respond(Fixture *fixture, guint response) {
  GVariantBuilder results;
  g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_emit_signal(fixture->connection, fixture->sender.c_str(),
      fixture->request.c_str(), "org.freedesktop.portal.Request", "Response",
      g_variant_new("(ua{sv})", response, &results), nullptr);
  record(fixture, "Response\t" + std::to_string(response));
}

static void handle_method(GDBusConnection *, const gchar *sender, const gchar *,
    const gchar *, const gchar *method, GVariant *parameters,
    GDBusMethodInvocation *invocation, gpointer data);

static const GDBusInterfaceVTable vtable{handle_method, nullptr, nullptr, {}};

static void handle_method(GDBusConnection *, const gchar *sender, const gchar *,
    const gchar *, const gchar *method, GVariant *parameters,
    GDBusMethodInvocation *invocation, gpointer data) {
  auto *fixture = static_cast<Fixture *>(data);
  const std::string name(method);
  if (name == "OpenDirectory") {
    const gchar *parent = nullptr;
    gint index = -1;
    GVariant *options = nullptr;
    g_variant_get(parameters, "(&sh@a{sv})", &parent, &index, &options);
    auto *message = g_dbus_method_invocation_get_message(invocation);
    auto *descriptors = g_dbus_message_get_unix_fd_list(message);
    const int descriptor = descriptors && index >= 0 &&
        index < g_unix_fd_list_get_length(descriptors)
        ? g_unix_fd_list_get(descriptors, index, nullptr) : -1;
    gchar *path = descriptor >= 0
        ? g_file_read_link(("/proc/self/fd/" + std::to_string(descriptor)).c_str(), nullptr)
        : nullptr;
    record(fixture, "OpenDirectory\t" + std::string(path ? path : "INVALID_FD"));
    g_free(path);
    if (descriptor >= 0) close(descriptor);
    const gchar *token = nullptr;
    const bool valid_token = g_variant_lookup(options, "handle_token", "&s", &token) &&
        token && *token;
    std::string sender_path(sender + 1);
    for (auto &character : sender_path) if (character == '.') character = '_';
    fixture->request = "/org/freedesktop/portal/desktop/request/" + sender_path +
        "/" + (valid_token ? token : "invalid");
    fixture->sender = sender;
    g_variant_unref(options);
    if (!valid_token || fixture->mode == "portal-unavailable" ||
        fixture->mode == "fallback" || fixture->mode == "all-fail") {
      g_dbus_method_invocation_return_dbus_error(invocation,
          "org.freedesktop.DBus.Error.UnknownMethod", "OpenDirectory unavailable");
      return;
    }
    g_dbus_connection_register_object(fixture->connection, fixture->request.c_str(),
        fixture->info->interfaces[2], &vtable, fixture, nullptr, nullptr);
    // Exercise a Response sent before the method reply as well as held responses.
    if (fixture->mode == "portal") respond(fixture, 0);
    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(o)", fixture->request.c_str()));
    return;
  }
  if (name == "ShowItems") {
    gchar **uris = nullptr;
    const gchar *startup = nullptr;
    g_variant_get(parameters, "(^as&s)", &uris, &startup);
    for (int index = 0; uris[index]; ++index) record(fixture, "ShowItems\t" + std::string(uris[index]));
    g_strfreev(uris);
    if (fixture->mode == "fallback" || fixture->mode == "all-fail") {
      g_dbus_method_invocation_return_dbus_error(invocation,
          "org.freedesktop.DBus.Error.Failed", "File manager could not launch");
      return;
    }
  } else if (name == "Respond") {
    guint response = 0;
    g_variant_get(parameters, "(u)", &response);
    respond(fixture, response);
  } else if (name == "Close") {
    record(fixture, "Close");
  }
  g_dbus_method_invocation_return_value(invocation, nullptr);
}

static bool own_name(GDBusConnection *connection, const char *name) {
  auto *reply = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", name, 0U), G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
  guint status = 0;
  if (reply) { g_variant_get(reply, "(u)", &status); g_variant_unref(reply); }
  return status == 1;
}

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  auto *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  auto *info = g_dbus_node_info_new_for_xml(xml, nullptr);
  if (!connection || !info) return 3;
  Fixture fixture{connection, info, argv[1], argv[2], {}, {}};
  if (!g_dbus_connection_register_object(connection, "/org/freedesktop/portal/desktop",
      info->interfaces[0], &vtable, &fixture, nullptr, nullptr) ||
      !g_dbus_connection_register_object(connection, "/org/freedesktop/FileManager1",
      info->interfaces[1], &vtable, &fixture, nullptr, nullptr) ||
      !g_dbus_connection_register_object(connection, "/fixture",
      info->interfaces[3], &vtable, &fixture, nullptr, nullptr)) return 4;
  if (!own_name(connection, "org.example.ElderTerms.FileManagerFixture") ||
      !own_name(connection, "org.freedesktop.FileManager1") ||
      (fixture.mode != "manager-only" && !own_name(connection, "org.freedesktop.portal.Desktop"))) return 5;
  record(&fixture, "Ready");
  auto *loop = g_main_loop_new(nullptr, FALSE);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  g_dbus_node_info_unref(info);
  g_object_unref(connection);
  return 0;
}

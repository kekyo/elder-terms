// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

#include "sftp-origin-bus.h"

#include <memory>
#include <utility>

namespace elder_terms {

static constexpr char interface_name[] = "net.kekyo.ElderTerms.SftpOrigin";
static constexpr char introspection_xml[] =
    "<node><interface name='net.kekyo.ElderTerms.SftpOrigin'>"
    "<method name='Show'/></interface></node>";

struct SftpOriginService {
  GDBusConnection *connection = nullptr;
  guint registration = 0;
  std::string bus_name;
  std::string path;
  std::string tab_bus_name;
  std::shared_ptr<struct SftpOriginState> state;
};

struct SftpOriginState {
  bool active = true;
  std::function<void()> show;
};

struct ShowCheck {
  std::shared_ptr<SftpOriginState> state;
  GDBusMethodInvocation *invocation = nullptr;
  std::string sender;
};

struct SftpRequest {
  std::function<void(bool)> complete;
};

static std::string object_path(const std::string &session_id) {
  std::string path = "/net/kekyo/ElderTerms/SftpOrigin/t";
  for (const char character : session_id)
    if (character != '-') path.push_back(character);
  return path;
}

static std::string tab_name(const std::string &session_id) {
  std::string name = "net.kekyo.ElderTerms.Tab.t";
  for (const char character : session_id)
    if (character != '-') name.push_back(character);
  return name;
}

static void on_owner_checked(GObject *source, GAsyncResult *result, gpointer user_data) {
  auto check = std::unique_ptr<ShowCheck>(static_cast<ShowCheck *>(user_data));
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  const gchar *owner = nullptr;
  if (reply) g_variant_get(reply, "(&s)", &owner);
  const bool allowed = check->state->active && owner != nullptr && check->sender == owner;
  if (allowed) {
    try {
      check->state->show();
      g_dbus_method_invocation_return_value(check->invocation, nullptr);
    } catch (...) {
      g_dbus_method_invocation_return_dbus_error(check->invocation,
          "net.kekyo.ElderTerms.SftpOrigin.ShowFailed",
          "Shared SFTP could not be presented");
    }
  } else g_dbus_method_invocation_return_dbus_error(check->invocation,
      "net.kekyo.ElderTerms.SftpOrigin.NotTabOwner",
      "Only the current tab owner may present shared SFTP");
  if (reply) g_variant_unref(reply);
  if (error) g_error_free(error);
}

static void on_show(GDBusConnection *, const gchar *, const gchar *,
    const gchar *, const gchar *, GVariant *, GDBusMethodInvocation *invocation,
    gpointer user_data) {
  auto *service = static_cast<SftpOriginService *>(user_data);
  auto check = std::make_unique<ShowCheck>();
  check->state = service->state;
  check->invocation = invocation;
  check->sender = g_dbus_method_invocation_get_sender(invocation);
  g_dbus_connection_call(service->connection, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
      g_variant_new("(s)", service->tab_bus_name.c_str()), G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, on_owner_checked, check.release());
}

static void on_show_finished(GObject *source, GAsyncResult *result, gpointer user_data) {
  auto request = std::unique_ptr<SftpRequest>(static_cast<SftpRequest *>(user_data));
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  const bool accepted = reply != nullptr;
  if (reply) g_variant_unref(reply);
  if (error) g_error_free(error);
  request->complete(accepted);
}

SftpOriginService *create_sftp_origin_service(GDBusConnection *connection,
    std::string session_id, std::function<void()> show) {
  if (!connection || !g_uuid_string_is_valid(session_id.c_str()) || !show) return nullptr;
  const auto *name = g_dbus_connection_get_unique_name(connection);
  if (!name) return nullptr;
  auto service = std::make_unique<SftpOriginService>();
  service->connection = G_DBUS_CONNECTION(g_object_ref(connection));
  service->bus_name = name;
  service->path = object_path(session_id);
  service->tab_bus_name = tab_name(session_id);
  service->state = std::make_shared<SftpOriginState>();
  service->state->show = std::move(show);
  GError *error = nullptr;
  auto *node = g_dbus_node_info_new_for_xml(introspection_xml, &error);
  if (!node) {
    if (error) g_error_free(error);
    g_object_unref(service->connection);
    return nullptr;
  }
  const GDBusInterfaceVTable vtable{on_show, nullptr, nullptr, {nullptr}};
  service->registration = g_dbus_connection_register_object(connection,
      service->path.c_str(), node->interfaces[0], &vtable,
      service.get(), nullptr, &error);
  g_dbus_node_info_unref(node);
  if (service->registration == 0) {
    if (error) g_error_free(error);
    g_object_unref(service->connection);
    return nullptr;
  }
  return service.release();
}

std::string sftp_origin_service_bus_name(const SftpOriginService *service) {
  return service ? service->bus_name : std::string();
}

void destroy_sftp_origin_service(SftpOriginService *service) {
  if (!service) return;
  service->state->active = false;
  g_dbus_connection_unregister_object(service->connection, service->registration);
  g_object_unref(service->connection);
  delete service;
}

void request_sftp_origin(GDBusConnection *connection,
    std::string origin_bus_name, std::string session_id,
    std::function<void(bool)> complete) {
  if (!complete) return;
  if (!connection || !g_dbus_is_unique_name(origin_bus_name.c_str()) ||
      !g_uuid_string_is_valid(session_id.c_str())) {
    complete(false);
    return;
  }
  const auto path = object_path(session_id);
  auto request = std::make_unique<SftpRequest>();
  request->complete = std::move(complete);
  g_dbus_connection_call(connection, origin_bus_name.c_str(), path.c_str(),
      interface_name, "Show", nullptr, G_VARIANT_TYPE_UNIT,
      G_DBUS_CALL_FLAGS_NONE, 3000, nullptr,
      on_show_finished, request.release());
}

}

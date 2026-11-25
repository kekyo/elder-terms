// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

#include "../../src/sftp-origin-bus.h"

#include <glib.h>

#include <stdexcept>

using namespace elder_terms;

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  auto *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  expect(bus != nullptr, "connect to isolated session bus");
  constexpr char session[] = "123e4567-e89b-12d3-a456-426614174000";
  int shown = 0;
  auto *service = create_sftp_origin_service(bus, session, [&shown] { ++shown; });
  expect(service != nullptr, "register shared SFTP origin");
  const auto owner = sftp_origin_service_bus_name(service);
  expect(g_dbus_is_unique_name(owner.c_str()), "publish the original process name");
  const auto *address = g_getenv("DBUS_SESSION_BUS_ADDRESS");
  expect(address != nullptr, "isolated bus address is available");
  auto *tab_bus = g_dbus_connection_new_for_address_sync(address,
      static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, nullptr);
  expect(tab_bus != nullptr && tab_bus != bus, "tab uses its own bus connection");
  bool complete = false;
  bool accepted = false;
  request_sftp_origin(tab_bus, owner, session, [&](bool value) {
    accepted = value;
    complete = true;
  });
  while (!complete) g_main_context_iteration(nullptr, TRUE);
  expect(!accepted && shown == 0, "unregistered caller cannot present shared SFTP");

  bool acquired = false;
  const auto name_owner = g_bus_own_name_on_connection(tab_bus,
      "net.kekyo.ElderTerms.Tab.t123e4567e89b12d3a456426614174000",
      G_BUS_NAME_OWNER_FLAGS_NONE,
      +[](GDBusConnection *, const gchar *, gpointer context) {
        *static_cast<bool *>(context) = true;
      }, nullptr, &acquired, nullptr);
  while (!acquired) g_main_context_iteration(nullptr, TRUE);
  complete = false;
  request_sftp_origin(bus, owner, session, [&](bool value) {
    accepted = value;
    complete = true;
  });
  while (!complete) g_main_context_iteration(nullptr, TRUE);
  expect(!accepted && shown == 0, "another caller cannot impersonate the current tab owner");
  complete = false;
  request_sftp_origin(tab_bus, owner, session, [&](bool value) {
    accepted = value;
    complete = true;
  });
  while (!complete) g_main_context_iteration(nullptr, TRUE);
  expect(accepted && shown == 1, "current tab owner presents shared SFTP once");

  complete = false;
  request_sftp_origin(bus, owner, "not-a-session", [&](bool value) {
    accepted = value;
    complete = true;
  });
  expect(complete && !accepted && shown == 1, "malformed session cannot select origin SFTP");

  bool vanished = false;
  const auto subscription = g_dbus_connection_signal_subscribe(bus,
      "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
      "/org/freedesktop/DBus", "net.kekyo.ElderTerms.Tab.t123e4567e89b12d3a456426614174000",
      G_DBUS_SIGNAL_FLAGS_NONE,
      +[](GDBusConnection *, const gchar *, const gchar *, const gchar *,
          const gchar *, GVariant *parameters, gpointer context) {
        const gchar *name = nullptr, *previous = nullptr, *next = nullptr;
        g_variant_get(parameters, "(&s&s&s)", &name, &previous, &next);
        if (previous[0] && !next[0]) *static_cast<bool *>(context) = true;
      }, &vanished, nullptr);
  g_bus_unown_name(name_owner);
  while (!vanished) g_main_context_iteration(nullptr, TRUE);
  g_dbus_connection_signal_unsubscribe(bus, subscription);
  complete = false;
  request_sftp_origin(tab_bus, owner, session, [&](bool value) {
    accepted = value;
    complete = true;
  });
  while (!complete) g_main_context_iteration(nullptr, TRUE);
  expect(!accepted && shown == 1, "former owner cannot present shared SFTP");

  destroy_sftp_origin_service(service);
  complete = false;
  request_sftp_origin(bus, owner, session, [&](bool value) {
    accepted = value;
    complete = true;
  });
  while (!complete) g_main_context_iteration(nullptr, TRUE);
  expect(!accepted && shown == 1, "closed origin cannot receive another request");
  g_object_unref(tab_bus);
  g_object_unref(bus);
  return 0;
}

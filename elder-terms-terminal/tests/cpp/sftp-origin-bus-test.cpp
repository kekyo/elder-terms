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
  bool complete = false;
  bool accepted = false;
  request_sftp_origin(bus, owner, session, [&](bool value) {
    accepted = value;
    complete = true;
  });
  while (!complete) g_main_context_iteration(nullptr, TRUE);
  expect(accepted && shown == 1, "remote tab presents the origin SFTP window once");

  complete = false;
  request_sftp_origin(bus, owner, "not-a-session", [&](bool value) {
    accepted = value;
    complete = true;
  });
  expect(complete && !accepted && shown == 1, "malformed session cannot select origin SFTP");

  destroy_sftp_origin_service(service);
  complete = false;
  request_sftp_origin(bus, owner, session, [&](bool value) {
    accepted = value;
    complete = true;
  });
  while (!complete) g_main_context_iteration(nullptr, TRUE);
  expect(!accepted && shown == 1, "closed origin cannot receive another request");
  g_object_unref(bus);
  return 0;
}

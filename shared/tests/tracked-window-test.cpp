// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/tracked-window.h>

#include <cassert>
#include <cstdint>
#include <string>

#include <gio/gio.h>
#include <gtk/gtk.h>

constexpr char object_path[] = "/net/kekyo/ElderTerms/TrackedWindow";
constexpr char interface_name[] = "net.kekyo.ElderTerms.TrackedWindow";

struct Observation {
  bool ready = false;
  bool completed = false;
  bool called = false;
  std::string sender;
  GMainLoop *loop = nullptr;
};

static void on_ready(GDBusConnection *, const gchar *sender, const gchar *,
              const gchar *, const gchar *, GVariant *parameters,
              gpointer user_data) {
  auto *observation = static_cast<Observation *>(user_data);
  const gchar *id = nullptr;
  g_variant_get(parameters, "(&s)", &id);
  assert(std::string(id) == "b5bf77a1-6418-4977-9bc1-18760bbb16a4");
  observation->sender = sender;
  observation->ready = true;
  if (observation->loop != nullptr) g_main_loop_quit(observation->loop);
}

static void on_present_reply(GObject *source, GAsyncResult *result, gpointer user_data) {
  auto *observation = static_cast<Observation *>(user_data);
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_finish(
      G_DBUS_CONNECTION(source), result, &error);
  assert(error == nullptr);
  assert(reply != nullptr);
  g_variant_unref(reply);
  observation->completed = true;
  if (observation->loop != nullptr) g_main_loop_quit(observation->loop);
}

static bool wait_for(Observation &observation, bool &value) {
  if (value) return true;
  GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
  observation.loop = loop;
  guint timeout = g_timeout_add_seconds(5, +[](gpointer user_data) -> gboolean {
    g_main_loop_quit(static_cast<GMainLoop *>(user_data));
    return G_SOURCE_REMOVE;
  }, loop);
  g_main_loop_run(loop);
  observation.loop = nullptr;
  if (g_main_context_find_source_by_id(nullptr, timeout)) g_source_remove(timeout);
  g_main_loop_unref(loop);
  return value;
}

int main(int argc, char **argv) {
  const bool no_bus = argc == 2 && std::string(argv[1]) == "--no-bus";
  if (no_bus) {
    g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/elder-plan12-bus", TRUE);
  }
  gtk_init(&argc, &argv);
  if (no_bus) {
    g_setenv("ELDER_TERMS_LAUNCH_ID", "b5bf77a1-6418-4977-9bc1-18760bbb16a4", TRUE);
    g_setenv("ELDER_TERMS_LAUNCHER_BUS", ":1.1", TRUE);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_show(window);
    assert(!elder_terms::attach_tracked_window(window,
        [](GtkWidget *, std::uint32_t, const std::string &) {}));
    gtk_widget_destroy(window);
    return 0;
  }
  GError *error = nullptr;
  GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  assert(error == nullptr && connection != nullptr);
  const char *bus_name = g_dbus_connection_get_unique_name(connection);
  assert(bus_name != nullptr);
  g_setenv("ELDER_TERMS_LAUNCH_ID", "b5bf77a1-6418-4977-9bc1-18760bbb16a4", TRUE);
  g_setenv("ELDER_TERMS_LAUNCHER_BUS", bus_name, TRUE);

  Observation observation;
  const guint subscription = g_dbus_connection_signal_subscribe(
      connection, nullptr, interface_name, "Ready", object_path, nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, on_ready, &observation, nullptr);
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_widget_show(window);
  assert(elder_terms::attach_tracked_window(window,
      [&observation](GtkWidget *presented, std::uint32_t time, const std::string &token) {
        assert(GTK_IS_WINDOW(presented));
        assert(time == 1234);
        assert(token == "activation-token");
        elder_terms::present_tracked_window(presented, time, token);
        observation.called = true;
      }));
  assert(wait_for(observation, observation.ready));
  assert(observation.sender == bus_name);

  gtk_widget_hide(window);
  assert(!gtk_widget_get_visible(window));

  g_dbus_connection_call(connection, observation.sender.c_str(), object_path,
      interface_name, "Present", g_variant_new("(us)", 1234u, "activation-token"),
      G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
      on_present_reply, &observation);
  assert(wait_for(observation, observation.completed));
  assert(observation.called);
  assert(gtk_widget_get_visible(window));

  gtk_widget_destroy(window);
  observation.completed = false;
  g_dbus_connection_call(connection, observation.sender.c_str(), object_path,
      interface_name, "Present", g_variant_new("(us)", 1234u, "activation-token"),
      G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
      +[](GObject *source, GAsyncResult *result, gpointer user_data) {
        auto *observation = static_cast<Observation *>(user_data);
        GError *error = nullptr;
        GVariant *reply = g_dbus_connection_call_finish(
            G_DBUS_CONNECTION(source), result, &error);
        assert(reply == nullptr && error != nullptr);
        g_clear_error(&error);
        observation->completed = true;
        if (observation->loop != nullptr) g_main_loop_quit(observation->loop);
      }, &observation);
  assert(wait_for(observation, observation.completed));

  g_dbus_connection_signal_unsubscribe(connection, subscription);
  g_object_unref(connection);
  return 0;
}

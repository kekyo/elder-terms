// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
#include "terminal-dock-bus.h"
#include "terminal-tabs.h"
#include <filesystem>
#include <map>
#include <set>
#include <utility>
#include <unistd.h>

namespace elder_terms {
static constexpr char path[] = "/net/kekyo/ElderTerms/Dock";
static constexpr char interface[] = "net.kekyo.ElderTerms.Dock";
static constexpr char prefix[] = "net.kekyo.ElderTerms.DockWindow.p";
static constexpr char methods[] = "<node><interface name='net.kekyo.ElderTerms.Dock'>"
    "<method name='Prepare'><arg type='s' direction='in'/><arg type='u' direction='out'/>"
    "<arg type='a(ssssssssaybhisuuu(asaybb))' direction='out'/></method>"
    "<method name='Finish'><arg type='s' direction='in'/><arg type='b' direction='in'/></method>"
    "<method name='State'><arg type='s' direction='in'/><arg type='b' direction='out'/></method>"
    "<method name='Release'><arg type='s' direction='in'/></method>"
    "<method name='CanDropAtWindow'><arg type='u' direction='in'/><arg type='b' direction='out'/></method>"
    "<method name='DropAtWindow'><arg type='s' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='Present'><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='s' direction='in'/></method>"
    "<signal name='Tracked'><arg type='s'/><arg type='s'/></signal></interface></node>";
struct Offer {
  std::function<cardio::promise<DockPrepared>()> prepare;
  DockDecision finish;
  std::string peer;
  gint64 offered_at = g_get_monotonic_time();
  bool committed = false;
  bool abandoned = false;
};
struct TrackedTab {
  std::string launcher;
  std::function<void(guint, const std::string &)> present;
};
static std::map<std::string, TrackedTab> tracked;
static GDBusConnection *bus;
static guint registration, subscription, publication;
static std::map<std::string, std::shared_ptr<Offer>> offers;
static std::set<std::string> peers;
static std::vector<cardio::promise<void>> requests;

using Variant = std::shared_ptr<GVariant>;
static Variant take_variant(GVariant *value) { return {value, g_variant_unref}; }
static cardio::promise<Variant> call(std::string peer, const char *object, const char *iface,
    const char *method, GVariant *args, const GVariantType *type, GUnixFDList **received = nullptr) {
  auto *reply = co_await cardio::gio::submit<GVariant *>(
      [&](GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
        g_dbus_connection_call_with_unix_fd_list(bus, peer.c_str(), object, iface, method, args,
            type, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, cancel, callback, data);
      }, [received](GObject *source, GAsyncResult *result, GError **error) {
        return g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(source), received, result, error);
      }, {});
  co_return take_variant(reply);
}

// A drag target receives a terminal FD. Bind both endpoints to the installed
// executable, in addition to the unguessable per-drag token and bus unique name.
static cardio::promise<bool> product_peer(const std::string &peer) {
  auto answer = co_await call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "GetConnectionUnixProcessID", g_variant_new("(s)", peer.c_str()), G_VARIANT_TYPE("(u)"));
  guint pid;
  g_variant_get(answer.get(), "(u)", &pid);
  std::error_code ec;
  co_return std::filesystem::equivalent("/proc/self/exe", "/proc/" + std::to_string(pid) + "/exe", ec);
}

static cardio::promise<void> prepare_offer(std::shared_ptr<Offer> offer, std::string peer,
    GDBusMethodInvocation *invocation) {
  try {
    if (!co_await product_peer(peer) || offer->abandoned) throw std::runtime_error("Invalid docking peer");
    auto prepared = co_await offer->prepare();
    offer->finish = std::move(prepared.finish);
    if (!offer->finish || offer->abandoned) throw std::runtime_error("The terminal cannot move now");
    GVariantBuilder pages;
    g_variant_builder_init(&pages, G_VARIANT_TYPE("a(ssssssssaybhisuuu(asaybb))"));
    for (const auto &page : prepared.batch.pages) {
      auto *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, page.checkpoint.data(), page.checkpoint.size(), 1);
      g_variant_builder_add(&pages, "(ssssssss@aybhisuuu@(asaybb))", page.id.c_str(), page.config.c_str(), page.startup.c_str(),
          page.title.c_str(), page.launcher.c_str(), page.settings.c_str(), page.log.c_str(), page.sftp_origin.c_str(), bytes, page.eof, page.descriptor, page.control_descriptor, page.encoding.c_str(), page.child, page.capacity, page.chunk, page.host_state.get());
    }
    g_dbus_method_invocation_return_value_with_unix_fd_list(invocation,
        g_variant_new("(ua(ssssssssaybhisuuu(asaybb)))", prepared.batch.active, &pages), prepared.batch.descriptors.get());
  } catch (const std::exception &error) {
    if (offer->finish) std::exchange(offer->finish, {})(false);
    offer->abandoned = true;
    g_dbus_method_invocation_return_dbus_error(invocation, "net.kekyo.ElderTerms.Dock.Unavailable", error.what());
  }
  g_object_unref(invocation);
}

static void dispatch(GDBusConnection *, const gchar *sender, const gchar *, const gchar *, const gchar *method,
    GVariant *args, GDBusMethodInvocation *invocation, gpointer) {
  if (std::string_view(method) == "CanDropAtWindow") {
    guint32 xid;
    g_variant_get(args, "(u)", &xid);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", can_receive_terminal_window_drop(xid)));
    return;
  }
  const char *key;
  g_variant_get_child(args, 0, "&s", &key);
  if (std::string_view(method) == "DropAtWindow") {
    guint32 xid;
    g_variant_get_child(args, 1, "u", &xid);
    if (std::string_view(key).starts_with(std::string(sender) + "/") && receive_terminal_window_drop(xid, key))
      g_dbus_method_invocation_return_value(invocation, nullptr);
    else g_dbus_method_invocation_return_dbus_error(invocation, "net.kekyo.ElderTerms.Dock.Closed", "Window unavailable");
    return;
  }
  if (std::string_view(method) == "Present") {
    auto found = tracked.find(key);
    if (found != tracked.end() && found->second.launcher == sender) {
      guint timestamp;
      const char *token;
      g_variant_get(args, "(&su&s)", &key, &timestamp, &token);
      found->second.present(timestamp, token);
      g_dbus_method_invocation_return_value(invocation, nullptr);
    } else g_dbus_method_invocation_return_dbus_error(invocation, "net.kekyo.ElderTerms.Dock.Closed", "Session unavailable");
    return;
  }
  auto found = offers.find(key);
  if (found != offers.end() && !found->second->abandoned) {
    auto offer = found->second;
    if (std::string_view(method) == "Prepare" && offer->peer.empty() &&
        g_get_monotonic_time() - offer->offered_at < 30 * G_TIME_SPAN_SECOND) {
      offer->peer = sender;
      std::erase_if(requests, [](const auto &task) { return task.is_ready(); });
      requests.emplace_back(prepare_offer(offer, sender, G_DBUS_METHOD_INVOCATION(g_object_ref(invocation))));
      return;
    }
    if (offer->peer == sender && std::string_view(method) == "State") {
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", offer->committed));
      return;
    }
    if (offer->peer == sender && offer->committed && std::string_view(method) == "Release") {
      offers.erase(found);
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    if (std::string_view(method) == "Finish" && offer->peer == sender) {
      gboolean commit;
      g_variant_get_child(args, 1, "b", &commit);
      if (offer->finish || (offer->committed && commit)) {
        if (offer->finish) std::exchange(offer->finish, {})(commit);
        offer->committed = commit;
        offer->abandoned = !commit;
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
      }
    }
  }
  g_dbus_method_invocation_return_dbus_error(invocation, "net.kekyo.ElderTerms.Dock.Expired", "Dock offer has expired");
}

GDBusConnection *terminal_dock_connection() { return bus; }
static std::string tab_name(std::string id) {
  std::erase(id, '-');
  return "net.kekyo.ElderTerms.Tab.t" + id;
}
bool track_terminal_dock_page(const std::string &id, std::string launcher, std::function<void(guint, const std::string &)> present) {
  if (!bus || !g_uuid_string_is_valid(id.c_str())) return false;
  auto *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", tab_name(id).c_str(),
          G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE), G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
  if (!reply) return false;
  guint result;
  g_variant_get(reply, "(u)", &result);
  g_variant_unref(reply);
  if (result != 1 && result != 4) return false;
  tracked.insert_or_assign(id, TrackedTab{std::move(launcher), std::move(present)});
  return true;
}
void untrack_terminal_dock_page(const std::string &id) {
  if (!bus || tracked.erase(id) == 0) return;
  g_dbus_connection_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName",
      g_variant_new("(s)", tab_name(id).c_str()), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr, nullptr);
}
void announce_terminal_dock_page(const std::string &id, const char *launch_id, const std::string &launcher) {
  if (bus && launch_id && !launcher.empty())
    g_dbus_connection_emit_signal(bus, launcher.c_str(), path, interface, "Tracked", g_variant_new("(ss)", launch_id, id.c_str()), nullptr);
}

static std::string presence_name() { return std::string(prefix) + std::to_string(getpid()); }
static cardio::promise<void> probe_window(std::string peer, guint32 xid, DockDecision ready) {
  gboolean available = FALSE;
  try {
    auto reply = co_await call(peer, path, interface, "CanDropAtWindow", g_variant_new("(u)", xid), G_VARIANT_TYPE("(b)"));
    g_variant_get(reply.get(), "(b)", &available);
  } catch (const std::exception &) {
    // A prospective target can disappear while the pointer passes over it.
  }
  ready(available);
}
void probe_terminal_window_drop(const std::string &peer, guint32 xid, DockDecision ready) {
  if (!bus || !g_dbus_is_unique_name(peer.c_str())) { ready(false); return; }
  std::erase_if(requests, [](const auto &task) { return task.is_ready(); });
  requests.emplace_back(probe_window(peer, xid, std::move(ready)));
}
void send_terminal_window_drop(const std::string &peer, guint32 xid, const std::string &token) {
  if (bus && g_dbus_is_unique_name(peer.c_str()))
    g_dbus_connection_call(bus, peer.c_str(), path, interface, "DropAtWindow",
        g_variant_new("(su)", token.c_str(), xid), G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NO_AUTO_START,
        -1, nullptr, nullptr, nullptr);
}
static void set_peer(const std::string &name, bool present) {
  if (!name.starts_with(prefix) || name == presence_name()) return;
  if (present) peers.insert(name); else peers.erase(name);
  set_terminal_tab_external_peers(!peers.empty());
}
void initialize_terminal_dock_bus() {
  bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  if (!bus) return;
  auto *xml = g_dbus_node_info_new_for_xml(methods, nullptr);
  static const GDBusInterfaceVTable handlers{dispatch, nullptr, nullptr, {nullptr}};
  registration = g_dbus_connection_register_object(bus, path, xml->interfaces[0], &handlers, nullptr, nullptr, nullptr);
  g_dbus_node_info_unref(xml);
  subscription = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus",
      "NameOwnerChanged", "/org/freedesktop/DBus", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
      +[](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GVariant *args, gpointer) {
        const char *name, *old_owner, *new_owner;
        g_variant_get(args, "(&s&s&s)", &name, &old_owner, &new_owner);
        set_peer(name, *new_owner);
        if (!*new_owner && *name == ':') {
          for (auto &[_, offer] : offers) if (offer->peer == name) {
            offer->abandoned = true;
            if (offer->finish) std::exchange(offer->finish, {})(false);
          }
        }
      }, nullptr, nullptr);
  // The bus daemon owns the name list and never waits for a GTK peer.
  auto *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "ListNames", nullptr, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
  if (reply) {
    GVariantIter *names;
    g_variant_get(reply, "(as)", &names);
    const char *name;
    while (g_variant_iter_next(names, "&s", &name)) set_peer(name, true);
    g_variant_iter_free(names);
    g_variant_unref(reply);
  }
}
void publish_terminal_dock_window(bool visible) {
  if (!bus || !registration) return;
  if (visible && !publication) publication = g_bus_own_name_on_connection(bus, presence_name().c_str(),
      G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr, nullptr, nullptr);
  else if (!visible && publication) g_bus_unown_name(std::exchange(publication, 0));
}
void shutdown_terminal_dock_bus() {
  if (!bus) return;
  publish_terminal_dock_window(false);
  g_dbus_connection_signal_unsubscribe(bus, subscription);
  if (registration) g_dbus_connection_unregister_object(bus, registration);
  requests.clear();
  offers.clear();
  g_clear_object(&bus);
}

std::string terminal_dock_offer(std::function<cardio::promise<DockPrepared>()> prepare) {
  if (!bus || !registration) return "local";
  std::erase_if(offers, [](const auto &entry) {
    auto &o = *entry.second;
    return o.peer.empty() || o.abandoned;
  });
  auto *uuid = g_uuid_string_random();
  std::string key(uuid);
  g_free(uuid);
  auto offer = std::make_shared<Offer>();
  offer->prepare = std::move(prepare);
  offers.emplace(key, std::move(offer));
  return std::string(g_dbus_connection_get_unique_name(bus)) + "/" + key;
}

cardio::promise<bool> terminal_dock_receive(std::string token, std::function<DockDecision(const DockBatch &)> stage) {
  const auto split = token.find('/');
  if (!bus || split == token.npos) co_return false;
  const auto peer = token.substr(0, split), key = token.substr(split + 1);
  if (!g_dbus_is_unique_name(peer.c_str()) || !g_uuid_string_is_valid(key.c_str())) co_return false;
  bool prepared = false, committed = false;
  DockDecision finish;
  bool commit_sent = false;
  try {
    if (!co_await product_peer(peer)) co_return false;
    GUnixFDList *fds = nullptr;
    auto reply = co_await call(peer, path, interface, "Prepare", g_variant_new("(s)", key.c_str()),
        G_VARIANT_TYPE("(ua(ssssssssaybhisuuu(asaybb)))"), &fds);
    prepared = true;
    DockBatch batch;
    if (!fds) throw std::runtime_error("Missing terminal descriptors");
    batch.descriptors.reset(fds, g_object_unref);
    GVariant *array;
    g_variant_get(reply.get(), "(u@a(ssssssssaybhisuuu(asaybb)))", &batch.active, &array);
    auto pages = take_variant(array);
    for (gsize i = 0; i < g_variant_n_children(array); ++i) {
      auto item = take_variant(g_variant_get_child_value(array, i));
      const char *id, *config, *startup, *title, *launcher, *settings, *log, *sftp;
      GVariant *bytes, *host_state;
      gboolean eof;
      gint fd, control_fd;
      const char *encoding;
      guint child, capacity, chunk;
      g_variant_get(item.get(), "(&s&s&s&s&s&s&s&s@aybhi&suuu@(asaybb))", &id, &config, &startup, &title, &launcher, &settings, &log, &sftp,
          &bytes, &eof, &fd, &control_fd, &encoding, &child, &capacity, &chunk, &host_state);
      auto host = take_variant(host_state);
      auto checkpoint = take_variant(bytes);
      gsize size;
      auto *data = static_cast<const std::uint8_t *>(g_variant_get_fixed_array(bytes, &size, 1));
      if (size == 0 || size > 64 * 1024 * 1024 || fd < 0 || fd >= g_unix_fd_list_get_length(fds) ||
          control_fd < -1 || control_fd >= g_unix_fd_list_get_length(fds) ||
          !g_uuid_string_is_valid(id)) throw std::runtime_error("Invalid terminal checkpoint");
      batch.pages.push_back({id, config, startup, title, launcher, settings, log, sftp, {data, data + size}, eof != FALSE, fd, control_fd, encoding, child, capacity, chunk, std::move(host)});
    }
    if (batch.active >= batch.pages.size()) throw std::runtime_error("Missing active tab");
    finish = stage(batch);
    if (finish) {
      commit_sent = true;
      co_await call(peer, path, interface, "Finish", g_variant_new("(sb)", key.c_str(), TRUE), G_VARIANT_TYPE_UNIT);
      committed = true;
    }
  } catch (const std::exception &error) {
    g_message("Terminal docking: %s", error.what());
  }
  if (commit_sent && !committed) {
    // A lost acknowledgement must never revive the previous reader.
    bool state_known = false;
    try {
      auto state = co_await call(peer, path, interface, "State", g_variant_new("(s)", key.c_str()), G_VARIANT_TYPE("(b)"));
      gboolean accepted;
      g_variant_get(state.get(), "(b)", &accepted);
      committed = accepted;
      state_known = true;
    } catch (const std::exception &) {}
    if (!state_known) {
      try {
        auto answer = co_await call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
            "NameHasOwner", g_variant_new("(s)", peer.c_str()), G_VARIANT_TYPE("(b)"));
        gboolean alive;
        g_variant_get(answer.get(), "(b)", &alive);
        // Once the old process is gone its descriptors cannot be read again.
        committed = !alive;
      } catch (const std::exception &) {}
    }
  }
  if (prepared && !committed) {
    try { co_await call(peer, path, interface, "Finish", g_variant_new("(sb)", key.c_str(), FALSE), G_VARIANT_TYPE_UNIT); }
    catch (const std::exception &) {}
  }
  if (finish) finish(committed);
  if (committed) g_dbus_connection_call(bus, peer.c_str(), path, interface, "Release", g_variant_new("(s)", key.c_str()),
      G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr, nullptr, nullptr);
  co_return committed;
}
}

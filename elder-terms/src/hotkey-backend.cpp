#include "hotkey-backend.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <X11/Xlib.h>
#include <gdk/gdkx.h>
#include <glib.h>

namespace elder_terms {

static constexpr char portal_bus_name[] =
    "org.freedesktop.portal.Desktop";
static constexpr char portal_object_path[] =
    "/org/freedesktop/portal/desktop";
static constexpr char portal_request_interface[] =
    "org.freedesktop.portal.Request";
static constexpr char portal_session_interface[] =
    "org.freedesktop.portal.Session";
static constexpr char portal_shortcuts_interface[] =
    "org.freedesktop.portal.GlobalShortcuts";
static constexpr auto supported_modifier_mask =
    static_cast<GdkModifierType>(GDK_CONTROL_MASK | GDK_MOD1_MASK |
                                 GDK_SHIFT_MASK | GDK_SUPER_MASK);

enum class PortalRequestKind {
  create_session,
  bind_shortcuts,
};

struct HotkeyBackendImplementation;

struct X11HotkeyGrab {
  std::string action_id;
  KeyCode keycode = 0;
  unsigned int modifiers = 0;
};

struct PortalRequestContext {
  std::weak_ptr<HotkeyBackendImplementation> implementation;
  unsigned int generation;
  PortalRequestKind kind;
};

struct HotkeyBackendImplementation {
  HotkeyBackendOptions options;
  std::vector<HotkeyAction> actions;
  HotkeyBackendKind kind = HotkeyBackendKind::none;
  GDBusConnection *connection = nullptr;
  std::optional<cardio::cancellation_source> cancellation_source;
  std::deque<cardio::promise<void>> tasks;
  Display *x11_display = nullptr;
  Window x11_root = 0;
  std::vector<X11HotkeyGrab> x11_grabs;
  std::vector<guint> portal_request_signal_ids;
  guint portal_activation_signal_id = 0;
  std::string portal_session_handle;
  unsigned int portal_session_response_generation = 0;
  unsigned int portal_bind_response_generation = 0;
  unsigned int generation = 1;
  bool destroyed = false;
};

struct HotkeyBackendState {
  std::shared_ptr<HotkeyBackendImplementation> implementation;
};

static cardio::promise<void> bind_portal_shortcut_async(
    std::shared_ptr<HotkeyBackendImplementation> implementation,
    unsigned int generation);
static cardio::promise<void> run_x11_event_loop_async(
    std::shared_ptr<HotkeyBackendImplementation> implementation);

HotkeyBackendKind
select_hotkey_backend_kind(const HotkeyBackendAvailability &availability) {
  if (availability.prefer_portal && availability.has_portal) {
    return HotkeyBackendKind::portal;
  }
  if (availability.has_x11) {
    return HotkeyBackendKind::x11;
  }
  if (availability.has_portal) {
    return HotkeyBackendKind::portal;
  }
  return HotkeyBackendKind::none;
}

std::optional<std::string>
build_portal_shortcut_trigger(const KeyBinding &binding) {
  if (binding.keyval == GDK_KEY_VoidSymbol || binding.modifiers == 0 ||
      (binding.modifiers & supported_modifier_mask) !=
          binding.modifiers) {
    return std::nullopt;
  }
  const char *key_name = gdk_keyval_name(binding.keyval);
  if (key_name == nullptr || *key_name == '\0') {
    return std::nullopt;
  }

  std::string trigger;
  if ((binding.modifiers & GDK_CONTROL_MASK) != 0) {
    trigger += "CTRL+";
  }
  if ((binding.modifiers & GDK_MOD1_MASK) != 0) {
    trigger += "ALT+";
  }
  if ((binding.modifiers & GDK_SHIFT_MASK) != 0) {
    trigger += "SHIFT+";
  }
  if ((binding.modifiers & GDK_SUPER_MASK) != 0) {
    trigger += "LOGO+";
  }
  trigger += key_name;
  return trigger;
}

std::optional<std::string>
find_hotkey_action_id(const std::vector<HotkeyAction> &actions,
                      guint keyval, GdkModifierType modifiers) {
  const auto action = std::find_if(
      actions.begin(), actions.end(),
      [keyval, modifiers](const HotkeyAction &candidate) {
        return key_binding_matches(candidate.binding, keyval,
                                   modifiers);
      });
  return action == actions.end()
             ? std::nullopt
             : std::optional<std::string>(action->id);
}

static bool has_text(const char *value) {
  return value != nullptr && *value != '\0';
}

static bool gdk_backend_is_pinned_to_x11() {
  const char *raw = g_getenv("GDK_BACKEND");
  if (!has_text(raw)) {
    return false;
  }
  std::string backend = raw;
  const std::size_t comma = backend.find(',');
  if (comma != std::string::npos) {
    backend.resize(comma);
  }
  std::transform(
      backend.begin(), backend.end(), backend.begin(),
      [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
      });
  return backend == "x11";
}

static bool should_prefer_portal() {
  if (gdk_backend_is_pinned_to_x11()) {
    return false;
  }
  if (has_text(g_getenv("WAYLAND_DISPLAY"))) {
    return true;
  }
  const char *session_type = g_getenv("XDG_SESSION_TYPE");
  return has_text(session_type) &&
         g_ascii_strcasecmp(session_type, "wayland") == 0;
}

static bool x11_is_available() {
  GdkDisplay *display = gdk_display_get_default();
  return display != nullptr && GDK_IS_X11_DISPLAY(display);
}

static unsigned int x11_modifier_mask(GdkModifierType modifiers) {
  unsigned int mask = 0;
  if ((modifiers & GDK_CONTROL_MASK) != 0) {
    mask |= ControlMask;
  }
  if ((modifiers & GDK_MOD1_MASK) != 0) {
    mask |= Mod1Mask;
  }
  if ((modifiers & GDK_SHIFT_MASK) != 0) {
    mask |= ShiftMask;
  }
  if ((modifiers & GDK_SUPER_MASK) != 0) {
    mask |= Mod4Mask;
  }
  return mask;
}

static bool x11_error_trapped = false;

static int on_x11_error(Display *, XErrorEvent *) {
  x11_error_trapped = true;
  return 0;
}

static constexpr std::array<unsigned int, 4> x11_lock_masks = {
    0U,
    LockMask,
    Mod2Mask,
    LockMask | Mod2Mask,
};

static void ungrab_x11_actions(
    HotkeyBackendImplementation *implementation) {
  if (implementation->x11_display == nullptr) {
    implementation->x11_grabs.clear();
    return;
  }
  x11_error_trapped = false;
  XErrorHandler previous_handler = XSetErrorHandler(on_x11_error);
  for (const X11HotkeyGrab &grab : implementation->x11_grabs) {
    for (const unsigned int lock_mask : x11_lock_masks) {
      XUngrabKey(implementation->x11_display,
                 static_cast<int>(grab.keycode),
                 grab.modifiers | lock_mask,
                 implementation->x11_root);
    }
  }
  XSync(implementation->x11_display, False);
  XSetErrorHandler(previous_handler);
  implementation->x11_grabs.clear();
}

static bool x11_action_is_already_grabbed(
    const HotkeyBackendImplementation *implementation,
    KeyCode keycode, unsigned int modifiers) {
  return std::any_of(
      implementation->x11_grabs.begin(),
      implementation->x11_grabs.end(),
      [keycode, modifiers](const X11HotkeyGrab &grab) {
        return grab.keycode == keycode &&
               grab.modifiers == modifiers;
      });
}

static bool grab_x11_action(
    HotkeyBackendImplementation *implementation,
    const HotkeyAction &action) {
  const KeyBinding &binding = action.binding;
  if (implementation->x11_display == nullptr ||
      binding.modifiers == 0 ||
      (binding.modifiers & supported_modifier_mask) !=
          binding.modifiers) {
    return false;
  }

  const KeyCode keycode = XKeysymToKeycode(
      implementation->x11_display,
      static_cast<KeySym>(binding.keyval));
  if (keycode == 0) {
    return false;
  }
  const unsigned int modifiers =
      x11_modifier_mask(binding.modifiers);
  if (x11_action_is_already_grabbed(
          implementation, keycode, modifiers)) {
    return true;
  }

  x11_error_trapped = false;
  XErrorHandler previous_handler = XSetErrorHandler(on_x11_error);
  for (const unsigned int lock_mask : x11_lock_masks) {
    XGrabKey(implementation->x11_display,
             static_cast<int>(keycode), modifiers | lock_mask,
             implementation->x11_root, True, GrabModeAsync,
             GrabModeAsync);
  }
  XSync(implementation->x11_display, False);
  XSetErrorHandler(previous_handler);
  if (x11_error_trapped) {
    x11_error_trapped = false;
    previous_handler = XSetErrorHandler(on_x11_error);
    for (const unsigned int lock_mask : x11_lock_masks) {
      XUngrabKey(implementation->x11_display,
                 static_cast<int>(keycode), modifiers | lock_mask,
                 implementation->x11_root);
    }
    XSync(implementation->x11_display, False);
    XSetErrorHandler(previous_handler);
    return false;
  }

  implementation->x11_grabs.push_back({
      .action_id = action.id,
      .keycode = keycode,
      .modifiers = modifiers,
  });
  return true;
}

static void grab_x11_actions(
    HotkeyBackendImplementation *implementation) {
  ungrab_x11_actions(implementation);
  for (const HotkeyAction &action : implementation->actions) {
    if (!grab_x11_action(implementation, action)) {
      std::cerr << "Failed to register X11 hotkey action "
                << action.id << '\n';
    }
  }
}

static void process_x11_event(
    HotkeyBackendImplementation *implementation, const XEvent &event) {
  if (implementation == nullptr || implementation->destroyed) {
    return;
  }
  if (event.type != KeyPress) {
    return;
  }
  const unsigned int modifiers =
      event.xkey.state &
      (ControlMask | Mod1Mask | ShiftMask | Mod4Mask);
  const auto grab = std::find_if(
      implementation->x11_grabs.begin(),
      implementation->x11_grabs.end(),
      [&event, modifiers](const X11HotkeyGrab &candidate) {
        return event.xkey.keycode == candidate.keycode &&
               modifiers == candidate.modifiers;
      });
  if (grab == implementation->x11_grabs.end()) {
    return;
  }
  if (implementation->options.activated) {
    implementation->options.activated(
        grab->action_id,
        {
            .activation_time =
                static_cast<std::uint32_t>(event.xkey.time),
            .activation_token = std::nullopt,
        });
  }
}

static bool initialize_x11_backend(
    const std::shared_ptr<HotkeyBackendImplementation> &implementation) {
  if (!x11_is_available() ||
      implementation->options.dispatcher == nullptr) {
    return false;
  }
  implementation->x11_display = XOpenDisplay(nullptr);
  if (implementation->x11_display == nullptr) {
    return false;
  }
  implementation->x11_root =
      DefaultRootWindow(implementation->x11_display);
  implementation->kind = HotkeyBackendKind::x11;
  grab_x11_actions(implementation.get());
  implementation->tasks.emplace_back(
      run_x11_event_loop_async(implementation));
  return true;
}

static void destroy_x11_backend(
    HotkeyBackendImplementation *implementation) {
  ungrab_x11_actions(implementation);
  if (implementation->x11_display != nullptr) {
    XCloseDisplay(implementation->x11_display);
    implementation->x11_display = nullptr;
  }
  implementation->x11_root = 0;
}

static cardio::promise<void> run_x11_event_loop_async(
    std::shared_ptr<HotkeyBackendImplementation> implementation) {
  try {
    const int file_descriptor =
        ConnectionNumber(implementation->x11_display);
    while (!implementation->destroyed) {
      const cardio::fd_event events = co_await cardio::from_fd(
          file_descriptor, cardio::fd_event::read,
          implementation->cancellation_source->get_cancellation());
      if ((events &
           (cardio::fd_event::error | cardio::fd_event::hangup)) !=
          cardio::fd_event::none) {
        co_return;
      }
      while (!implementation->destroyed &&
             XPending(implementation->x11_display) > 0) {
        XEvent event;
        XNextEvent(implementation->x11_display, &event);
        process_x11_event(implementation.get(), event);
      }
    }
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) {
    if (!implementation->destroyed) {
      std::cerr << "Failed to receive X11 hotkey events: "
                << error.what() << '\n';
    }
  }
}

static cardio::promise<GVariant *> call_dbus_async(
    GDBusConnection *connection, const char *bus_name,
    const char *object_path, const char *interface_name,
    const char *method_name, GVariant *parameters,
    const GVariantType *reply_type,
    cardio::cancellation cancellation) {
  co_return co_await cardio::gio::submit<GVariant *>(
      [connection, bus_name, object_path, interface_name, method_name,
       parameters, reply_type](
          GCancellable *cancellable, GAsyncReadyCallback callback,
          gpointer user_data) {
        g_dbus_connection_call(
            connection, bus_name, object_path, interface_name,
            method_name, parameters, reply_type,
            G_DBUS_CALL_FLAGS_NONE, -1, cancellable, callback,
            user_data);
      },
      [connection](GObject *, GAsyncResult *result, GError **error) {
        return g_dbus_connection_call_finish(connection, result, error);
      },
      std::move(cancellation));
}

static cardio::promise<bool> portal_is_available_async(
    HotkeyBackendImplementation *implementation,
    cardio::cancellation cancellation) {
  if (implementation->connection == nullptr) {
    co_return false;
  }
  GVariant *result = co_await call_dbus_async(
      implementation->connection, portal_bus_name,
      portal_object_path, "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", portal_shortcuts_interface, "version"),
      G_VARIANT_TYPE("(v)"), std::move(cancellation));
  GVariant *version = nullptr;
  g_variant_get(result, "(v)", &version);
  const bool available =
      version != nullptr && g_variant_get_uint32(version) > 0U;
  if (version != nullptr) {
    g_variant_unref(version);
  }
  g_variant_unref(result);
  co_return available;
}

static std::string portal_object_path_element(std::string value) {
  if (!value.empty() && value.front() == ':') {
    value.erase(value.begin());
  }
  for (char &character : value) {
    if (std::isalnum(static_cast<unsigned char>(character)) == 0) {
      character = '_';
    }
  }
  return value.empty() ? std::string("elder_terms") : value;
}

static std::string portal_request_handle(
    GDBusConnection *connection, const std::string &token) {
  const char *unique_name =
      connection == nullptr
          ? nullptr
          : g_dbus_connection_get_unique_name(connection);
  return std::string(portal_object_path) + "/request/" +
         portal_object_path_element(
             unique_name == nullptr ? std::string()
                                    : std::string(unique_name)) +
         "/" + token;
}

static std::string portal_token(const char *prefix,
                                unsigned int generation) {
  gchar *uuid = g_uuid_string_random();
  std::string token =
      std::string(prefix) + std::to_string(generation) + "_" +
      (uuid == nullptr ? std::string("token") : std::string(uuid));
  g_free(uuid);
  return portal_object_path_element(std::move(token));
}

static void free_portal_request_context(gpointer data) {
  delete static_cast<PortalRequestContext *>(data);
}

static void close_portal_session(
    HotkeyBackendImplementation *implementation) {
  if (implementation->connection == nullptr ||
      implementation->portal_session_handle.empty()) {
    implementation->portal_session_handle.clear();
    return;
  }
  const std::string handle =
      implementation->portal_session_handle;
  implementation->portal_session_handle.clear();
  g_dbus_connection_call(
      implementation->connection, portal_bus_name, handle.c_str(),
      portal_session_interface, "Close", nullptr, nullptr,
      G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

static void clear_portal_subscriptions(
    HotkeyBackendImplementation *implementation) {
  if (implementation->connection != nullptr) {
    for (const guint signal_id :
         implementation->portal_request_signal_ids) {
      g_dbus_connection_signal_unsubscribe(
          implementation->connection, signal_id);
    }
    if (implementation->portal_activation_signal_id != 0) {
      g_dbus_connection_signal_unsubscribe(
          implementation->connection,
          implementation->portal_activation_signal_id);
    }
  }
  implementation->portal_request_signal_ids.clear();
  implementation->portal_activation_signal_id = 0;
}

static void clear_portal_backend(
    HotkeyBackendImplementation *implementation) {
  clear_portal_subscriptions(implementation);
  close_portal_session(implementation);
  implementation->portal_session_response_generation = 0;
  implementation->portal_bind_response_generation = 0;
}

static void on_portal_activated(
    GDBusConnection *, const gchar *, const gchar *, const gchar *,
    const gchar *, GVariant *parameters, gpointer user_data) {
  auto *implementation =
      static_cast<HotkeyBackendImplementation *>(user_data);
  if (implementation == nullptr || implementation->destroyed ||
      implementation->kind != HotkeyBackendKind::portal ||
      implementation->portal_bind_response_generation !=
          implementation->generation ||
      implementation->actions.empty()) {
    return;
  }

  const gchar *session_handle = nullptr;
  const gchar *shortcut_id = nullptr;
  guint64 timestamp = 0;
  GVariant *options = nullptr;
  g_variant_get(parameters, "(&o&st@a{sv})", &session_handle,
                &shortcut_id, &timestamp, &options);
  const bool session_matches =
      session_handle != nullptr &&
      implementation->portal_session_handle == session_handle &&
      shortcut_id != nullptr;
  const auto action =
      session_matches
          ? std::find_if(
                implementation->actions.begin(),
                implementation->actions.end(),
                [shortcut_id](const HotkeyAction &candidate) {
                  return candidate.id == shortcut_id;
                })
          : implementation->actions.end();
  if (action == implementation->actions.end()) {
    g_variant_unref(options);
    return;
  }

  HotkeyActivationContext context;
  if (timestamp <= std::numeric_limits<std::uint32_t>::max()) {
    context.activation_time =
        static_cast<std::uint32_t>(timestamp);
  }
  gchar *activation_token = nullptr;
  if (g_variant_lookup(options, "activation_token", "s",
                       &activation_token) &&
      activation_token != nullptr && *activation_token != '\0') {
    context.activation_token = activation_token;
  }
  g_free(activation_token);
  g_variant_unref(options);
  if (implementation->options.activated) {
    implementation->options.activated(action->id, context);
  }
}

static void subscribe_portal_activation(
    HotkeyBackendImplementation *implementation) {
  if (implementation->connection == nullptr ||
      implementation->portal_activation_signal_id != 0) {
    return;
  }
  implementation->portal_activation_signal_id =
      g_dbus_connection_signal_subscribe(
          implementation->connection, portal_bus_name,
          portal_shortcuts_interface, "Activated",
          portal_object_path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
          on_portal_activated, implementation, nullptr);
}

static void handle_portal_create_response(
    const std::shared_ptr<HotkeyBackendImplementation> &implementation,
    unsigned int generation, guint32 response, GVariant *results) {
  if (implementation->destroyed ||
      generation != implementation->generation ||
      implementation->portal_session_response_generation ==
          generation ||
      response != 0U) {
    return;
  }

  gchar *session_handle = nullptr;
  if (results == nullptr ||
      !g_variant_lookup(results, "session_handle", "s",
                        &session_handle) ||
      session_handle == nullptr || *session_handle == '\0') {
    g_free(session_handle);
    std::cerr << "Global shortcuts portal did not return a session\n";
    return;
  }
  implementation->portal_session_handle = session_handle;
  g_free(session_handle);
  implementation->portal_session_response_generation = generation;
  subscribe_portal_activation(implementation.get());
  implementation->tasks.emplace_back(
      bind_portal_shortcut_async(implementation, generation));
}

static void handle_portal_bind_response(
    const std::shared_ptr<HotkeyBackendImplementation> &implementation,
    unsigned int generation, guint32 response, GVariant *results) {
  if (implementation->destroyed ||
      generation != implementation->generation ||
      implementation->portal_bind_response_generation == generation ||
      response != 0U) {
    return;
  }
  GVariant *shortcuts =
      results == nullptr
          ? nullptr
          : g_variant_lookup_value(
                results, "shortcuts",
                G_VARIANT_TYPE("a(sa{sv})"));
  if (shortcuts == nullptr) {
    std::cerr << "Global shortcuts portal did not bind the hotkey\n";
    return;
  }

  std::vector<std::string> accepted_ids;
  GVariantIter iterator;
  g_variant_iter_init(&iterator, shortcuts);
  gchar *shortcut_id = nullptr;
  GVariant *properties = nullptr;
  while (g_variant_iter_next(&iterator, "(s@a{sv})",
                             &shortcut_id, &properties)) {
    if (shortcut_id != nullptr) {
      accepted_ids.emplace_back(shortcut_id);
    }
    g_free(shortcut_id);
    shortcut_id = nullptr;
    g_variant_unref(properties);
    properties = nullptr;
  }
  g_variant_unref(shortcuts);
  const bool all_accepted = std::all_of(
      implementation->actions.begin(),
      implementation->actions.end(),
      [&accepted_ids](const HotkeyAction &action) {
        if (!build_portal_shortcut_trigger(action.binding)
                 .has_value()) {
          return true;
        }
        return std::find(accepted_ids.begin(), accepted_ids.end(),
                         action.id) != accepted_ids.end();
      });
  if (all_accepted && !accepted_ids.empty()) {
    implementation->portal_bind_response_generation = generation;
  } else {
    std::cerr << "Global shortcuts portal rejected one or more "
                 "hotkey actions\n";
  }
}

static void on_portal_request_response(
    GDBusConnection *, const gchar *, const gchar *, const gchar *,
    const gchar *, GVariant *parameters, gpointer user_data) {
  auto *context = static_cast<PortalRequestContext *>(user_data);
  if (context == nullptr) {
    return;
  }
  const std::shared_ptr<HotkeyBackendImplementation> implementation =
      context->implementation.lock();
  if (implementation == nullptr || implementation->destroyed ||
      context->generation != implementation->generation) {
    return;
  }

  guint32 response = 0;
  GVariant *results = nullptr;
  g_variant_get(parameters, "(u@a{sv})", &response, &results);
  if (context->kind == PortalRequestKind::create_session) {
    handle_portal_create_response(implementation, context->generation,
                                  response, results);
  } else {
    handle_portal_bind_response(implementation, context->generation,
                                response, results);
  }
  g_variant_unref(results);
}

static bool subscribe_portal_request(
    const std::shared_ptr<HotkeyBackendImplementation> &implementation,
    const std::string &request_handle, unsigned int generation,
    PortalRequestKind kind) {
  if (implementation->connection == nullptr) {
    return false;
  }
  auto *context = new PortalRequestContext{
      .implementation = implementation,
      .generation = generation,
      .kind = kind,
  };
  const guint signal_id = g_dbus_connection_signal_subscribe(
      implementation->connection, portal_bus_name,
      portal_request_interface, "Response", request_handle.c_str(),
      nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
      on_portal_request_response, context,
      free_portal_request_context);
  if (signal_id == 0) {
    delete context;
    return false;
  }
  implementation->portal_request_signal_ids.push_back(signal_id);
  return true;
}

static cardio::promise<void> create_portal_session_async(
    std::shared_ptr<HotkeyBackendImplementation> implementation,
    unsigned int generation) {
  try {
    if (implementation->destroyed ||
        generation != implementation->generation ||
        implementation->actions.empty()) {
      co_return;
    }
    const std::string request_token =
        portal_token("elder_terms_hotkeys_request_", generation);
    const std::string session_token =
        portal_token("elder_terms_hotkeys_session_", generation);
    const std::string expected_handle = portal_request_handle(
        implementation->connection, request_token);
    const bool subscribed_expected = subscribe_portal_request(
        implementation, expected_handle, generation,
        PortalRequestKind::create_session);

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(
        &options, "{sv}", "handle_token",
        g_variant_new_string(request_token.c_str()));
    g_variant_builder_add(
        &options, "{sv}", "session_handle_token",
        g_variant_new_string(session_token.c_str()));
    GVariant *result = co_await call_dbus_async(
        implementation->connection, portal_bus_name,
        portal_object_path, portal_shortcuts_interface,
        "CreateSession", g_variant_new("(a{sv})", &options),
        G_VARIANT_TYPE("(o)"),
        implementation->cancellation_source->get_cancellation());
    const gchar *returned_handle = nullptr;
    g_variant_get(result, "(&o)", &returned_handle);
    if (!implementation->destroyed &&
        generation == implementation->generation &&
        returned_handle != nullptr &&
        (!subscribed_expected || expected_handle != returned_handle) &&
        !subscribe_portal_request(
            implementation, returned_handle, generation,
            PortalRequestKind::create_session)) {
      std::cerr << "Failed to observe the global shortcuts session response\n";
    }
    g_variant_unref(result);
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) {
    if (!implementation->destroyed &&
        generation == implementation->generation) {
      std::cerr << "Failed to create a global shortcuts session: "
                << error.what() << '\n';
    }
  }
}

static cardio::promise<void> bind_portal_shortcut_async(
    std::shared_ptr<HotkeyBackendImplementation> implementation,
    unsigned int generation) {
  try {
    if (implementation->destroyed ||
        generation != implementation->generation ||
        implementation->portal_session_handle.empty() ||
        implementation->actions.empty()) {
      co_return;
    }

    GVariantBuilder shortcuts;
    g_variant_builder_init(&shortcuts,
                           G_VARIANT_TYPE("a(sa{sv})"));
    bool has_shortcuts = false;
    for (const HotkeyAction &action : implementation->actions) {
      const std::optional<std::string> trigger =
          build_portal_shortcut_trigger(action.binding);
      if (!trigger.has_value()) {
        continue;
      }
      GVariantBuilder properties;
      g_variant_builder_init(&properties,
                             G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(
          &properties, "{sv}", "description",
          g_variant_new_string(action.description.c_str()));
      g_variant_builder_add(
          &properties, "{sv}", "preferred_trigger",
          g_variant_new_string(trigger->c_str()));
      g_variant_builder_add(
          &shortcuts, "(s@a{sv})", action.id.c_str(),
          g_variant_builder_end(&properties));
      has_shortcuts = true;
    }
    if (!has_shortcuts) {
      co_return;
    }

    const std::string request_token =
        portal_token("elder_terms_hotkeys_bind_", generation);
    const std::string expected_handle = portal_request_handle(
        implementation->connection, request_token);
    const bool subscribed_expected = subscribe_portal_request(
        implementation, expected_handle, generation,
        PortalRequestKind::bind_shortcuts);

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(
        &options, "{sv}", "handle_token",
        g_variant_new_string(request_token.c_str()));
    GVariant *result = co_await call_dbus_async(
        implementation->connection, portal_bus_name,
        portal_object_path, portal_shortcuts_interface,
        "BindShortcuts",
        g_variant_new(
            "(oa(sa{sv})sa{sv})",
            implementation->portal_session_handle.c_str(), &shortcuts,
            "", &options),
        G_VARIANT_TYPE("(o)"),
        implementation->cancellation_source->get_cancellation());
    const gchar *returned_handle = nullptr;
    g_variant_get(result, "(&o)", &returned_handle);
    if (!implementation->destroyed &&
        generation == implementation->generation &&
        returned_handle != nullptr &&
        (!subscribed_expected || expected_handle != returned_handle) &&
        !subscribe_portal_request(
            implementation, returned_handle, generation,
            PortalRequestKind::bind_shortcuts)) {
      std::cerr << "Failed to observe the global shortcuts bind response\n";
    }
    g_variant_unref(result);
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) {
    if (!implementation->destroyed &&
        generation == implementation->generation) {
      std::cerr << "Failed to bind global hotkey actions: "
                << error.what() << '\n';
    }
  }
}

static void start_portal_registration(
    const std::shared_ptr<HotkeyBackendImplementation> &implementation) {
  clear_portal_backend(implementation.get());
  if (implementation->actions.empty() ||
      implementation->destroyed) {
    return;
  }
  implementation->tasks.emplace_back(
      create_portal_session_async(implementation,
                                  implementation->generation));
}

static cardio::promise<void> initialize_portal_or_fallback_async(
    std::shared_ptr<HotkeyBackendImplementation> implementation,
    bool prefer_portal, bool has_x11) {
  try {
    const bool has_portal =
        implementation->connection != nullptr &&
        co_await portal_is_available_async(
            implementation.get(),
            implementation->cancellation_source->get_cancellation());
    if (implementation->destroyed) {
      co_return;
    }
    const HotkeyBackendKind selected =
        select_hotkey_backend_kind({
            .prefer_portal = prefer_portal,
            .has_portal = has_portal,
            .has_x11 = has_x11,
        });
    if (selected == HotkeyBackendKind::portal) {
      implementation->kind = selected;
      start_portal_registration(implementation);
    } else if (selected == HotkeyBackendKind::x11) {
      if (!initialize_x11_backend(implementation)) {
        implementation->kind = HotkeyBackendKind::none;
      }
    } else {
      implementation->kind = selected;
    }
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) {
    if (!implementation->destroyed) {
      std::cerr << "Global shortcuts portal is unavailable: "
                << error.what() << '\n';
      if (has_x11 &&
          !initialize_x11_backend(implementation)) {
        implementation->kind = HotkeyBackendKind::none;
      }
    }
  }
}

HotkeyBackendState *
create_hotkey_backend(HotkeyBackendOptions options,
                      const std::vector<HotkeyAction> &actions) {
  auto implementation =
      std::make_shared<HotkeyBackendImplementation>();
  implementation->options = std::move(options);
  implementation->actions = actions;
  implementation->cancellation_source.emplace();
  implementation->connection = g_application_get_dbus_connection(
      implementation->options.application);
  if (implementation->connection != nullptr) {
    g_object_ref(implementation->connection);
  }

  auto *state = new HotkeyBackendState{
      .implementation = implementation,
  };
  const bool prefer_portal = should_prefer_portal();
  const bool has_x11 = x11_is_available();
  if (!prefer_portal && has_x11) {
    if (!initialize_x11_backend(implementation)) {
      implementation->kind = HotkeyBackendKind::none;
    }
    return state;
  }
  if (implementation->options.dispatcher == nullptr) {
    if (has_x11 &&
        !initialize_x11_backend(implementation)) {
      implementation->kind = HotkeyBackendKind::none;
    }
    return state;
  }
  implementation->tasks.emplace_back(
      initialize_portal_or_fallback_async(
          implementation, prefer_portal, has_x11));
  return state;
}

void replace_hotkey_actions(
    HotkeyBackendState *state,
    const std::vector<HotkeyAction> &actions) {
  if (state == nullptr || state->implementation->destroyed) {
    return;
  }
  const std::shared_ptr<HotkeyBackendImplementation> implementation =
      state->implementation;
  ++implementation->generation;
  implementation->actions = actions;
  if (implementation->kind == HotkeyBackendKind::x11) {
    grab_x11_actions(implementation.get());
  } else if (implementation->kind ==
             HotkeyBackendKind::portal) {
    start_portal_registration(implementation);
  }
}

void destroy_hotkey_backend(HotkeyBackendState *state) {
  if (state == nullptr) {
    return;
  }
  const std::shared_ptr<HotkeyBackendImplementation> implementation =
      state->implementation;
  implementation->destroyed = true;
  ++implementation->generation;
  if (implementation->cancellation_source.has_value()) {
    (void)implementation->cancellation_source->cancel();
  }
  clear_portal_backend(implementation.get());
  destroy_x11_backend(implementation.get());
  g_clear_object(&implementation->connection);
  implementation->kind = HotkeyBackendKind::none;
  delete state;
}

HotkeyBackendKind
hotkey_backend_kind(const HotkeyBackendState *state) {
  return state == nullptr ? HotkeyBackendKind::none
                          : state->implementation->kind;
}

} // namespace elder_terms

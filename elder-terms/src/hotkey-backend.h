#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <cardio.h>
#include <gio/gio.h>

#include <elder-terms/key-binding.h>

namespace elder_terms {

/**
 * Identifies the runtime transport used for global hotkey actions.
 */
enum class HotkeyBackendKind {
  /** No global hotkey transport is available. */
  none,
  /** X11 passive key grab. */
  x11,
  /** XDG Desktop Portal GlobalShortcuts session. */
  portal,
};

/**
 * Describes the hotkey transports available in the current session.
 */
struct HotkeyBackendAvailability {
  /** Whether the current session should prefer the portal. */
  bool prefer_portal;
  /** Whether the GlobalShortcuts portal is available. */
  bool has_portal;
  /** Whether the current GDK display supports X11 grabs. */
  bool has_x11;
};

/**
 * Carries platform focus context for one global hotkey activation.
 */
struct HotkeyActivationContext {
  /** Event timestamp when provided by the platform. */
  std::optional<std::uint32_t> activation_time;
  /** Wayland activation token when provided by the portal. */
  std::optional<std::string> activation_token;
};

/**
 * Describes one desktop-wide hotkey action.
 */
struct HotkeyAction {
  /** Stable identifier returned when the action is activated. */
  std::string id;
  /** User-visible description passed to registration backends. */
  std::string description;
  /** Key combination registered for the action. */
  KeyBinding binding;
};

/** Callback invoked when a global hotkey action is activated. */
using HotkeyActivationCallback =
    std::function<void(const std::string &,
                       const HotkeyActivationContext &)>;

/**
 * Configures a global hotkey backend.
 */
struct HotkeyBackendOptions {
  /** Registered application that owns the session-bus connection. */
  GApplication *application;
  /** Cardio dispatcher integrated into the GLib main loop. */
  cardio::dispatcher *dispatcher;
  /** Receives global hotkey activation events. */
  HotkeyActivationCallback activated;
};

/** Opaque global hotkey backend state. */
struct HotkeyBackendState;

/**
 * Chooses the runtime hotkey transport.
 *
 * @param availability Available transports and session preference.
 * @returns Portal for a preferred Wayland session, otherwise X11, then portal,
 * then none.
 */
HotkeyBackendKind
select_hotkey_backend_kind(const HotkeyBackendAvailability &availability);

/**
 * Converts a parsed key binding to the portal preferred-trigger syntax.
 *
 * @param binding Valid global key binding.
 * @returns Portal trigger, or no value for an unsupported binding.
 */
std::optional<std::string>
build_portal_shortcut_trigger(const KeyBinding &binding);

/**
 * Finds the first action matching a key event.
 *
 * @param actions Ordered hotkey actions.
 * @param keyval GDK key value from the event.
 * @param modifiers Raw modifier state from the event.
 * @returns First matching action identifier, or no value.
 */
std::optional<std::string>
find_hotkey_action_id(const std::vector<HotkeyAction> &actions,
                      guint keyval, GdkModifierType modifiers);

/**
 * Creates and starts a global hotkey backend.
 *
 * @param options Application, dispatcher, and activation callback.
 * @param actions Initial ordered actions. An empty list disables
 * registration.
 * @returns Opaque backend state.
 */
HotkeyBackendState *
create_hotkey_backend(HotkeyBackendOptions options,
                      const std::vector<HotkeyAction> &actions);

/**
 * Replaces all active global hotkey actions.
 *
 * @param state Backend state.
 * @param actions New ordered actions, or an empty list to disable them.
 */
void replace_hotkey_actions(
    HotkeyBackendState *state,
    const std::vector<HotkeyAction> &actions);

/**
 * Stops hotkey registration and releases backend resources.
 *
 * @param state Backend state, or null.
 */
void destroy_hotkey_backend(HotkeyBackendState *state);

/**
 * Returns the currently selected runtime transport.
 *
 * @param state Backend state, or null.
 * @returns Selected transport.
 */
HotkeyBackendKind
hotkey_backend_kind(const HotkeyBackendState *state);

} // namespace elder_terms

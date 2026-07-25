#include "hotkey-backend.h"

#include <iostream>
#include <optional>
#include <string>

#include <gdk/gdk.h>

static bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

int main() {
  if (!expect(
          elder_terms::select_hotkey_backend_kind({
              .prefer_portal = true,
              .has_portal = true,
              .has_x11 = true,
          }) == elder_terms::HotkeyBackendKind::portal,
          "Wayland sessions should prefer the global shortcuts portal")) {
    return 1;
  }
  if (!expect(
          elder_terms::select_hotkey_backend_kind({
              .prefer_portal = true,
              .has_portal = false,
              .has_x11 = true,
          }) == elder_terms::HotkeyBackendKind::x11,
          "X11 should be used when the preferred portal is unavailable")) {
    return 1;
  }
  if (!expect(
          elder_terms::select_hotkey_backend_kind({
              .prefer_portal = false,
              .has_portal = true,
              .has_x11 = true,
          }) == elder_terms::HotkeyBackendKind::x11,
          "X11 sessions should use the X11 backend")) {
    return 1;
  }
  if (!expect(
          elder_terms::select_hotkey_backend_kind({
              .prefer_portal = false,
              .has_portal = false,
              .has_x11 = false,
          }) == elder_terms::HotkeyBackendKind::none,
          "hotkeys should be disabled when no backend is available")) {
    return 1;
  }

  const elder_terms::KeyBinding ctrl_alt_t{
      .keyval = GDK_KEY_t,
      .modifiers = static_cast<GdkModifierType>(
          GDK_CONTROL_MASK | GDK_MOD1_MASK),
  };
  if (!expect(
          elder_terms::build_portal_shortcut_trigger(ctrl_alt_t) ==
              std::optional<std::string>("CTRL+ALT+t"),
          "portal trigger should preserve the configured modifiers and key")) {
    return 1;
  }
  const elder_terms::KeyBinding shift_f2{
      .keyval = GDK_KEY_F2,
      .modifiers = GDK_SHIFT_MASK,
  };
  if (!expect(
          elder_terms::build_portal_shortcut_trigger(shift_f2) ==
              std::optional<std::string>("SHIFT+F2"),
          "portal trigger should use XKB key names")) {
    return 1;
  }
  const elder_terms::KeyBinding unmodified{
      .keyval = GDK_KEY_t,
      .modifiers = static_cast<GdkModifierType>(0),
  };
  if (!expect(
          !elder_terms::build_portal_shortcut_trigger(unmodified)
               .has_value(),
          "unmodified keys should not become global shortcuts")) {
    return 1;
  }

  return 0;
}

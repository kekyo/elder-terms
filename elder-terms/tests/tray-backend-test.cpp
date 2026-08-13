#include "tray-backend.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <gio/gio.h>

static bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

int main() {
  if (!expect(
          std::string(elder_terms::launcher_application_id()) ==
              "net.kekyo.elder-terms",
          "application id should match the configured reverse-DNS name")) {
    return 1;
  }
  if (!expect(
          g_application_id_is_valid(elder_terms::launcher_application_id()),
          "application id should be valid for GApplication")) {
    return 1;
  }
  if (!expect(
          elder_terms::select_tray_backend_kind(
              {.has_status_notifier_item = true, .has_xembed = true}) ==
              elder_terms::TrayBackendKind::status_notifier_item,
          "StatusNotifierItem should take priority over XEmbed")) {
    return 1;
  }
  if (!expect(
          elder_terms::select_tray_backend_kind(
              {.has_status_notifier_item = false, .has_xembed = true}) ==
              elder_terms::TrayBackendKind::xembed,
          "XEmbed should be selected when StatusNotifierItem is unavailable")) {
    return 1;
  }
  if (!expect(
          elder_terms::select_tray_backend_kind(
              {.has_status_notifier_item = false, .has_xembed = false}) ==
              elder_terms::TrayBackendKind::none,
          "no tray backend should be selected when no transport is available")) {
    return 1;
  }
  if (!expect(
          elder_terms::tray_backend_availability(nullptr) ==
              elder_terms::TrayBackendAvailabilityState::unavailable,
          "a missing tray backend should be unavailable")) {
    return 1;
  }

  const elder_terms::TrayActivationContext timed =
      elder_terms::build_tray_activation_context(1234U);
  if (!expect(
          timed.activation_time == std::optional<std::uint32_t>(1234U),
          "tray activation should preserve a non-zero event timestamp")) {
    return 1;
  }
  const elder_terms::TrayActivationContext untimed =
      elder_terms::build_tray_activation_context(0U);
  if (!expect(!untimed.activation_time.has_value(),
              "zero tray timestamps should be treated as unavailable")) {
    return 1;
  }

  const std::uint8_t rgba_pixel[] = {0x11U, 0x22U, 0x33U, 0x44U};
  const std::vector<std::uint8_t> argb_pixel =
      elder_terms::convert_tray_icon_pixels_to_argb(rgba_pixel, 1, 1, 4, 4);
  if (!expect(
          argb_pixel ==
              std::vector<std::uint8_t>{0x44U, 0x11U, 0x22U, 0x33U},
          "RGBA pixels should be converted to StatusNotifierItem ARGB order")) {
    return 1;
  }

  GVariant *pixmap_variant = elder_terms::build_tray_icon_pixmap_variant(
      {{.width = 1, .height = 1, .argb_pixels = argb_pixel}});
  const bool pixmap_matches =
      g_variant_is_of_type(pixmap_variant, G_VARIANT_TYPE("a(iiay)")) &&
      g_variant_n_children(pixmap_variant) == 1;
  g_variant_unref(pixmap_variant);
  if (!expect(pixmap_matches,
              "tray pixmaps should be exposed as one a(iiay) entry")) {
    return 1;
  }

  return 0;
}

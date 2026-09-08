#include "../../src/activity-indicator-id.h"
#include "../../src/activity-indicator.h"
#include "../../src/indicator-color.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace elder_terms {

static bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "activity-indicator-test: FAIL: " << message << '\n';
    return false;
  }
  return true;
}

static bool colored_images_preserve_shading_alpha_and_source_pixels() {
  const std::array<RgbColor, 4> colors{
      RgbColor{.red = 255, .green = 0, .blue = 0},
      RgbColor{.red = 0, .green = 0, .blue = 255},
      RgbColor{.red = 0, .green = 0, .blue = 0},
      RgbColor{.red = 255, .green = 255, .blue = 255},
  };
  constexpr guint8 original[3][4] = {
      {92, 167, 68, 201}, {88, 88, 88, 97}, {240, 240, 240, 0},
  };
  for (const bool alpha : {false, true}) {
    auto *source = gdk_pixbuf_new(GDK_COLORSPACE_RGB, alpha, 8, 1, 3);
    if (!expect(source != nullptr, "test pixbuf should allocate")) return false;
    const auto stride = gdk_pixbuf_get_rowstride(source);
    const auto channels = gdk_pixbuf_get_n_channels(source);
    auto *pixels = gdk_pixbuf_get_pixels(source);
    // Include inter-row padding, but never assume trailing padding exists.
    std::fill_n(pixels, 2 * stride + channels, guint8{0});
    for (int row = 0; row < 3; ++row) {
      std::copy_n(original[row], channels, pixels + row * stride);
    }
    for (const auto &color : colors) {
      auto *colored = create_colored_indicator_pixbuf(source, color);
      if (!expect(colored != nullptr, "a colored copy should allocate")) {
        g_object_unref(source);
        return false;
      }
      const auto colored_stride = gdk_pixbuf_get_rowstride(colored);
      const auto *result = gdk_pixbuf_get_pixels(colored);
      bool valid = expect(gdk_pixbuf_get_width(colored) == 1 &&
                              gdk_pixbuf_get_height(colored) == 3 &&
                              gdk_pixbuf_get_n_channels(colored) == channels,
                          "coloring should preserve image dimensions and channels");
      for (int row = 0; row < 3; ++row) {
        valid = expect(std::equal(original[row], original[row] + channels,
                                   pixels + row * stride),
                        "coloring should never modify the source") && valid;
        if (alpha) {
          valid = expect(result[row * colored_stride + 3] == original[row][3],
                          "coloring should preserve each alpha sample") && valid;
        }
      }
      const int lit = std::max({result[0], result[1], result[2]});
      const auto *dark_pixel = result + colored_stride;
      const int dark = std::max({dark_pixel[0], dark_pixel[1], dark_pixel[2]});
      const bool black = color.red == 0 && color.green == 0 && color.blue == 0;
      valid = expect(lit - dark > (black ? 10 : 40),
                      "lit and dark lamps should remain distinguishable, including black") && valid;
      if (color.red == color.green && color.green == color.blue) {
        valid = expect(result[0] == result[1] && result[1] == result[2] &&
                            dark_pixel[0] == dark_pixel[1] && dark_pixel[1] == dark_pixel[2],
                        "neutral colors should not retain the original green hue") && valid;
      } else {
        const auto channel = color.red == 255 ? 0 : 2;
        valid = expect(result[channel] - result[1] > 30 &&
                            dark_pixel[channel] - dark_pixel[1] > 30,
                        "both lit and dark pixels should acquire the requested hue") && valid;
      }
      const auto *highlight = result + 2 * colored_stride;
      valid = expect(std::min({highlight[0], highlight[1], highlight[2]}) > dark,
                      "neutral highlights should remain visible") && valid;
      g_object_unref(colored);
      if (!valid) {
        g_object_unref(source);
        return false;
      }
    }
    g_object_unref(source);
  }
  return true;
}

static bool replacing_images_preserves_steady_blink_and_latched_activity() {
  std::array<GdkPixbuf *, 4> icons{};
  bool allocated = true;
  for (auto *&icon : icons) {
    icon = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 1, 1);
    if (icon == nullptr) allocated = false;
    else std::fill_n(gdk_pixbuf_get_pixels(icon), 4, guint8{255});
  }
  bool valid = expect(allocated, "test icons should allocate");
  if (allocated) {
    for (const auto mode : {ActivityIndicatorMode::steady, ActivityIndicatorMode::blink}) {
      for (const bool latch : {false, true}) {
        if (mode == ActivityIndicatorMode::steady && latch) continue;
        for (const bool active : {false, true}) {
          ActivityIndicatorWidget indicator;
          initialize_activity_indicator_widget(&indicator, nullptr, icons[0], icons[1], mode);
          set_activity_indicator_widget_latched(&indicator, latch);
          if (active) note_activity_indicator_widget(&indicator);
          const auto before = indicator.blink_state;
          const auto timer = indicator.blink_timeout_id;
          const auto steady = indicator.steady_active;
          replace_activity_indicator_widget_images(&indicator, icons[2], icons[3]);
          valid = expect(indicator.blink_state == before &&
                              indicator.blink_timeout_id == timer &&
                              indicator.steady_active == steady &&
                              indicator.latch_activity == latch &&
                              indicator.on_icon == icons[2] && indicator.off_icon == icons[3],
                          "new images should retain current state, timer, and latch") && valid;
          if (mode == ActivityIndicatorMode::blink && active && !latch) {
            advance_activity_indicator_blink(indicator.blink_state);
            note_activity_indicator_widget(&indicator);
            const auto pending = indicator.blink_state;
            replace_activity_indicator_widget_images(&indicator, icons[0], icons[1]);
            valid = expect(indicator.blink_state == pending && !pending.active &&
                                pending.pending_activity && indicator.blink_timeout_id == timer,
                            "changing images in a dark phase should retain pending activity") && valid;
            advance_activity_indicator_blink(indicator.blink_state);
            valid = expect(indicator.blink_state.active,
                            "pending activity should still light the next blink phase") && valid;
          }
          release_activity_indicator_widget(&indicator);
        }
      }
    }
  }
  for (auto *icon : icons) {
    if (icon != nullptr) g_object_unref(icon);
  }
  return valid;
}

static bool blink_state_matches_predecessor_sequence() {
  if (!expect(activity_indicator_blink_period_ms() == 150,
              "the activity indicator blink period should be 150ms")) {
    return false;
  }

  ActivityIndicatorBlinkState blink_state;
  note_activity_indicator_blink(blink_state);
  if (!expect(
          blink_state == ActivityIndicatorBlinkState{
                             .active = true,
                             .running = true,
                             .pending_activity = false,
                         },
          "the first activity event should light the indicator and start the blink machine")) {
    return false;
  }

  note_activity_indicator_blink(blink_state);
  if (!expect(
          blink_state == ActivityIndicatorBlinkState{
                             .active = true,
                             .running = true,
                             .pending_activity = false,
                         },
          "activity during the lit phase should not alter the current cycle")) {
    return false;
  }

  if (!expect(
          advance_activity_indicator_blink(blink_state) &&
              blink_state == ActivityIndicatorBlinkState{
                                 .active = false,
                                 .running = true,
                                 .pending_activity = false,
                             },
          "the first timer tick should always switch the indicator off")) {
    return false;
  }

  note_activity_indicator_blink(blink_state);
  if (!expect(
          blink_state == ActivityIndicatorBlinkState{
                             .active = false,
                             .running = true,
                             .pending_activity = true,
                         },
          "activity during the dark phase should be recorded for the next cycle")) {
    return false;
  }

  note_activity_indicator_blink(blink_state);
  if (!expect(
          blink_state == ActivityIndicatorBlinkState{
                             .active = false,
                             .running = true,
                             .pending_activity = true,
                         },
          "repeated dark-phase activity should keep only the pending flag")) {
    return false;
  }

  if (!expect(
          advance_activity_indicator_blink(blink_state) &&
              blink_state == ActivityIndicatorBlinkState{
                                 .active = true,
                                 .running = true,
                                 .pending_activity = false,
                             },
          "the next dark-phase tick should relight the indicator when activity was recorded")) {
    return false;
  }

  if (!expect(
          advance_activity_indicator_blink(blink_state) &&
              blink_state == ActivityIndicatorBlinkState{
                                 .active = false,
                                 .running = true,
                                 .pending_activity = false,
                             },
          "after relighting, the next tick should switch back to the dark phase")) {
    return false;
  }

  if (!expect(!advance_activity_indicator_blink(blink_state) &&
                  blink_state == ActivityIndicatorBlinkState{},
              "the blink machine should stop after a dark phase with no recorded activity")) {
    return false;
  }

  return true;
}

static bool indicator_labels_are_stable() {
  return expect(activity_indicator_count() == 10,
                "activity indicator count should include CONN, LOG, and serial lines") &&
         expect(std::string_view(activity_indicator_label(
                    ActivityIndicatorId::conn)) == "CONN",
                "CONN label should stay stable") &&
         expect(activity_indicator_index(ActivityIndicatorId::conn) == 0,
                "CONN should be the first activity indicator") &&
         expect(std::string_view(activity_indicator_label(
                    ActivityIndicatorId::log)) == "LOG",
                "LOG label should stay stable") &&
         expect(activity_indicator_index(ActivityIndicatorId::log) == 1,
                "LOG should be shown after CONN") &&
         expect(std::string_view(activity_indicator_label(
                    ActivityIndicatorId::sd)) == "SD",
                "SD label should stay stable") &&
         expect(activity_indicator_index(ActivityIndicatorId::sd) == 2,
                "SD should be shown after LOG") &&
         expect(std::string_view(activity_indicator_label(
                    ActivityIndicatorId::rd)) == "RD",
                "RD label should stay stable") &&
         expect(activity_indicator_index(ActivityIndicatorId::rd) == 3,
                "RD should be shown after SD") &&
         expect(std::string_view(activity_indicator_label(
                    ActivityIndicatorId::rts)) == "RTS",
                "RTS label should stay stable") &&
         expect(std::string_view(activity_indicator_label(
                    ActivityIndicatorId::cts)) == "CTS",
                "CTS label should stay stable") &&
         expect(std::string_view(activity_indicator_label(
                    ActivityIndicatorId::dtr)) == "DTR",
                "DTR label should stay stable") &&
         expect(std::string_view(activity_indicator_label(
                    ActivityIndicatorId::dsr)) == "DSR",
                "DSR label should stay stable") &&
         expect(std::string_view(activity_indicator_label(
                    ActivityIndicatorId::cd)) == "CD",
                "CD label should stay stable") &&
         expect(std::string_view(activity_indicator_label(
                    ActivityIndicatorId::ri)) == "RI",
                "RI label should stay stable") &&
         expect(!activity_indicator_is_serial_line(ActivityIndicatorId::conn),
                "CONN should not be a serial-only indicator") &&
         expect(!activity_indicator_is_serial_line(ActivityIndicatorId::log),
                "LOG should not be a serial-only indicator") &&
         expect(!activity_indicator_is_serial_line(ActivityIndicatorId::sd),
                "SD should not be a serial-only indicator") &&
         expect(activity_indicator_is_serial_line(ActivityIndicatorId::rts),
                "RTS should be a serial-only indicator");
}

static bool steady_widget_state_does_not_start_a_blink_timer() {
  ActivityIndicatorWidget indicator;
  initialize_activity_indicator_widget(
      &indicator, nullptr, nullptr, nullptr, ActivityIndicatorMode::steady);

  note_activity_indicator_widget(&indicator);
  if (!expect(indicator.steady_active,
              "steady activity should light the indicator directly")) {
    return false;
  }
  if (!expect(indicator.blink_timeout_id == 0,
              "steady activity should not start a blink timer")) {
    return false;
  }

  set_activity_indicator_widget_active(&indicator, false);
  return expect(!indicator.steady_active,
                "explicit steady off should clear the lit state") &&
         expect(indicator.blink_state == ActivityIndicatorBlinkState{},
                "explicit steady off should clear blink state");
}

static bool latched_widget_activity_stays_lit_until_reset() {
  ActivityIndicatorWidget indicator;
  initialize_activity_indicator_widget(
      &indicator, nullptr, nullptr, nullptr, ActivityIndicatorMode::blink);
  set_activity_indicator_widget_latched(&indicator, true);
  note_activity_indicator_widget(&indicator);

  if (!expect(indicator.latch_activity && indicator.blink_state.active &&
                  indicator.blink_state.running &&
                  indicator.blink_timeout_id == 0,
              "latched activity should stay active without a timer")) {
    return false;
  }

  reset_activity_indicator_widget(&indicator);
  return expect(indicator.latch_activity && !indicator.blink_state.active &&
                    !indicator.blink_state.running &&
                    indicator.blink_timeout_id == 0,
                "reset should clear activity without disabling the latch");
}

} // namespace elder_terms

int main() {
  if (!elder_terms::colored_images_preserve_shading_alpha_and_source_pixels() ||
      !elder_terms::replacing_images_preserves_steady_blink_and_latched_activity()) {
    return 1;
  }
  if (!elder_terms::blink_state_matches_predecessor_sequence()) {
    return 1;
  }
  if (!elder_terms::indicator_labels_are_stable()) {
    return 1;
  }
  if (!elder_terms::steady_widget_state_does_not_start_a_blink_timer()) {
    return 1;
  }
  if (!elder_terms::latched_widget_activity_stays_lit_until_reset()) {
    return 1;
  }

  std::cout << "activity-indicator-test: PASS" << '\n';
  return 0;
}

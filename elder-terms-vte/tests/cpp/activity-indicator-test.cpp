#include "../../src/activity-indicator-id.h"
#include "../../src/activity-indicator.h"

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
  return expect(activity_indicator_count() == 9,
                "activity indicator count should include CONN and serial lines") &&
         expect(std::string_view(activity_indicator_label(
                    ActivityIndicatorId::conn)) == "CONN",
                "CONN label should stay stable") &&
         expect(activity_indicator_index(ActivityIndicatorId::conn) == 0,
                "CONN should be the first activity indicator") &&
         expect(std::string_view(activity_indicator_label(
                    ActivityIndicatorId::sd)) == "SD",
                "SD label should stay stable") &&
         expect(activity_indicator_index(ActivityIndicatorId::sd) == 1,
                "SD should be shown after CONN") &&
         expect(std::string_view(activity_indicator_label(
                    ActivityIndicatorId::rd)) == "RD",
                "RD label should stay stable") &&
         expect(activity_indicator_index(ActivityIndicatorId::rd) == 2,
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

} // namespace elder_terms

int main() {
  if (!elder_terms::blink_state_matches_predecessor_sequence()) {
    return 1;
  }
  if (!elder_terms::indicator_labels_are_stable()) {
    return 1;
  }
  if (!elder_terms::steady_widget_state_does_not_start_a_blink_timer()) {
    return 1;
  }

  std::cout << "activity-indicator-test: PASS" << '\n';
  return 0;
}

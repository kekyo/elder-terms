#pragma once

#include <array>
#include <cstddef>

namespace elder_terms {

/**
 * Stable status-bar activity indicator ids.
 */
enum class ActivityIndicatorId {
  /** Backend connection state. */
  conn,
  /** Sent data. */
  sd,
  /** Received data. */
  rd,
  /** Request to send modem line. */
  rts,
  /** Clear to send modem line. */
  cts,
  /** Data terminal ready modem line. */
  dtr,
  /** Data set ready modem line. */
  dsr,
  /** Carrier detect modem line. */
  cd,
  /** Ring indicator modem line. */
  ri,
};

/** Ordered activity indicators as displayed in the status bar. */
inline constexpr std::array<ActivityIndicatorId, 9> activity_indicator_ids{
    ActivityIndicatorId::conn, ActivityIndicatorId::sd,
    ActivityIndicatorId::rd,   ActivityIndicatorId::rts,
    ActivityIndicatorId::cts,  ActivityIndicatorId::dtr,
    ActivityIndicatorId::dsr,  ActivityIndicatorId::cd,
    ActivityIndicatorId::ri,
};

/**
 * Active/inactive state for one activity indicator.
 */
struct ActivityIndicatorState {
  /** Indicator id receiving the state. */
  ActivityIndicatorId indicator = ActivityIndicatorId::conn;
  /** True when the indicator should show the lit icon. */
  bool active = false;

  bool operator==(const ActivityIndicatorState &) const = default;
};

/**
 * Returns the number of supported activity indicators.
 *
 * @returns Activity indicator count.
 */
constexpr std::size_t activity_indicator_count() {
  return activity_indicator_ids.size();
}

/**
 * Returns the array index for one activity indicator.
 *
 * @param indicator Activity indicator id.
 * @returns Stable zero-based index.
 */
constexpr std::size_t activity_indicator_index(ActivityIndicatorId indicator) {
  switch (indicator) {
  case ActivityIndicatorId::conn:
    return 0;
  case ActivityIndicatorId::sd:
    return 1;
  case ActivityIndicatorId::rd:
    return 2;
  case ActivityIndicatorId::rts:
    return 3;
  case ActivityIndicatorId::cts:
    return 4;
  case ActivityIndicatorId::dtr:
    return 5;
  case ActivityIndicatorId::dsr:
    return 6;
  case ActivityIndicatorId::cd:
    return 7;
  case ActivityIndicatorId::ri:
    return 8;
  }
  return 0;
}

/**
 * Returns the lowercase token used in widget ids.
 *
 * @param indicator Activity indicator id.
 * @returns Stable lowercase token.
 */
constexpr const char *activity_indicator_token(ActivityIndicatorId indicator) {
  switch (indicator) {
  case ActivityIndicatorId::conn:
    return "conn";
  case ActivityIndicatorId::sd:
    return "sd";
  case ActivityIndicatorId::rd:
    return "rd";
  case ActivityIndicatorId::rts:
    return "rts";
  case ActivityIndicatorId::cts:
    return "cts";
  case ActivityIndicatorId::dtr:
    return "dtr";
  case ActivityIndicatorId::dsr:
    return "dsr";
  case ActivityIndicatorId::cd:
    return "cd";
  case ActivityIndicatorId::ri:
    return "ri";
  }
  return "";
}

/**
 * Returns the visible label for one activity indicator.
 *
 * @param indicator Activity indicator id.
 * @returns Uppercase label shown beneath the icon.
 */
constexpr const char *activity_indicator_label(ActivityIndicatorId indicator) {
  switch (indicator) {
  case ActivityIndicatorId::conn:
    return "CONN";
  case ActivityIndicatorId::sd:
    return "SD";
  case ActivityIndicatorId::rd:
    return "RD";
  case ActivityIndicatorId::rts:
    return "RTS";
  case ActivityIndicatorId::cts:
    return "CTS";
  case ActivityIndicatorId::dtr:
    return "DTR";
  case ActivityIndicatorId::dsr:
    return "DSR";
  case ActivityIndicatorId::cd:
    return "CD";
  case ActivityIndicatorId::ri:
    return "RI";
  }
  return "";
}

/**
 * Returns whether an indicator is visible only for serial sessions.
 *
 * @param indicator Activity indicator id.
 * @returns True for modem-line indicators.
 */
constexpr bool activity_indicator_is_serial_line(
    ActivityIndicatorId indicator) {
  return indicator != ActivityIndicatorId::conn &&
         indicator != ActivityIndicatorId::sd &&
         indicator != ActivityIndicatorId::rd;
}

} // namespace elder_terms

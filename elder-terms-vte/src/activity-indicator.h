#pragma once

#include <gtk/gtk.h>

namespace elder_terms {

/**
 * Runtime state for one activity-indicator blink sequence.
 */
struct ActivityIndicatorBlinkState {
  /** True while the indicator should show the lit icon. */
  bool active = false;
  /** True while the blink timer should keep advancing this state. */
  bool running = false;
  /** True when activity arrived during a dark phase. */
  bool pending_activity = false;

  bool operator==(const ActivityIndicatorBlinkState &) const = default;
};

/**
 * Rendering behavior for one activity indicator widget.
 */
enum class ActivityIndicatorMode {
  /** Activity events start the 150ms blink state machine. */
  blink,
  /** Explicit state updates directly select the lit or dark icon. */
  steady,
};

/**
 * GTK widgets and resources used to present one activity indicator.
 */
struct ActivityIndicatorWidget {
  /** Indicator rendering behavior. */
  ActivityIndicatorMode mode = ActivityIndicatorMode::blink;
  /** GtkImage receiving the current indicator pixbuf. */
  GtkWidget *image = nullptr;
  /** Pixbuf shown while the indicator is lit. */
  GdkPixbuf *on_icon = nullptr;
  /** Pixbuf shown while the indicator is dark. */
  GdkPixbuf *off_icon = nullptr;
  /** Active GLib timeout source, or 0 when no timer is running. */
  guint blink_timeout_id = 0;
  /** Current blink state. */
  ActivityIndicatorBlinkState blink_state;
  /** Current steady-mode state. */
  bool steady_active = false;
  /** True when test activity should remain lit until an explicit reset. */
  bool latch_activity = false;
};

/**
 * Returns the fixed activity-indicator blink phase duration.
 *
 * @returns Blink phase duration in milliseconds.
 */
unsigned int activity_indicator_blink_period_ms();

/**
 * Records one activity event in the blink state machine.
 *
 * @param state Blink state to update.
 */
void note_activity_indicator_blink(ActivityIndicatorBlinkState &state);

/**
 * Advances one blink state by a single timer tick.
 *
 * @param state Blink state to update.
 * @returns True while the timer should continue running.
 */
bool advance_activity_indicator_blink(ActivityIndicatorBlinkState &state);

/**
 * Initializes one GTK activity indicator widget.
 *
 * @param indicator Widget state to initialize.
 * @param image GtkImage controlled by this indicator.
 * @param on_icon Pixbuf used for the lit phase.
 * @param off_icon Pixbuf used for the dark phase.
 * @param mode Indicator rendering behavior.
 */
void initialize_activity_indicator_widget(ActivityIndicatorWidget *indicator,
                                          GtkWidget *image,
                                          GdkPixbuf *on_icon,
                                          GdkPixbuf *off_icon,
                                          ActivityIndicatorMode mode);

/**
 * Records activity and starts the indicator blink timer when needed.
 *
 * @param indicator Widget state to update.
 */
void note_activity_indicator_widget(ActivityIndicatorWidget *indicator);

/**
 * Controls whether blink activity remains lit until an explicit reset.
 *
 * @param indicator Widget state to update.
 * @param latch True to suppress the blink timer after activity.
 * @remarks This is intended for deterministic GTK integration tests. Normal
 * runtime behavior leaves this disabled and retains the fixed 150ms blink.
 */
void set_activity_indicator_widget_latched(ActivityIndicatorWidget *indicator,
                                           bool latch);

/**
 * Sets the explicit lit/dark state for a steady activity indicator.
 *
 * @param indicator Widget state to update.
 * @param active True to show the lit icon.
 */
void set_activity_indicator_widget_active(ActivityIndicatorWidget *indicator,
                                          bool active);

/**
 * Stops blinking and restores the dark icon.
 *
 * @param indicator Widget state to reset.
 */
void reset_activity_indicator_widget(ActivityIndicatorWidget *indicator);

/**
 * Stops timers and clears non-owning widget/resource references.
 *
 * @param indicator Widget state to release.
 */
void release_activity_indicator_widget(ActivityIndicatorWidget *indicator);

} // namespace elder_terms

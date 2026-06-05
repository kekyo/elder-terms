#include "activity-indicator.h"

namespace elder_terms {

static constexpr unsigned int blink_period_ms = 150;

static void set_indicator_active(ActivityIndicatorWidget *indicator,
                                 bool active) {
  if (indicator == nullptr || indicator->image == nullptr) {
    return;
  }

  GdkPixbuf *pixbuf = active ? indicator->on_icon : indicator->off_icon;
  if (pixbuf != nullptr) {
    gtk_image_set_from_pixbuf(GTK_IMAGE(indicator->image), pixbuf);
    return;
  }

  gtk_image_clear(GTK_IMAGE(indicator->image));
}

static void stop_activity_indicator_timer(ActivityIndicatorWidget *indicator) {
  if (indicator == nullptr || indicator->blink_timeout_id == 0) {
    return;
  }

  g_source_remove(indicator->blink_timeout_id);
  indicator->blink_timeout_id = 0;
}

static gboolean on_activity_indicator_timeout(gpointer data) {
  auto *indicator = static_cast<ActivityIndicatorWidget *>(data);
  if (indicator == nullptr) {
    return G_SOURCE_REMOVE;
  }

  if (!advance_activity_indicator_blink(indicator->blink_state)) {
    indicator->blink_timeout_id = 0;
    set_indicator_active(indicator, false);
    return G_SOURCE_REMOVE;
  }

  set_indicator_active(indicator, indicator->blink_state.active);
  return G_SOURCE_CONTINUE;
}

unsigned int activity_indicator_blink_period_ms() {
  return blink_period_ms;
}

void note_activity_indicator_blink(ActivityIndicatorBlinkState &state) {
  if (!state.running) {
    state.active = true;
    state.running = true;
    state.pending_activity = false;
    return;
  }
  if (!state.active) {
    state.pending_activity = true;
  }
}

bool advance_activity_indicator_blink(ActivityIndicatorBlinkState &state) {
  if (!state.running) {
    state = {};
    return false;
  }
  if (state.active) {
    state.active = false;
    return true;
  }
  if (!state.pending_activity) {
    state = {};
    return false;
  }

  state.active = true;
  state.pending_activity = false;
  return true;
}

void initialize_activity_indicator_widget(ActivityIndicatorWidget *indicator,
                                          GtkWidget *image,
                                          GdkPixbuf *on_icon,
                                          GdkPixbuf *off_icon,
                                          ActivityIndicatorMode mode) {
  if (indicator == nullptr) {
    return;
  }

  release_activity_indicator_widget(indicator);
  indicator->mode = mode;
  indicator->image = image;
  indicator->on_icon = on_icon;
  indicator->off_icon = off_icon;
  indicator->steady_active = false;
  set_indicator_active(indicator, false);
}

void note_activity_indicator_widget(ActivityIndicatorWidget *indicator) {
  if (indicator == nullptr) {
    return;
  }

  if (indicator->mode == ActivityIndicatorMode::steady) {
    set_activity_indicator_widget_active(indicator, true);
    return;
  }

  note_activity_indicator_blink(indicator->blink_state);
  set_indicator_active(indicator, indicator->blink_state.active);
  if (indicator->blink_timeout_id != 0) {
    return;
  }

  indicator->blink_timeout_id =
      g_timeout_add(activity_indicator_blink_period_ms(),
                    on_activity_indicator_timeout, indicator);
}

void set_activity_indicator_widget_active(ActivityIndicatorWidget *indicator,
                                          bool active) {
  if (indicator == nullptr) {
    return;
  }

  stop_activity_indicator_timer(indicator);
  indicator->blink_state = {};
  indicator->steady_active = active;
  set_indicator_active(indicator, active);
}

void reset_activity_indicator_widget(ActivityIndicatorWidget *indicator) {
  if (indicator == nullptr) {
    return;
  }

  set_activity_indicator_widget_active(indicator, false);
}

void release_activity_indicator_widget(ActivityIndicatorWidget *indicator) {
  if (indicator == nullptr) {
    return;
  }

  stop_activity_indicator_timer(indicator);
  indicator->blink_state = {};
  indicator->steady_active = false;
  indicator->image = nullptr;
  indicator->on_icon = nullptr;
  indicator->off_icon = nullptr;
}

} // namespace elder_terms

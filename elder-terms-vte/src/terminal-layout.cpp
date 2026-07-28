#include <vte/vte.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "terminal-layout.h"

namespace elder_terms {

static constexpr glong default_columns = 80;
static constexpr glong default_rows = 24;
static constexpr int minimum_columns = 4;
static constexpr int minimum_rows = 1;
static constexpr gdouble font_scale_step = 0.1;
static constexpr gdouble minimum_font_scale = 0.5;
static constexpr gdouble maximum_font_scale = 3.0;
static constexpr gdouble font_scale_epsilon = 0.0001;
static constexpr guint font_zoom_apply_timeout_ms = 16;
static constexpr guint font_resize_guard_timeout_ms = 50;
static constexpr guint window_size_sync_debounce_ms = 200;

struct TerminalGridPadding {
  int left = 0;
  int right = 0;
  int top = 0;
  int bottom = 0;
};

struct TerminalLayoutState {
  GtkWidget *window = nullptr;
  GtkWidget *root_box = nullptr;
  GtkWidget *terminal_scroller = nullptr;
  GtkWidget *terminal = nullptr;
  GtkWidget *terminal_scrollbar = nullptr;
  GtkWidget *status_bar = nullptr;
  GtkWidget *fixture_grid_size_label = nullptr;
  TestOptions options;
  TerminalLayoutCallbacks callbacks;
  TerminalKeyBindings key_bindings;
  GdkGeometry hints = {};
  GdkWindowState window_state = static_cast<GdkWindowState>(0);
  glong desired_columns = default_columns;
  glong desired_rows = default_rows;
  glong notified_columns = 0;
  glong notified_rows = 0;
  glong notified_display_columns = 0;
  glong notified_display_rows = 0;
  gdouble notified_display_zoom = 0.0;
  bool realized = false;
  bool update_pending = false;
  bool font_resize_guard_active = false;
  bool font_zoom_anchor_active = false;
  bool font_zoom_pending = false;
  guint window_size_sync_source = 0;
  guint font_resize_guard_source = 0;
  guint font_zoom_source = 0;
  gdouble pending_font_scale = 1.0;
  gdouble font_zoom_anchor_scale = 1.0;
  gdouble smooth_zoom_delta_y = 0.0;
  glong font_zoom_anchor_columns = default_columns;
  glong font_zoom_anchor_rows = default_rows;
  int old_char_width = -1;
  int old_char_height = -1;
  int old_chrome_width = -1;
  int old_chrome_height = -1;
  int old_csd_width = -1;
  int old_csd_height = -1;
  int old_terminal_extra_width = -1;
  int old_terminal_extra_height = -1;
};

static int clamped_non_negative(int value) {
  return value < 0 ? 0 : value;
}

static bool window_state_is_snapped(GdkWindowState state) {
  return (state & (GDK_WINDOW_STATE_FULLSCREEN | GDK_WINDOW_STATE_MAXIMIZED |
                   GDK_WINDOW_STATE_TILED | GDK_WINDOW_STATE_TOP_TILED |
                   GDK_WINDOW_STATE_RIGHT_TILED |
                   GDK_WINDOW_STATE_BOTTOM_TILED |
                   GDK_WINDOW_STATE_LEFT_TILED)) != 0;
}

static TerminalGridPadding terminal_grid_padding(GtkWidget *terminal) {
  GtkStyleContext *context = gtk_widget_get_style_context(terminal);
  const GtkStateFlags state = gtk_style_context_get_state(context);
  GtkBorder padding;
  GtkBorder border;
  gtk_style_context_get_padding(context, state, &padding);
  gtk_style_context_get_border(context, state, &border);

  return {
      .left = padding.left + border.left,
      .right = padding.right + border.right,
      .top = padding.top + border.top,
      .bottom = padding.bottom + border.bottom,
  };
}

static bool update_geometry(TerminalLayoutState *state) {
  if (gtk_widget_in_destruction(state->window)) {
    return false;
  }

  VteTerminal *terminal = VTE_TERMINAL(state->terminal);
  const glong columns = state->desired_columns;
  const glong rows = state->desired_rows;
  const int char_width = static_cast<int>(vte_terminal_get_char_width(terminal));
  const int char_height =
      static_cast<int>(vte_terminal_get_char_height(terminal));
  if (columns <= 0 || rows <= 0 || char_width <= 0 || char_height <= 0) {
    return false;
  }
  const TerminalGridPadding padding = terminal_grid_padding(state->terminal);
  const int terminal_extra_width = padding.left + padding.right;
  const int terminal_extra_height = padding.top + padding.bottom;

  int chrome_width = 0;
  int chrome_height = 0;
  int csd_width = 0;
  int csd_height = 0;
  if (state->realized) {
    GtkAllocation window_allocation;
    GtkAllocation root_allocation;
    GtkAllocation terminal_allocation;
    gtk_widget_get_allocation(state->window, &window_allocation);
    gtk_widget_get_allocation(state->root_box, &root_allocation);
    gtk_widget_get_allocation(state->terminal, &terminal_allocation);
    chrome_width =
        clamped_non_negative(root_allocation.width - terminal_allocation.width);
    chrome_height = clamped_non_negative(root_allocation.height -
                                         terminal_allocation.height);
    csd_width =
        clamped_non_negative(window_allocation.width - root_allocation.width);
    csd_height =
        clamped_non_negative(window_allocation.height - root_allocation.height);
  } else {
    GtkRequisition root_request;
    gtk_widget_get_preferred_size(state->root_box, nullptr, &root_request);
    chrome_width = clamped_non_negative(root_request.width -
                                        terminal_extra_width -
                                        char_width * columns);
    chrome_height = clamped_non_negative(root_request.height -
                                         terminal_extra_height -
                                         char_height * rows);
  }

  if (state->realized && !window_state_is_snapped(state->window_state)) {
    state->hints.base_width =
        chrome_width + terminal_extra_width + csd_width;
    state->hints.base_height =
        chrome_height + terminal_extra_height + csd_height;
    state->hints.width_inc = char_width;
    state->hints.height_inc = char_height;
    state->hints.min_width =
        state->hints.base_width + state->hints.width_inc * minimum_columns;
    state->hints.min_height =
        state->hints.base_height + state->hints.height_inc * minimum_rows;

    gtk_window_set_geometry_hints(
        GTK_WINDOW(state->window), nullptr, &state->hints,
        static_cast<GdkWindowHints>(GDK_HINT_RESIZE_INC | GDK_HINT_MIN_SIZE |
                                    GDK_HINT_BASE_SIZE));
  }

  state->old_char_width = char_width;
  state->old_char_height = char_height;
  state->old_chrome_width = chrome_width;
  state->old_chrome_height = chrome_height;
  state->old_csd_width = csd_width;
  state->old_csd_height = csd_height;
  state->old_terminal_extra_width = terminal_extra_width;
  state->old_terminal_extra_height = terminal_extra_height;

  return true;
}

static gboolean feed_fixture_idle(gpointer data);
static bool font_scale_matches(gdouble left, gdouble right);
static void start_font_resize_guard(TerminalLayoutState *state);

static int client_width_for_grid(TerminalLayoutState *state, glong columns) {
  return state->old_chrome_width + state->old_terminal_extra_width +
         static_cast<int>(columns) * state->old_char_width;
}

static int client_height_for_grid(TerminalLayoutState *state, glong rows) {
  return state->old_chrome_height + state->old_terminal_extra_height +
         static_cast<int>(rows) * state->old_char_height;
}

static int hinted_width_for_grid(TerminalLayoutState *state, glong columns) {
  return state->hints.base_width +
         static_cast<int>(columns) * state->hints.width_inc;
}

static int hinted_height_for_grid(TerminalLayoutState *state, glong rows) {
  return state->hints.base_height +
         static_cast<int>(rows) * state->hints.height_inc;
}

static bool font_grid_is_locked(TerminalLayoutState *state) {
  return state->font_resize_guard_active || state->font_zoom_pending ||
         state->font_zoom_source != 0;
}

static void update_fixture_grid_status(TerminalLayoutState *state) {
  if (!state->options.fixture || state->fixture_grid_size_label == nullptr) {
    return;
  }

  VteTerminal *terminal = VTE_TERMINAL(state->terminal);
  const glong columns = vte_terminal_get_column_count(terminal);
  const glong rows = vte_terminal_get_row_count(terminal);
  if (columns <= 0 || rows <= 0) {
    return;
  }

  const std::string label =
      std::to_string(columns) + "x" + std::to_string(rows);
  gtk_label_set_text(GTK_LABEL(state->fixture_grid_size_label), label.c_str());
}

static void notify_terminal_grid_size_changed(TerminalLayoutState *state,
                                              glong columns, glong rows) {
  if (columns <= 0 || rows <= 0 || !state->callbacks.grid_size_changed) {
    return;
  }
  if (state->notified_columns == columns && state->notified_rows == rows) {
    return;
  }

  state->notified_columns = columns;
  state->notified_rows = rows;
  state->callbacks.grid_size_changed(columns, rows);
}

static void notify_terminal_display_settings_changed(
    TerminalLayoutState *state, glong columns, glong rows) {
  if (columns <= 0 || rows <= 0 ||
      !state->callbacks.display_settings_changed) {
    return;
  }

  VteTerminal *terminal = VTE_TERMINAL(state->terminal);
  const gdouble zoom = vte_terminal_get_font_scale(terminal);
  if (state->notified_display_columns == columns &&
      state->notified_display_rows == rows &&
      font_scale_matches(state->notified_display_zoom, zoom)) {
    return;
  }

  state->notified_display_columns = columns;
  state->notified_display_rows = rows;
  state->notified_display_zoom = zoom;
  state->callbacks.display_settings_changed({
      .width = columns,
      .height = rows,
      .zoom = zoom,
  });
}

static void ensure_terminal_grid_size(TerminalLayoutState *state,
                                      glong columns, glong rows) {
  if (columns <= 0 || rows <= 0) {
    return;
  }

  VteTerminal *terminal = VTE_TERMINAL(state->terminal);
  if (columns != vte_terminal_get_column_count(terminal) ||
      rows != vte_terminal_get_row_count(terminal)) {
    vte_terminal_set_size(terminal, columns, rows);
  }
  update_fixture_grid_status(state);
  const glong current_columns = vte_terminal_get_column_count(terminal);
  const glong current_rows = vte_terminal_get_row_count(terminal);
  notify_terminal_grid_size_changed(state, current_columns, current_rows);
  notify_terminal_display_settings_changed(state, current_columns,
                                           current_rows);
}

static void resize_window_to_hinted_grid(TerminalLayoutState *state,
                                         glong columns, glong rows) {
  if (state->hints.width_inc <= 0 || state->hints.height_inc <= 0) {
    return;
  }

  GtkAllocation window_allocation;
  gtk_widget_get_allocation(state->window, &window_allocation);
  const int desired_width = hinted_width_for_grid(state, columns);
  const int desired_height = hinted_height_for_grid(state, rows);
  if (window_allocation.width == desired_width &&
      window_allocation.height == desired_height) {
    return;
  }

  GdkWindow *gdk_window = gtk_widget_get_window(state->window);
  if (gdk_window != nullptr) {
    gdk_window_resize(gdk_window, desired_width, desired_height);
  }
}

static void update_window_size(TerminalLayoutState *state) {
  state->update_pending = false;
  if (!update_geometry(state)) {
    return;
  }
  if (state->realized && window_state_is_snapped(state->window_state)) {
    return;
  }

  const int pixel_width =
      client_width_for_grid(state, state->desired_columns);
  const int pixel_height =
      client_height_for_grid(state, state->desired_rows);
  ensure_terminal_grid_size(state, state->desired_columns,
                            state->desired_rows);
  gtk_window_resize(GTK_WINDOW(state->window), pixel_width, pixel_height);
  if (state->options.fixture) {
    g_timeout_add(50, feed_fixture_idle, state);
  }
}

static gboolean update_window_size_idle(gpointer data) {
  update_window_size(static_cast<TerminalLayoutState *>(data));
  return G_SOURCE_REMOVE;
}

static void queue_window_size_update(TerminalLayoutState *state) {
  if (state->update_pending) {
    return;
  }

  state->update_pending = true;
  g_idle_add(update_window_size_idle, state);
}

static bool has_pending_font_zoom(TerminalLayoutState *state) {
  return state->font_zoom_pending || state->font_zoom_source != 0;
}

static bool font_scale_matches(gdouble left, gdouble right) {
  return std::abs(left - right) < font_scale_epsilon;
}

static gboolean clear_font_resize_guard(gpointer data) {
  TerminalLayoutState *state = static_cast<TerminalLayoutState *>(data);
  state->font_resize_guard_source = 0;
  if (has_pending_font_zoom(state)) {
    start_font_resize_guard(state);
    return G_SOURCE_REMOVE;
  }

  if (state->window_size_sync_source != 0) {
    g_source_remove(state->window_size_sync_source);
    state->window_size_sync_source = 0;
  }
  state->font_resize_guard_active = false;
  // A zoomed font may leave the window constrained to fewer visible cells.
  // Keep desired_* unchanged so zooming back can restore the previous grid.
  ensure_terminal_grid_size(state, state->desired_columns,
                            state->desired_rows);
  update_geometry(state);
  return G_SOURCE_REMOVE;
}

static void start_font_resize_guard(TerminalLayoutState *state) {
  state->font_resize_guard_active = true;
  if (state->font_resize_guard_source != 0) {
    g_source_remove(state->font_resize_guard_source);
  }
  state->font_resize_guard_source =
      g_timeout_add(font_resize_guard_timeout_ms, clear_font_resize_guard,
                    state);
}

static gboolean apply_pending_font_scale(gpointer data) {
  TerminalLayoutState *state = static_cast<TerminalLayoutState *>(data);
  state->font_zoom_source = 0;
  if (!state->font_zoom_pending) {
    return G_SOURCE_REMOVE;
  }

  const gdouble next_scale = state->pending_font_scale;
  state->font_zoom_pending = false;

  VteTerminal *terminal = VTE_TERMINAL(state->terminal);
  const gdouble current_scale = vte_terminal_get_font_scale(terminal);
  if (next_scale != current_scale) {
    start_font_resize_guard(state);
    vte_terminal_set_font_scale(terminal, next_scale);
    if (state->font_zoom_anchor_active &&
        font_scale_matches(next_scale, state->font_zoom_anchor_scale)) {
      state->desired_columns = state->font_zoom_anchor_columns;
      state->desired_rows = state->font_zoom_anchor_rows;
      state->font_zoom_anchor_active = false;
    }
    ensure_terminal_grid_size(state, state->desired_columns,
                              state->desired_rows);
    update_window_size(state);
  } else if (state->font_zoom_anchor_active &&
             font_scale_matches(next_scale, state->font_zoom_anchor_scale)) {
    state->font_zoom_anchor_active = false;
  }

  return G_SOURCE_REMOVE;
}

static void queue_font_scale_update(TerminalLayoutState *state, int steps) {
  VteTerminal *terminal = VTE_TERMINAL(state->terminal);
  const gdouble terminal_scale = vte_terminal_get_font_scale(terminal);
  const gdouble current_scale =
      state->font_zoom_pending ? state->pending_font_scale : terminal_scale;
  const gdouble next_scale =
      std::clamp(current_scale + font_scale_step * steps, minimum_font_scale,
                 maximum_font_scale);
  if (next_scale == current_scale) {
    return;
  }

  if (!state->font_zoom_anchor_active) {
    state->font_zoom_anchor_active = true;
    state->font_zoom_anchor_columns = state->desired_columns;
    state->font_zoom_anchor_rows = state->desired_rows;
    state->font_zoom_anchor_scale = terminal_scale;
  }

  state->pending_font_scale = next_scale;
  state->font_zoom_pending = true;
  if (state->font_zoom_source == 0) {
    state->font_zoom_source =
        g_timeout_add(font_zoom_apply_timeout_ms, apply_pending_font_scale,
                      state);
  }
}

static glong hinted_cell_count(int window_size, int base_size, int increment,
                               int minimum) {
  const int grid_size = window_size - base_size;
  if (grid_size <= 0) {
    return minimum;
  }

  const glong cells = grid_size / increment;
  return cells < minimum ? minimum : cells;
}

static void on_terminal_font_metrics_changed(VteTerminal *, GParamSpec *,
                                             gpointer data) {
  TerminalLayoutState *state = static_cast<TerminalLayoutState *>(data);
  if (!state->realized || window_state_is_snapped(state->window_state)) {
    return;
  }

  start_font_resize_guard(state);
  update_window_size(state);
}

static int consume_smooth_zoom_steps(TerminalLayoutState *state,
                                     gdouble delta_y) {
  state->smooth_zoom_delta_y += delta_y;

  int steps = 0;
  while (state->smooth_zoom_delta_y <= -1.0) {
    ++steps;
    state->smooth_zoom_delta_y += 1.0;
  }
  while (state->smooth_zoom_delta_y >= 1.0) {
    --steps;
    state->smooth_zoom_delta_y -= 1.0;
  }
  return steps;
}

static int scroll_zoom_steps(TerminalLayoutState *state,
                             const GdkEventScroll *event,
                             bool *handled_vertical_scroll) {
  *handled_vertical_scroll = false;
  switch (event->direction) {
  case GDK_SCROLL_UP:
    state->smooth_zoom_delta_y = 0.0;
    *handled_vertical_scroll = true;
    return 1;
  case GDK_SCROLL_DOWN:
    state->smooth_zoom_delta_y = 0.0;
    *handled_vertical_scroll = true;
    return -1;
  case GDK_SCROLL_SMOOTH:
    if (event->delta_y != 0.0) {
      *handled_vertical_scroll = true;
      return consume_smooth_zoom_steps(state, event->delta_y);
    }
    return 0;
  case GDK_SCROLL_LEFT:
  case GDK_SCROLL_RIGHT:
    return 0;
  }

  return 0;
}

static gboolean on_terminal_scroll_event(GtkWidget *, GdkEventScroll *event,
                                         gpointer data) {
  if ((event->state & GDK_CONTROL_MASK) == 0) {
    return GDK_EVENT_PROPAGATE;
  }

  TerminalLayoutState *state = static_cast<TerminalLayoutState *>(data);
  bool handled_vertical_scroll = false;
  const int steps =
      scroll_zoom_steps(state, event, &handled_vertical_scroll);
  if (steps != 0) {
    queue_font_scale_update(state, steps);
  }

  return handled_vertical_scroll ? GDK_EVENT_STOP : GDK_EVENT_PROPAGATE;
}

static gboolean on_terminal_key_press_event(GtkWidget *, GdkEventKey *event,
                                             gpointer data) {
  TerminalLayoutState *state = static_cast<TerminalLayoutState *>(data);
  const auto modifiers = static_cast<GdkModifierType>(event->state);
  if (state->key_bindings.zoom_in.has_value() &&
      key_binding_matches(*state->key_bindings.zoom_in, event->keyval,
                          modifiers)) {
    queue_font_scale_update(state, 1);
    return GDK_EVENT_STOP;
  }
  if (state->key_bindings.zoom_out.has_value() &&
      key_binding_matches(*state->key_bindings.zoom_out, event->keyval,
                          modifiers)) {
    queue_font_scale_update(state, -1);
    return GDK_EVENT_STOP;
  }
  return GDK_EVENT_PROPAGATE;
}

static void on_terminal_resize_window(VteTerminal *, guint columns, guint rows,
                                      gpointer data) {
  TerminalLayoutState *state = static_cast<TerminalLayoutState *>(data);
  if (state->realized && window_state_is_snapped(state->window_state)) {
    return;
  }

  if (font_grid_is_locked(state)) {
    ensure_terminal_grid_size(state, state->desired_columns,
                              state->desired_rows);
  } else {
    state->desired_columns = columns;
    state->desired_rows = rows;
    ensure_terminal_grid_size(state, columns, rows);
  }
  update_window_size(state);
}

static void sync_window_size(TerminalLayoutState *state) {
  if (!state->realized || window_state_is_snapped(state->window_state)) {
    return;
  }

  if (state->hints.width_inc <= 0 || state->hints.height_inc <= 0) {
    return;
  }

  GtkAllocation window_allocation;
  gtk_widget_get_allocation(state->window, &window_allocation);
  const glong columns = hinted_cell_count(window_allocation.width,
                                          state->hints.base_width,
                                          state->hints.width_inc,
                                          minimum_columns);
  const glong rows = hinted_cell_count(window_allocation.height,
                                       state->hints.base_height,
                                       state->hints.height_inc, minimum_rows);
  const int expected_width =
      state->hints.base_width +
      static_cast<int>(columns) * state->hints.width_inc;
  const int expected_height =
      state->hints.base_height +
      static_cast<int>(rows) * state->hints.height_inc;

  const bool window_size_is_exact =
      window_allocation.width == expected_width &&
      window_allocation.height == expected_height;
  VteTerminal *terminal = VTE_TERMINAL(state->terminal);
  if (font_grid_is_locked(state)) {
    // Keep font-driven allocations from becoming a terminal grid resize.
    ensure_terminal_grid_size(state, state->desired_columns,
                              state->desired_rows);
    resize_window_to_hinted_grid(state, state->desired_columns,
                                 state->desired_rows);
    if (state->options.fixture) {
      g_timeout_add(50, feed_fixture_idle, state);
    }
    return;
  }

  const bool grid_size_changed =
      columns != state->desired_columns || rows != state->desired_rows;
  if (state->font_zoom_anchor_active && grid_size_changed) {
    state->font_zoom_anchor_active = false;
  }
  if (!state->font_zoom_anchor_active) {
    state->desired_columns = columns;
    state->desired_rows = rows;
  }

  if (columns == vte_terminal_get_column_count(terminal) &&
      rows == vte_terminal_get_row_count(terminal) && window_size_is_exact) {
    update_fixture_grid_status(state);
    notify_terminal_grid_size_changed(state, columns, rows);
    notify_terminal_display_settings_changed(state, columns, rows);
    if (state->options.fixture) {
      g_timeout_add(50, feed_fixture_idle, state);
    }
    return;
  }

  if (!window_size_is_exact) {
    GdkWindow *gdk_window = gtk_widget_get_window(state->window);
    if (gdk_window != nullptr) {
      // Low-level X11 resizes can bypass GTK geometry hints.
      gdk_window_resize(gdk_window, expected_width, expected_height);
    }
  }

  if (columns != vte_terminal_get_column_count(terminal) ||
      rows != vte_terminal_get_row_count(terminal)) {
    ensure_terminal_grid_size(state, columns, rows);
  }
  queue_window_size_update(state);
}

static gboolean sync_window_size_timeout(gpointer data) {
  TerminalLayoutState *state = static_cast<TerminalLayoutState *>(data);
  state->window_size_sync_source = 0;
  sync_window_size(state);
  return G_SOURCE_REMOVE;
}

static bool window_size_matches_hinted_grid(TerminalLayoutState *state) {
  if (!state->realized || window_state_is_snapped(state->window_state)) {
    return false;
  }

  if (state->hints.width_inc <= 0 || state->hints.height_inc <= 0) {
    return false;
  }

  GtkAllocation window_allocation;
  gtk_widget_get_allocation(state->window, &window_allocation);
  const glong columns = hinted_cell_count(window_allocation.width,
                                          state->hints.base_width,
                                          state->hints.width_inc,
                                          minimum_columns);
  const glong rows = hinted_cell_count(window_allocation.height,
                                       state->hints.base_height,
                                       state->hints.height_inc, minimum_rows);
  return window_allocation.width == hinted_width_for_grid(state, columns) &&
         window_allocation.height == hinted_height_for_grid(state, rows);
}

static void queue_window_size_sync(TerminalLayoutState *state) {
  if (window_size_matches_hinted_grid(state)) {
    if (state->window_size_sync_source != 0) {
      g_source_remove(state->window_size_sync_source);
      state->window_size_sync_source = 0;
    }
    sync_window_size(state);
    return;
  }

  if (state->window_size_sync_source != 0) {
    g_source_remove(state->window_size_sync_source);
  }

  state->window_size_sync_source =
      g_timeout_add(window_size_sync_debounce_ms, sync_window_size_timeout,
                    state);
}

static gboolean on_window_configure_event(GtkWidget *, GdkEventConfigure *,
                                          gpointer data) {
  TerminalLayoutState *state = static_cast<TerminalLayoutState *>(data);
  if (!state->realized || window_state_is_snapped(state->window_state)) {
    return GDK_EVENT_PROPAGATE;
  }

  queue_window_size_sync(state);
  return GDK_EVENT_PROPAGATE;
}

static void on_window_size_allocate(GtkWidget *, GtkAllocation *,
                                    gpointer data) {
  TerminalLayoutState *state = static_cast<TerminalLayoutState *>(data);
  if (!state->realized || window_state_is_snapped(state->window_state)) {
    return;
  }

  queue_window_size_sync(state);
}

static void on_style_updated(GtkWidget *, gpointer data) {
  queue_window_size_update(static_cast<TerminalLayoutState *>(data));
}

static gboolean on_window_state_event(GtkWidget *, GdkEventWindowState *event,
                                      gpointer data) {
  TerminalLayoutState *state = static_cast<TerminalLayoutState *>(data);
  state->window_state = event->new_window_state;

  if (window_state_is_snapped(event->new_window_state)) {
    gtk_window_set_geometry_hints(GTK_WINDOW(state->window), nullptr, nullptr,
                                  static_cast<GdkWindowHints>(0));
  } else {
    queue_window_size_update(state);
  }

  return GDK_EVENT_PROPAGATE;
}

static void on_window_realized(GtkWidget *, gpointer data) {
  TerminalLayoutState *state = static_cast<TerminalLayoutState *>(data);
  state->realized = true;
  update_window_size(state);
}

static void feed_fixture(TerminalLayoutState *state) {
  VteTerminal *terminal = VTE_TERMINAL(state->terminal);
  const glong columns = vte_terminal_get_column_count(terminal);
  const glong rows = vte_terminal_get_row_count(terminal);
  if (columns <= 0 || rows <= 0) {
    return;
  }
  update_fixture_grid_status(state);

  std::string output =
      "\033[?25l\033[?7h\033[38;2;238;238;238m\033[48;2;0;0;0m\033[2J\033[H";
  constexpr char first_printable = '!';
  constexpr char last_printable = '~';
  constexpr int printable_count = last_printable - first_printable + 1;
  output.reserve(output.size() + static_cast<std::size_t>(columns * rows) + 3);
  for (glong index = 0; index < columns * rows; ++index) {
    output += static_cast<char>(first_printable + (index % printable_count));
  }
  output += "\033[H";

  vte_terminal_feed(terminal, output.data(), static_cast<gssize>(output.size()));
}

static gboolean feed_fixture_idle(gpointer data) {
  feed_fixture(static_cast<TerminalLayoutState *>(data));
  return G_SOURCE_REMOVE;
}

TerminalLayoutState *
create_terminal_layout(const MainWindow &main_window, TestOptions options,
                       TerminalDisplaySettings terminal_display_settings,
                       TerminalKeyBindings terminal_key_bindings,
                       TerminalLayoutCallbacks callbacks) {
  auto *state = new TerminalLayoutState();
  state->window = main_window.window;
  state->root_box = main_window.root_box;
  state->terminal_scroller = main_window.terminal_scroller;
  state->terminal = main_window.terminal;
  state->terminal_scrollbar = main_window.terminal_scrollbar;
  state->status_bar = main_window.status_bar;
  state->fixture_grid_size_label = main_window.fixture_grid_size_label;
  state->options = options;
  state->callbacks = callbacks;
  state->key_bindings = std::move(terminal_key_bindings);
  state->desired_columns = terminal_display_settings.width;
  state->desired_rows = terminal_display_settings.height;

  GtkAdjustment *adjustment =
      gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(state->terminal));
  gtk_range_set_adjustment(GTK_RANGE(state->terminal_scrollbar), adjustment);
  gtk_widget_add_events(state->terminal,
                        GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
                            GDK_KEY_PRESS_MASK);

  return state;
}

void connect_terminal_layout_signals(TerminalLayoutState *state) {
  if (state == nullptr) {
    return;
  }

  g_signal_connect(state->window, "realize", G_CALLBACK(on_window_realized),
                   state);
  g_signal_connect(state->window, "configure-event",
                   G_CALLBACK(on_window_configure_event), state);
  g_signal_connect(state->window, "size-allocate",
                   G_CALLBACK(on_window_size_allocate), state);
  g_signal_connect(state->window, "window-state-event",
                   G_CALLBACK(on_window_state_event), state);
  g_signal_connect(state->window, "style-updated", G_CALLBACK(on_style_updated),
                   state);
  g_signal_connect(state->root_box, "style-updated",
                   G_CALLBACK(on_style_updated), state);
  g_signal_connect(state->terminal, "style-updated",
                   G_CALLBACK(on_style_updated), state);
  g_signal_connect(state->terminal, "notify::font-desc",
                   G_CALLBACK(on_terminal_font_metrics_changed), state);
  g_signal_connect(state->terminal, "notify::font-scale",
                   G_CALLBACK(on_terminal_font_metrics_changed), state);
  g_signal_connect(state->terminal, "notify::cell-width-scale",
                   G_CALLBACK(on_terminal_font_metrics_changed), state);
  g_signal_connect(state->terminal, "notify::cell-height-scale",
                   G_CALLBACK(on_terminal_font_metrics_changed), state);
  g_signal_connect(state->terminal, "scroll-event",
                   G_CALLBACK(on_terminal_scroll_event), state);
  g_signal_connect(state->terminal, "key-press-event",
                   G_CALLBACK(on_terminal_key_press_event), state);
  g_signal_connect(state->terminal, "resize-window",
                   G_CALLBACK(on_terminal_resize_window), state);
}

void start_terminal_layout(TerminalLayoutState *state) {
  if (state == nullptr) {
    return;
  }

  queue_window_size_update(state);
  if (state->options.fixture) {
    g_idle_add(feed_fixture_idle, state);
  }
}

void apply_terminal_display_settings(
    TerminalLayoutState *state,
    TerminalDisplaySettings terminal_display_settings) {
  if (state == nullptr || terminal_display_settings.width <= 0 ||
      terminal_display_settings.height <= 0 ||
      terminal_display_settings.zoom <= 0.0) {
    return;
  }

  if (state->font_zoom_source != 0) {
    g_source_remove(state->font_zoom_source);
    state->font_zoom_source = 0;
  }
  state->font_zoom_pending = false;
  state->font_zoom_anchor_active = false;
  state->smooth_zoom_delta_y = 0.0;
  state->desired_columns = terminal_display_settings.width;
  state->desired_rows = terminal_display_settings.height;

  VteTerminal *terminal = VTE_TERMINAL(state->terminal);
  if (!font_scale_matches(vte_terminal_get_font_scale(terminal),
                          terminal_display_settings.zoom)) {
    start_font_resize_guard(state);
    vte_terminal_set_font_scale(terminal, terminal_display_settings.zoom);
  }

  ensure_terminal_grid_size(state, state->desired_columns,
                            state->desired_rows);
  update_window_size(state);
}

void apply_terminal_key_bindings(
    TerminalLayoutState *state,
    TerminalKeyBindings terminal_key_bindings) {
  if (state == nullptr) {
    return;
  }
  state->key_bindings = std::move(terminal_key_bindings);
}

void destroy_terminal_layout(TerminalLayoutState *state) {
  if (state == nullptr) {
    return;
  }

  if (state->font_zoom_source != 0) {
    g_source_remove(state->font_zoom_source);
  }
  if (state->window_size_sync_source != 0) {
    g_source_remove(state->window_size_sync_source);
  }
  if (state->font_resize_guard_source != 0) {
    g_source_remove(state->font_resize_guard_source);
  }
  delete state;
}

} // namespace elder_terms

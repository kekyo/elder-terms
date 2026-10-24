// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-presentation.h"
#include "terminal-runtime.h"
#include "terminal-background.h"
#include <elder-terms/terminal-color-filter.h>

#include <cmath>
#include <cstdio>
#include <deque>
#include <string_view>
#include <utility>

namespace elder_terms {

struct TerminalPresentation {
  MainWindow *window;
  TestOptions options;
  std::function<void(TerminalDisplaySettings)> changed;
  std::function<void()> send_break;
  TerminalKeyBindings bindings;
  std::optional<TerminalDisplaySettings> published;
  std::size_t fixture_rows = 0;
  std::size_t fixture_columns = 0;
  bool maximize_pending = false;
  std::deque<boxrig_font_zoom_direction> zoom_requests;
  guint zoom_source = 0;
  std::optional<TerminalDisplaySettings> pending_grid;
  TerminalCursorSettings cursor{TerminalCursorShape::block, true};
  TerminalBackground *background = nullptr;
  TerminalColorFilterContext colors{};
  bool block_copy_trim_trailing_spaces = true;
};

static bool apply_copy_policy(TerminalPresentation *presentation, bool trim) {
  auto *view = boxrig_gtk3_terminal_widget_view(terminal_runtime_widget(presentation->window->runtime));
  const auto status = boxrig_ui_view_set_rectangular_copy_policy(view, trim ?
      BOXRIG_UI_RECTANGULAR_COPY_TRIM_TRAILING_SPACES : BOXRIG_UI_RECTANGULAR_COPY_PRESERVE_SPACES);
  if (status != BOXRIG_UI_OK) {
    g_printerr("Terminal block copy: %d\n", status);
    return false;
  }
  presentation->block_copy_trim_trailing_spaces = trim;
  return true;
}

static bool apply_colors(TerminalPresentation *presentation, TerminalColorFilterContext context) {
  auto *view = boxrig_gtk3_terminal_widget_view(terminal_runtime_widget(presentation->window->runtime));
  context.reverse_palette.reset();
  if (!prepare_terminal_color_filter(&context, boxrig_ui_model_terminal(boxrig_ui_view_model(view)))) return false;
  const auto previous = presentation->colors;
  presentation->colors = context;
  const bool enabled = context.background.has_value() || context.filter != TerminalColorFilter::none ||
      (context.color_vision != TerminalColorVision::none && context.color_vision_strength > 0);
  const auto status = boxrig_ui_view_set_color_filter(view, enabled ? filter_terminal_colors : nullptr,
      enabled ? &presentation->colors : nullptr);
  if (status != BOXRIG_UI_OK) { presentation->colors = previous; return false; }
  // Registration invalidates colors and requests a frame, including when the
  // pointers are unchanged. Do not invalidate a second time for the same edit.
  return true;
}

static void prepare_colors(TerminalPresentation *presentation) {
  if (presentation->colors.filter != TerminalColorFilter::reverse_palette) return;
  auto *view = boxrig_gtk3_terminal_widget_view(terminal_runtime_widget(presentation->window->runtime));
  const auto previous = presentation->colors.reverse_palette;
  if (!prepare_terminal_color_filter(&presentation->colors, boxrig_ui_model_terminal(boxrig_ui_view_model(view)))) {
    g_printerr("Failed to prepare terminal color palette\n");
    return;
  }
  if (previous != presentation->colors.reverse_palette && boxrig_ui_view_invalidate_colors(view) != BOXRIG_UI_OK)
    g_printerr("Failed to invalidate terminal color palette\n");
}

static bool apply_cursor(TerminalPresentation *presentation, const TerminalCursorSettings &settings) {
  auto *view = boxrig_gtk3_terminal_widget_view(terminal_runtime_widget(presentation->window->runtime));
  auto style = boxrig_ui_cursor_style_default();
  style.shape = settings.shape == TerminalCursorShape::bar ? BOXRIG_UI_CURSOR_SHAPE_BAR :
      settings.shape == TerminalCursorShape::underline ? BOXRIG_UI_CURSOR_SHAPE_UNDERLINE : BOXRIG_UI_CURSOR_SHAPE_BLOCK;
  style.presentation = BOXRIG_UI_CURSOR_PRESENTATION_FOCUSED;
  style.box_width_dip = 4;
  style.line_width_dip = 1;
  style.blink = settings.blink ? BOXRIG_UI_CURSOR_BLINK_ENABLED : BOXRIG_UI_CURSOR_BLINK_STEADY;
  const auto status = boxrig_ui_view_set_cursor_style(view, &style);
  if (status != BOXRIG_UI_OK) {
    g_printerr("Terminal cursor: %d\n", status);
    return false;
  }
  presentation->cursor = settings;
  return true;
}

static gboolean dispatch_zoom(gpointer context) {
  auto *presentation = static_cast<TerminalPresentation *>(context);
  presentation->zoom_source = 0;
  while (!presentation->zoom_requests.empty()) {
    const auto result = terminal_runtime_visual_zoom(presentation->window->runtime, presentation->zoom_requests.front());
    if (result == BOXRIG_UI_BUSY) return G_SOURCE_REMOVE;
    presentation->zoom_requests.pop_front();
    if (result != BOXRIG_UI_OK) g_printerr("Terminal zoom: %d\n", result);
  }
  return G_SOURCE_REMOVE;
}

static void schedule_zoom(TerminalPresentation *presentation) {
  if (!presentation->zoom_requests.empty() && !presentation->zoom_source)
    presentation->zoom_source = g_idle_add_full(G_PRIORITY_DEFAULT, dispatch_zoom, presentation, nullptr);
}

static guint key_value(const boxrig_key_event &event) {
  if (event.key >= BOXRIG_KEY_F1 && event.key <= BOXRIG_KEY_F35)
    return GDK_KEY_F1 + event.key - BOXRIG_KEY_F1;
  if (event.key >= BOXRIG_KEY_KP_0 && event.key <= BOXRIG_KEY_KP_9)
    return GDK_KEY_KP_0 + event.key - BOXRIG_KEY_KP_0;
  switch (event.key) {
    case BOXRIG_KEY_ENTER: return GDK_KEY_Return;
    case BOXRIG_KEY_BACKSPACE: return GDK_KEY_BackSpace;
    case BOXRIG_KEY_TAB: return GDK_KEY_Tab;
    case BOXRIG_KEY_ESCAPE: return GDK_KEY_Escape;
    case BOXRIG_KEY_UP: return GDK_KEY_Up;
    case BOXRIG_KEY_DOWN: return GDK_KEY_Down;
    case BOXRIG_KEY_RIGHT: return GDK_KEY_Right;
    case BOXRIG_KEY_LEFT: return GDK_KEY_Left;
    case BOXRIG_KEY_HOME: return GDK_KEY_Home;
    case BOXRIG_KEY_END: return GDK_KEY_End;
    case BOXRIG_KEY_INSERT: return GDK_KEY_Insert;
    case BOXRIG_KEY_DELETE: return GDK_KEY_Delete;
    case BOXRIG_KEY_PAGE_UP: return GDK_KEY_Page_Up;
    case BOXRIG_KEY_PAGE_DOWN: return GDK_KEY_Page_Down;
    case BOXRIG_KEY_PAUSE: return GDK_KEY_Pause;
    case BOXRIG_KEY_PRINT_SCREEN: return GDK_KEY_Print;
    case BOXRIG_KEY_MENU: return GDK_KEY_Menu;
    case BOXRIG_KEY_KP_ADD: return GDK_KEY_KP_Add;
    case BOXRIG_KEY_KP_SUBTRACT: return GDK_KEY_KP_Subtract;
    case BOXRIG_KEY_KP_MULTIPLY: return GDK_KEY_KP_Multiply;
    case BOXRIG_KEY_KP_DIVIDE: return GDK_KEY_KP_Divide;
    case BOXRIG_KEY_KP_DECIMAL: return GDK_KEY_KP_Decimal;
    case BOXRIG_KEY_KP_ENTER: return GDK_KEY_KP_Enter;
    case BOXRIG_KEY_KP_EQUAL: return GDK_KEY_KP_Equal;
    default: break;
  }
  const auto codepoint = (event.modifiers & BOXRIG_MODIFIER_SHIFT) && event.shifted_codepoint ?
      event.shifted_codepoint : event.codepoint;
  return codepoint ? gdk_unicode_to_keyval(codepoint) : GDK_KEY_VoidSymbol;
}

static boxrig_input_key_route route_key(const boxrig_key_event *event, void *context) {
  auto *presentation = static_cast<TerminalPresentation *>(context);
  const auto selection = terminal_runtime_route_selection_key(presentation->window->runtime, *event);
  if (selection != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return selection;
  const auto clipboard = route_main_window_terminal_key(presentation->window, *event);
  if (clipboard != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return clipboard;
  const guint modifiers = ((event->modifiers & BOXRIG_MODIFIER_CONTROL) ? GDK_CONTROL_MASK : 0) |
      ((event->modifiers & BOXRIG_MODIFIER_SHIFT) ? GDK_SHIFT_MASK : 0) |
      ((event->modifiers & BOXRIG_MODIFIER_ALT) ? GDK_MOD1_MASK : 0) |
      ((event->modifiers & BOXRIG_MODIFIER_SUPER) ? GDK_SUPER_MASK : 0) |
      ((event->modifiers & BOXRIG_MODIFIER_META) ? GDK_META_MASK : 0) |
      ((event->modifiers & BOXRIG_MODIFIER_HYPER) ? GDK_HYPER_MASK : 0);
  const auto matches = [&](const std::optional<KeyBinding> &binding) {
    return binding && key_binding_matches(*binding, key_value(*event), static_cast<GdkModifierType>(modifiers));
  };
  if (matches(presentation->bindings.send_break)) {
    if (event->action == BOXRIG_KEY_PRESS && presentation->send_break) presentation->send_break();
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
  }
  const bool increase = matches(presentation->bindings.zoom_in);
  if (!increase && !matches(presentation->bindings.zoom_out)) return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
  if (event->action != BOXRIG_KEY_RELEASE) {
    // Native key delivery occurs inside frame(), where font mutation is BUSY.
    // Preserve event order and hand the steps to boxrig after dispatch. Its
    // visual sequence owns metric selection and pending resize generations.
    if (presentation->zoom_requests.size() == BOXRIG_GTK3_NATIVE_INPUT_EVENTS_MAXIMUM)
      return BOXRIG_INPUT_KEY_ROUTE_ERROR;
    presentation->zoom_requests.push_back(increase ? BOXRIG_FONT_ZOOM_INCREASE : BOXRIG_FONT_ZOOM_DECREASE);
    schedule_zoom(presentation);
  }
  return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}

TerminalPresentation *create_terminal_presentation(MainWindow *window, TestOptions options,
    std::function<void(TerminalDisplaySettings)> changed, std::function<void()> send_break) {
  auto *presentation = new TerminalPresentation{window, options, std::move(changed), std::move(send_break), {}, {}, 0, 0,
      options.maximize_window, {}, 0, {}};
  presentation->background = create_terminal_background(window);
  g_signal_connect_after(window->root_box, "size-allocate", G_CALLBACK(+[](GtkWidget *, GtkAllocation *, gpointer context) {
    auto *current = static_cast<TerminalPresentation *>(context);
    if (!current->pending_grid) return;
    const auto grid = *current->pending_grid;
    current->pending_grid.reset();
    // Chrome has its actual allocation now. Ask boxrig for the preferred grid
    // before the host dispatches any resulting content/grid layout actions.
    const auto status = terminal_runtime_set_preferred_grid(current->window->runtime, grid.height, grid.width);
    if (status != BOXRIG_UI_OK) g_printerr("Terminal grid: %d\n", status);
  }), presentation);
  return presentation;
}

void attach_terminal_presentation(TerminalPresentation *presentation) {
  terminal_runtime_set_prepare_frame_callback(presentation->window->runtime, [presentation] { prepare_colors(presentation); });
  const auto status = boxrig_gtk3_terminal_widget_set_key_router(
      terminal_runtime_widget(presentation->window->runtime), route_key, presentation);
  if (status != BOXRIG_UI_OK) g_printerr("Terminal shortcuts: %d\n", status);
  apply_cursor(presentation, presentation->cursor);
  const auto selection_status = apply_terminal_selection_style(
      boxrig_gtk3_terminal_widget_view(terminal_runtime_widget(presentation->window->runtime)));
  if (selection_status != BOXRIG_UI_OK) g_printerr("Terminal selection: %d\n", selection_status);
  apply_copy_policy(presentation, presentation->block_copy_trim_trailing_spaces);
  if (!apply_colors(presentation, presentation->colors)) g_printerr("Failed to restore terminal color correction\n");
  attach_terminal_background(presentation->background);
}

boxrig_input_mouse_route route_terminal_presentation_mouse(
    TerminalPresentation *presentation, const boxrig_mouse_event &event) {
  if (event.action != BOXRIG_MOUSE_PRESS || event.modifiers != BOXRIG_MODIFIER_CONTROL ||
      (event.button != BOXRIG_MOUSE_BUTTON_WHEEL_UP && event.button != BOXRIG_MOUSE_BUTTON_WHEEL_DOWN))
    return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
  if (presentation->zoom_requests.size() == BOXRIG_GTK3_NATIVE_INPUT_EVENTS_MAXIMUM)
    return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
  presentation->zoom_requests.push_back(event.button == BOXRIG_MOUSE_BUTTON_WHEEL_UP ?
      BOXRIG_FONT_ZOOM_INCREASE : BOXRIG_FONT_ZOOM_DECREASE);
  schedule_zoom(presentation);
  return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
}

TerminalBackgroundImagePtr terminal_presentation_background_image(const TerminalPresentation *presentation) {
  return terminal_background_image(presentation->background);
}

void prepare_terminal_presentation_background(TerminalPresentation *presentation, const SettingsStore &settings,
    std::function<bool()> current, std::function<void(TerminalBackgroundImagePtr, std::string)> complete) {
  prepare_terminal_background_image(presentation->background, settings, std::move(current), std::move(complete));
}

void cancel_terminal_presentation_background(TerminalPresentation *presentation) {
  if (presentation) cancel_terminal_background_image(presentation->background);
}

bool apply_terminal_presentation(TerminalPresentation *presentation, const SettingsStore &settings, TerminalBackgroundImagePtr image) {
  auto *window = presentation->window;
  auto *widget = terminal_runtime_widget(window->runtime);
  const auto display = terminal_display_settings(settings);
  const auto vision = terminal_color_vision_settings(settings);
  if (!apply_colors(presentation, {general_color_settings(settings).background,
      terminal_color_filter(settings), vision.profile, vision.strength / 100.0})) return false;
  if (!apply_cursor(presentation, terminal_cursor_settings(settings))) return false;
  if (!apply_copy_policy(presentation, terminal_block_copy_trim_trailing_spaces(settings))) return false;
  if (!apply_terminal_background(presentation->background, settings, std::move(image))) return false;
  const auto accessibility_status = boxrig_gtk3_terminal_widget_set_accessibility_enabled(
      widget, terminal_accessibility_enabled(settings));
  if (accessibility_status != BOXRIG_UI_OK) {
    g_printerr("Terminal accessibility: %d\n", accessibility_status);
    return false;
  }
  const auto families = terminal_font_families(settings);
  std::vector<boxrig_font_family> native_families;
  for (const auto &family : families.families)
    native_families.push_back({family.data(), family.size(), BOXRIG_FONT_RESOURCE_NONE});
  auto font = boxrig_font_specification_default();
  if (!native_families.empty()) {
    font.families = native_families.data();
    font.family_count = native_families.size();
  }
  font.size_dip = static_cast<float>(display.font_size_dip);
  boxrig_font_specification current_font{};
  auto font_status = boxrig_gtk3_terminal_widget_get_font_specification(widget, &current_font);
  if (font_status != BOXRIG_UI_OK) { g_printerr("Terminal font: %d\n", font_status); return false; }
  bool same_font = current_font.family_count == font.family_count && current_font.size_dip == font.size_dip &&
      current_font.line_height_adjustment_dip == font.line_height_adjustment_dip && current_font.weight == font.weight &&
      current_font.style == font.style && current_font.stretch == font.stretch && current_font.fallback == font.fallback &&
      current_font.monospace == font.monospace && current_font.feature_count == 0 && current_font.variation_count == 0;
  for (std::size_t index = 0; same_font && index != font.family_count; ++index) {
    const auto &current = current_font.families[index];
    const auto &requested = font.families[index];
    same_font = current.resource_id == requested.resource_id &&
        std::string_view(current.name, current.name_length) == std::string_view(requested.name, requested.name_length);
  }
  // Reapplying an unchanged font invalidates text layout and the selection.
  // Copy policy edits must leave an existing rectangle available for copying.
  if (!same_font) {
    font_status = boxrig_gtk3_terminal_widget_set_font_specification(widget, &font);
    if (font_status != BOXRIG_UI_OK) { g_printerr("Terminal font: %d\n", font_status); return false; }
  }
  auto *core = boxrig_ui_model_terminal(boxrig_ui_view_model(boxrig_gtk3_terminal_widget_view(widget)));
  if (boxrig_terminal_set_scrollback_capacity(core, display.scrollback_lines) != BOXRIG_STATUS_OK) return false;
  presentation->bindings = terminal_key_bindings(settings);
  const bool border = general_show_border(settings);
  const auto width = general_border_width(settings);
  const bool compact = general_compact_mode(settings);
  gint border_width = 0, border_height = 0;
  gtk_widget_get_size_request(window->frame_start_border, &border_width, &border_height);
  const bool chrome_changed = window->compact_mode != compact ||
      (gtk_widget_get_visible(window->frame_start_border) != FALSE) != border || (border && border_width != width);
  for (auto *side : {window->frame_start_border, window->frame_end_border}) {
    gtk_widget_set_size_request(side, width, -1);
    gtk_widget_set_no_show_all(side, !border);
    gtk_widget_set_visible(side, border);
  }
  gtk_widget_set_size_request(window->frame_bottom_border, -1, width);
  gtk_widget_set_no_show_all(window->frame_bottom_border, !(border && compact));
  gtk_widget_set_visible(window->frame_bottom_border, border && compact);
  set_main_window_compact_mode(window, compact);
  if (chrome_changed && gtk_widget_get_mapped(window->root_box)) {
    presentation->pending_grid = display;
    return true;
  }
  return terminal_runtime_set_preferred_grid(window->runtime, display.height, display.width) == BOXRIG_UI_OK;
}

void refresh_terminal_presentation(TerminalPresentation *presentation) {
  schedule_zoom(presentation);
  auto *window = presentation->window;
  auto *widget = terminal_runtime_widget(window->runtime);
  auto *core = boxrig_ui_model_terminal(boxrig_ui_view_model(boxrig_gtk3_terminal_widget_view(widget)));
  std::size_t rows = 0, columns = 0;
  boxrig_terminal_size(core, &rows, &columns);
  boxrig_font_effective_metrics metrics{};
  boxrig_gtk_render_state render{};
  if (boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) != BOXRIG_FONT_CATALOG_OK ||
      boxrig_gtk3_terminal_widget_get_render_state(widget, &render) != BOXRIG_UI_OK) return;
  const auto history = boxrig_terminal_scrollback_capacity(core);
  const bool settled = terminal_runtime_is_settled(window->runtime) &&
      presentation->zoom_requests.empty() && presentation->zoom_source == 0 && !presentation->pending_grid;
  {
    boxrig_scrollbar_state scroll{};
    boxrig_ui_selection selection{};
    auto *view = boxrig_gtk3_terminal_widget_view(widget);
    if (boxrig_ui_view_get_scrollbar_state(view, &scroll) != BOXRIG_UI_OK) return;
    const bool selected = boxrig_ui_view_get_selection(view, &selection);
    char report[512];
    std::snprintf(report, sizeof(report), "backend=%s grid=%zux%zu font=%.2f history=%zu cell=%ux%u scale=%d ready=%d a11y=%s scroll=%zu/%zu selected=%d selection_rows=%zu",
        render.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL ? "opengl" :
            render.active_backend == BOXRIG_GTK_RENDER_BACKEND_CAIRO ? "cairo" : "initializing",
        columns, rows, metrics.requested_size_dip, history, metrics.cell_width, metrics.cell_height,
        gtk_widget_get_scale_factor(GTK_WIDGET(widget)), settled,
        boxrig_gtk3_terminal_widget_get_accessibility_enabled(widget) ? "on" : "off",
        scroll.model.position, scroll.model.history_rows, selected, selection.row_count);
    gtk_label_set_text(GTK_LABEL(window->fixture_terminal_state_label), report);
    const auto grid = std::to_string(columns) + "x" + std::to_string(rows);
    gtk_label_set_text(GTK_LABEL(window->fixture_grid_size_label), grid.c_str());
    gtk_label_set_text(GTK_LABEL(window->fixture_scrollback_lines_label), std::to_string(history).c_str());
    if (presentation->options.fixture && settled &&
        (rows != presentation->fixture_rows || columns != presentation->fixture_columns)) {
      presentation->fixture_rows = rows;
      presentation->fixture_columns = columns;
      std::string text = "\033[?25l\033[?7h\033[38;2;238;238;238m\033[48;2;0;0;0m\033[2J\033[H";
      for (std::size_t index = 0; index < rows * columns; ++index) text += static_cast<char>('!' + index % ('~' - '!' + 1));
      text += "\033[H";
      terminal_runtime_feed(window->runtime, text);
    }
  }
  if (!settled || presentation->pending_grid) return;
  if (presentation->maximize_pending) {
    presentation->maximize_pending = false;
    // Xvfb has no window manager. Emit the state and the actual allocation
    // independently, as a window manager would, without forcing a core grid.
    GdkEventWindowState event{};
    event.new_window_state = GDK_WINDOW_STATE_MAXIMIZED;
    gboolean handled = FALSE;
    g_signal_emit_by_name(window->window, "window-state-event", &event, &handled);
    gint width = 0, height = 0;
    gtk_window_get_size(GTK_WINDOW(window->window), &width, &height);
    gtk_window_resize(GTK_WINDOW(window->window), width + 2 * metrics.cell_width, height + 2 * metrics.cell_height);
  }
  std::size_t preferred_rows = 0, preferred_columns = 0;
  if (boxrig_gtk3_terminal_widget_get_preferred_grid(widget, &preferred_rows, &preferred_columns) != BOXRIG_UI_OK) return;
  TerminalDisplaySettings current{static_cast<glong>(preferred_columns), static_cast<glong>(preferred_rows),
      static_cast<glong>(history), metrics.requested_size_dip};
  const auto &before = presentation->published;
  if (!before || before->width != current.width || before->height != current.height ||
      before->scrollback_lines != current.scrollback_lines || before->font_size_dip != current.font_size_dip) {
    presentation->published = current;
    if (presentation->changed) presentation->changed(current);
  }
}

void destroy_terminal_presentation(TerminalPresentation *presentation) {
  if (presentation && presentation->window->runtime)
    terminal_runtime_set_prepare_frame_callback(presentation->window->runtime, {});
  if (presentation) {
    auto *widget = terminal_runtime_widget(presentation->window->runtime);
    if (widget) boxrig_ui_view_set_color_filter(boxrig_gtk3_terminal_widget_view(widget), nullptr, nullptr);
  }
  if (presentation) destroy_terminal_background(presentation->background);
  if (presentation && presentation->zoom_source) g_source_remove(presentation->zoom_source);
  if (presentation) g_signal_handlers_disconnect_by_data(presentation->window->root_box, presentation);
  delete presentation;
}

std::string terminal_presentation_background_status(const TerminalPresentation *presentation) {
  return terminal_background_status(presentation ? presentation->background : nullptr);
}

}

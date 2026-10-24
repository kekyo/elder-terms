// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-presentation.h"
#include "../../src/terminal-runtime.h"

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

using namespace elder_terms;

static bool reject_frame = false;
static bool rejected_frame = false;
static std::string selection_limit;
static bool reject_selection_update = false;
static std::string selection_warnings;

extern "C" boxrig_ui_status __real_boxrig_gtk3_terminal_widget_create(
    boxrig_ui_model *, const boxrig_gtk3_terminal_widget_config *, BoxrigGtk3TerminalWidget **);
extern "C" boxrig_ui_status __wrap_boxrig_gtk3_terminal_widget_create(
    boxrig_ui_model *model, const boxrig_gtk3_terminal_widget_config *config, BoxrigGtk3TerminalWidget **widget) {
  auto bounded = *config;
  if (selection_limit == "rows") bounded.view.maximum_selection_rows = 2;
  if (selection_limit == "ranges") bounded.view.maximum_selection_ranges = 2;
  if (selection_limit == "bytes") bounded.view.maximum_copy_bytes = 8;
  return __real_boxrig_gtk3_terminal_widget_create(model, &bounded, widget);
}

extern "C" boxrig_ui_status __real_boxrig_ui_view_update_selection(boxrig_ui_view *, const boxrig_ui_hit_test_result *);
extern "C" boxrig_ui_status __wrap_boxrig_ui_view_update_selection(boxrig_ui_view *view, const boxrig_ui_hit_test_result *hit) {
  if (reject_selection_update) {
    reject_selection_update = false;
    return BOXRIG_UI_OUT_OF_MEMORY;
  }
  return __real_boxrig_ui_view_update_selection(view, hit);
}

extern "C" boxrig_ui_status __real_boxrig_gtk3_terminal_widget_frame(
    BoxrigGtk3TerminalWidget *, boxrig_ui_component_frame_result *);
extern "C" boxrig_ui_status __wrap_boxrig_gtk3_terminal_widget_frame(
    BoxrigGtk3TerminalWidget *widget, boxrig_ui_component_frame_result *result) {
  if (reject_frame) {
    reject_frame = false;
    rejected_frame = true;
    return BOXRIG_UI_OUT_OF_MEMORY;
  }
  return __real_boxrig_gtk3_terminal_widget_frame(widget, result);
}

struct Fixture {
  MainWindow *window;
  TerminalPresentation *presentation;
  cardio::primitives::conditional changed;
  cardio::primitives::conditional received;
  std::string outgoing;
  bool accept_commit = false;
  std::string committed;
};

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static boxrig_ui_view *view(Fixture *fixture) {
  return boxrig_gtk3_terminal_widget_view(terminal_runtime_widget(fixture->window->runtime));
}

static void key(Fixture *fixture, guint symbol, GdkEventType type, guint modifiers) {
  auto *widget = GTK_WIDGET(terminal_runtime_widget(fixture->window->runtime));
  GdkEventKey event{};
  event.type = type;
  event.window = gtk_widget_get_window(widget);
  event.send_event = TRUE;
  event.time = GDK_CURRENT_TIME;
  event.keyval = symbol;
  event.hardware_keycode = symbol == GDK_KEY_Escape ? 9 : symbol == GDK_KEY_Return ? 36 : symbol == GDK_KEY_c ? 54 : 38;
  event.state = modifiers;
  gboolean handled = FALSE;
  g_signal_emit_by_name(widget, type == GDK_KEY_PRESS ? "key-press-event" : "key-release-event", &event, &handled);
  expect(handled, "native terminal accepts the key event");
}

static void mouse(Fixture *fixture, GdkEventType type, double column, double row, guint modifiers, guint button = GDK_BUTTON_PRIMARY) {
  auto *widget = terminal_runtime_widget(fixture->window->runtime);
  boxrig_font_effective_metrics metrics{};
  expect(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) == BOXRIG_FONT_CATALOG_OK, "mouse metrics");
  const auto scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
  const auto x = column * metrics.cell_width / scale;
  const auto y = row * metrics.cell_height / scale;
  gboolean handled = FALSE;
  if (type == GDK_MOTION_NOTIFY) {
    GdkEventMotion event{};
    event.type = type;
    event.window = gtk_widget_get_window(GTK_WIDGET(widget));
    event.send_event = TRUE;
    event.x = x;
    event.y = y;
    event.state = modifiers;
    g_signal_emit_by_name(widget, "motion-notify-event", &event, &handled);
  } else {
    GdkEventButton event{};
    event.type = type;
    event.window = gtk_widget_get_window(GTK_WIDGET(widget));
    event.send_event = TRUE;
    event.x = x;
    event.y = y;
    event.button = button;
    event.state = modifiers;
    g_signal_emit_by_name(widget, type == GDK_BUTTON_RELEASE ? "button-release-event" : "button-press-event", &event, &handled);
  }
  expect(handled, "native terminal accepts the pointer event");
}

static bool selected(Fixture *fixture) {
  boxrig_ui_selection selection{};
  return boxrig_ui_view_get_selection(view(fixture), &selection);
}

static void wheel(Fixture *fixture, GdkScrollDirection direction, double column, double row, guint modifiers) {
  auto *widget = terminal_runtime_widget(fixture->window->runtime);
  boxrig_font_effective_metrics metrics{};
  expect(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) == BOXRIG_FONT_CATALOG_OK, "wheel metrics");
  const auto scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
  GdkEventScroll event{};
  event.type = GDK_SCROLL;
  event.window = gtk_widget_get_window(GTK_WIDGET(widget));
  event.send_event = TRUE;
  event.x = column * metrics.cell_width / scale;
  event.y = row * metrics.cell_height / scale;
  event.state = modifiers;
  event.direction = direction;
  gboolean handled = FALSE;
  g_signal_emit_by_name(widget, "scroll-event", &event, &handled);
  expect(handled, "native terminal accepts the wheel event");
}

static std::string selected_text(Fixture *fixture) {
  boxrig_text_snapshot *snapshot = nullptr;
  expect(boxrig_ui_view_create_selection_snapshot(view(fixture), &snapshot) == BOXRIG_UI_OK, "snapshot selected text");
  std::size_t size = 0;
  const auto *bytes = boxrig_text_snapshot_data(snapshot, &size);
  const std::string text(reinterpret_cast<const char *>(bytes), size);
  boxrig_text_snapshot_destroy(snapshot);
  return text;
}

static void select(Fixture *fixture, boxrig_ui_selection_shape shape, bool empty) {
  auto *widget = terminal_runtime_widget(fixture->window->runtime);
  boxrig_font_effective_metrics metrics{};
  expect(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) == BOXRIG_FONT_CATALOG_OK, "selection metrics");
  boxrig_ui_hit_test_result hit{};
  expect(boxrig_ui_view_hit_test(view(fixture), BOXRIG_UI_FIXED_ONE,
      metrics.cell_height / 2 * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK, "selection start");
  boxrig_ui_selection_options options{};
  options.kind = BOXRIG_UI_SELECTION_CHARACTER;
  options.shape = shape;
  expect(boxrig_ui_view_begin_selection_with_options(view(fixture), &options, &hit) == BOXRIG_UI_OK, "begin retained selection");
  if (!empty) expect(boxrig_ui_view_hit_test(view(fixture), metrics.cell_width * 8 * BOXRIG_UI_FIXED_ONE,
      metrics.cell_height / 2 * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK, "selection end");
  expect(boxrig_ui_view_update_selection(view(fixture), &hit) == BOXRIG_UI_OK && selected(fixture), "retain selection anchor");
  if (empty) {
    boxrig_ui_selection selection{};
    expect(boxrig_ui_view_get_selection(view(fixture), &selection) && selection.row_count == 0,
        "zero-width rectangle retains its empty anchor");
  }
}

static cardio::promise<std::string> clipboard(GdkAtom selection) {
  struct Request {
    cardio::primitives::conditional ready;
    bool complete = false;
    std::string text;
  } request;
  gtk_clipboard_request_text(gtk_clipboard_get(selection), +[](GtkClipboard *, const char *text, gpointer context) {
    auto *request = static_cast<Request *>(context);
    request->text = text ? text : "";
    request->complete = true;
    request->ready.trigger();
  }, &request);
  while (!request.complete) co_await request.ready.wait();
  co_return request.text;
}

static cardio::promise<void> barrier(Fixture *fixture, bool kitty) {
  // A non-text key avoids the IME's committed-text path in Kitty mode.
  key(fixture, GDK_KEY_Return, GDK_KEY_PRESS, 0);
  key(fixture, GDK_KEY_Return, GDK_KEY_RELEASE, 0);
  const std::string tail = kitty ? "\033[13;1:3u" : "\r";
  while (!fixture->outgoing.ends_with(tail) || !terminal_runtime_is_settled(fixture->window->runtime)) {
    boxrig_ui_component_diagnostic diagnostic{};
    expect(boxrig_gtk3_terminal_widget_get_diagnostic(terminal_runtime_widget(fixture->window->runtime), &diagnostic) == BOXRIG_UI_OK &&
        diagnostic.failure_reason == BOXRIG_UI_COMPONENT_FAILURE_NONE, "selection failure must not disable the terminal component");
    co_await fixture->changed.wait();
  }
}

static cardio::promise<void> click_scenarios(Fixture *fixture) {
  auto *runtime = fixture->window->runtime;
  expect(terminal_runtime_feed(runtime, "\033[=0u\033[2J\033[Halpha beta gamma\r\nsecond row"), "prepare click text");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), "click primary", -1);
  select(fixture, BOXRIG_UI_SELECTION_LINEAR, false);
  mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, 0);
  co_await barrier(fixture, false);
  expect(!selected(fixture), "single press clears the old selection without selecting one character");
  mouse(fixture, GDK_BUTTON_RELEASE, 0.1, 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(!selected(fixture) && co_await clipboard(GDK_SELECTION_PRIMARY) == "click primary",
      "single click preserves PRIMARY and has no selected character");

  auto *widget = GTK_WIDGET(terminal_runtime_widget(runtime));
  boxrig_font_effective_metrics metrics{};
  expect(boxrig_gtk3_terminal_widget_get_effective_metrics(terminal_runtime_widget(runtime), &metrics) == BOXRIG_FONT_CATALOG_OK,
      "drag threshold metrics");
  int threshold = 0;
  g_object_get(gtk_widget_get_settings(widget), "gtk-dnd-drag-threshold", &threshold, nullptr);
  const auto columns_per_dip = static_cast<double>(gtk_widget_get_scale_factor(widget)) / metrics.cell_width;
  mouse(fixture, GDK_BUTTON_PRESS, 1.25 * columns_per_dip, 0.5, 0);
  mouse(fixture, GDK_MOTION_NOTIFY, (threshold + 1.25) * columns_per_dip, 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(!selected(fixture), "motion at the GTK threshold remains a pending click at either display scale");
  mouse(fixture, GDK_MOTION_NOTIFY, (threshold + 2.25) * columns_per_dip, 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(selected(fixture), "motion beyond the GTK threshold starts a selection");
  mouse(fixture, GDK_BUTTON_RELEASE, 8.9, 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect((co_await clipboard(GDK_SELECTION_PRIMARY)).starts_with("alpha"), "drag starts at the original press");

  for (const auto type : {GDK_2BUTTON_PRESS, GDK_3BUTTON_PRESS}) {
    mouse(fixture, type, 1.1, 0.5, 0);
    co_await barrier(fixture, false);
    boxrig_ui_selection selection{};
    expect(boxrig_ui_view_get_selection(view(fixture), &selection) && selection.kind ==
        (type == GDK_2BUTTON_PRESS ? BOXRIG_UI_SELECTION_WORD : BOXRIG_UI_SELECTION_LINE),
        "multiple clicks immediately select a word or line");
    mouse(fixture, GDK_BUTTON_RELEASE, 1.1, 0.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    const auto copied = co_await clipboard(GDK_SELECTION_PRIMARY);
    expect(type == GDK_2BUTTON_PRESS ? copied == "alpha" : copied.starts_with("alpha beta gamma"),
        "word and line selection copy their text");
    mouse(fixture, type, 1.1, 0.5, GDK_CONTROL_MASK);
    mouse(fixture, GDK_MOTION_NOTIFY, 8.9, 1.5, GDK_BUTTON1_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, 8.9, 1.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    expect(boxrig_ui_view_get_selection(view(fixture), &selection) &&
        selection.kind == BOXRIG_UI_SELECTION_CHARACTER && selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR,
        "Ctrl multiple clicks use character rectangles even after Ctrl is released");
  }

  for (const auto modifier : {GDK_SHIFT_MASK, GDK_CONTROL_MASK}) {
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), "cancel pending", -1);
    mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, modifier);
    key(fixture, GDK_KEY_Escape, GDK_KEY_PRESS, 0);
    key(fixture, GDK_KEY_Escape, GDK_KEY_RELEASE, 0);
    mouse(fixture, GDK_MOTION_NOTIFY, 8.9, 0.5, GDK_BUTTON1_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, 8.9, 0.5, GDK_BUTTON1_MASK);
    fixture->outgoing.clear();
    co_await barrier(fixture, false);
    expect(!selected(fixture) && fixture->outgoing == "\r" &&
        co_await clipboard(GDK_SELECTION_PRIMARY) == "cancel pending", "Escape cancels a pending click without side effects");
  }

  // A retained press must never acquire a new generation after terminal output.
  mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, GDK_CONTROL_MASK);
  co_await barrier(fixture, false);
  expect(terminal_runtime_feed(runtime, "\033[Hchanged text"), "replace text under a pending click");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  mouse(fixture, GDK_MOTION_NOTIFY, 8.9, 0.5, GDK_BUTTON1_MASK);
  mouse(fixture, GDK_BUTTON_RELEASE, 8.9, 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(!selected(fixture) && co_await clipboard(GDK_SELECTION_PRIMARY) == "cancel pending",
      "a stale pending press aborts without selecting replacement text");

  std::vector<TerminalHyperlinkResolution> actions;
  terminal_runtime_set_hyperlink_rules(runtime, default_hyperlink_action_rules(),
      [&actions](TerminalHyperlinkResolution action) { actions.push_back(std::move(action)); });
  for (const bool osc8 : {false, true}) {
    const std::string text = osc8 ? "\033]8;;https://link.test/\033\\Open link\033]8;;\033\\" : "https://link.test/";
    expect(terminal_runtime_feed(runtime, "\033[2J\033[H" + text + "\r\nsecond row\033[?1002h\033[?1006h"),
        "prepare links with mouse reporting");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    for (const auto modifier : {guint(GDK_CONTROL_MASK), guint(GDK_CONTROL_MASK | GDK_SHIFT_MASK)}) {
      const auto previous = actions.size();
      mouse(fixture, GDK_BUTTON_PRESS, 2.1, 0.5, modifier);
      co_await barrier(fixture, false);
      expect(actions.size() == previous && !selected(fixture), "link activation waits for release");
      mouse(fixture, GDK_MOTION_NOTIFY, 8.9, 1.5, GDK_BUTTON1_MASK);
      mouse(fixture, GDK_BUTTON_RELEASE, 8.9, 1.5, GDK_BUTTON1_MASK);
      fixture->outgoing.clear();
      co_await barrier(fixture, false);
      boxrig_ui_selection selection{};
      expect(actions.size() == previous && boxrig_ui_view_get_selection(view(fixture), &selection) &&
          selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR && fixture->outgoing == "\r",
          "Ctrl drag over regex and OSC 8 links selects a rectangle without launching or terminal mouse reports");
    }
    const auto previous = actions.size();
    mouse(fixture, GDK_BUTTON_PRESS, 2.1, 0.5, GDK_CONTROL_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, 2.1, 0.5, GDK_BUTTON1_MASK | GDK_CONTROL_MASK);
    co_await barrier(fixture, false);
    expect(actions.size() == previous + 1 && actions.back().action &&
        actions.back().action->arguments == std::vector<std::string>{"https://link.test/"},
        "Ctrl click launches the same valid link once on release");
    mouse(fixture, GDK_BUTTON_PRESS, 2.1, 0.5, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, 2.1, 0.5, GDK_BUTTON1_MASK | GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    co_await barrier(fixture, false);
    expect(actions.size() == previous + 1, "Ctrl Shift click never launches a link");
    mouse(fixture, GDK_BUTTON_PRESS, 2.1, 0.5, GDK_CONTROL_MASK);
    co_await barrier(fixture, false);
    expect(terminal_runtime_feed(runtime, "\033[Hremoved link         "), "invalidate a pending link");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    mouse(fixture, GDK_BUTTON_RELEASE, 2.1, 0.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    expect(actions.size() == previous + 1, "an invalidated link does not launch on release");
    const std::string adjacent = osc8 ?
        "\033]8;id=first;https://a.test/\033\\left\033]8;;\033\\ "
        "\033]8;id=second;https://a.test/\033\\right\033]8;;\033\\" :
        "https://a.test/ https://a.test/";
    expect(terminal_runtime_feed(runtime, "\033[2J\033[H" + adjacent), "prepare distinct links with identical actions");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    const auto boundary = osc8 ? 4.0 : 15.0;
    mouse(fixture, GDK_BUTTON_PRESS, boundary - 0.1, 0.5, GDK_CONTROL_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, boundary + 1.1, 0.5, GDK_BUTTON1_MASK | GDK_CONTROL_MASK);
    co_await barrier(fixture, false);
    expect(actions.size() == previous + 1, "release on a different link with the same URL does not activate it");
  }
  terminal_runtime_set_hyperlink_rules(runtime, {}, {});
  fixture->outgoing.clear();
  mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, 0);
  mouse(fixture, GDK_MOTION_NOTIFY, 8.9, 0.5, GDK_BUTTON1_MASK);
  mouse(fixture, GDK_BUTTON_RELEASE, 8.9, 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(fixture->outgoing == "\033[<0;1;1M\033[<32;9;1M\033[<0;9;1m\r" && !selected(fixture),
      "unmodified mouse reports reach the peer as one complete sequence");
  expect(terminal_runtime_feed(runtime, "\033[?1002l\033[?1006l"), "restore ordinary mouse input");
}

static cardio::promise<void> scroll_scenarios(Fixture *fixture) {
  auto *runtime = fixture->window->runtime;
  for (const auto modifier : {GDK_SHIFT_MASK, GDK_CONTROL_MASK}) {
    expect(terminal_runtime_feed(runtime, "\033[3J\033[2J\033[Hfirst row\r\nsecond row"), "prepare edge text");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    for (const bool bottom : {false, true}) {
      mouse(fixture, GDK_BUTTON_PRESS, 3.1, 1.5, modifier);
      mouse(fixture, GDK_MOTION_NOTIFY, bottom ? 1000 : -3, bottom ? 1000 : -3, GDK_BUTTON1_MASK);
      mouse(fixture, GDK_BUTTON_RELEASE, bottom ? 1000 : -3, bottom ? 1000 : -3, GDK_BUTTON1_MASK);
      co_await barrier(fixture, false);
      boxrig_ui_layout_result layout{};
      expect(boxrig_ui_view_get_layout(view(fixture), &layout) == BOXRIG_UI_OK, "edge layout");
      boxrig_ui_hit_test_result edge{};
      expect(boxrig_ui_view_hit_test(view(fixture), bottom ? layout.grid_width - 1 : 0,
          bottom ? layout.grid_height - 1 : 0, &edge) == BOXRIG_UI_OK, "clipped edge position");
      boxrig_ui_selection selection{};
      expect(boxrig_ui_view_get_selection(view(fixture), &selection) &&
          selection.focus.line_id.serial == edge.position.line_id.serial && selection.focus.column == edge.position.column,
          "outside motion and release extend to the nearest corner of the grid");
    }

    std::string history = "\033[3J\033[2J\033[H";
    for (int row = 0; row != 100; ++row) history += std::to_string(1000 + row) + " alpha beta" + (row == 99 ? "" : "\r\n");
    expect(terminal_runtime_feed(runtime, history), "populate numbered scrollback");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    boxrig_ui_layout_result layout{};
    expect(boxrig_ui_view_get_layout(view(fixture), &layout) == BOXRIG_UI_OK, "history layout");
    boxrig_font_effective_metrics font{};
    expect(boxrig_gtk3_terminal_widget_get_effective_metrics(terminal_runtime_widget(runtime), &font) == BOXRIG_FONT_CATALOG_OK,
        "history font before selection");
    mouse(fixture, GDK_BUTTON_PRESS, 0.1, layout.rows - 1.5, modifier);
    mouse(fixture, GDK_MOTION_NOTIFY, 3.9, layout.rows - 0.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    boxrig_ui_selection anchor{};
    expect(boxrig_ui_view_get_selection(view(fixture), &anchor), "capture history anchor");
    boxrig_scrollbar_model initial{};
    auto *terminal = boxrig_ui_model_terminal(boxrig_ui_view_model(view(fixture)));
    expect(boxrig_scrollbar_model_read(terminal, &initial) == 0, "history position before wheel");
    const auto steps = layout.rows / 3 + 4;
    for (std::size_t step = 0; step != steps; ++step) {
      wheel(fixture, GDK_SCROLL_UP, 3.9, 0.5, modifier | GDK_BUTTON1_MASK);
      co_await barrier(fixture, false);
    }
    boxrig_scrollbar_model current{};
    expect(boxrig_scrollbar_model_read(terminal, &current) == 0 &&
        current.position == initial.position - steps * 3, "held selection wheel scrolls three history rows each time");
    boxrig_font_effective_metrics after{};
    expect(boxrig_gtk3_terminal_widget_get_effective_metrics(terminal_runtime_widget(runtime), &after) == BOXRIG_FONT_CATALOG_OK &&
        after.requested_size_dip == font.requested_size_dip, "Ctrl wheel while selecting never zooms");
    boxrig_ui_selection selection{};
    expect(boxrig_ui_view_get_selection(view(fixture), &selection) &&
        selection.anchor.line_id.serial == anchor.anchor.line_id.serial &&
        selection.anchor.column == (modifier == GDK_SHIFT_MASK ? 1 : anchor.anchor.column),
        "wheel selection retains its original logical anchor outside the viewport");
    const auto first = 100 - layout.rows - steps * 3;
    std::string expected;
    for (std::size_t row = first; row <= 98; ++row) {
      if (!expected.empty()) expected += '\n';
      auto line = std::to_string(1000 + row);
      if (modifier == GDK_SHIFT_MASK) {
        line += " alpha beta";
        if (row == first) line.erase(0, 3);
        if (row == 98) line.resize(1);
      }
      expected += line;
    }
    if (modifier == GDK_CONTROL_MASK) {
      expect(!selection.anchor_visual_row_valid && selection.row_count > layout.rows,
          "rectangle query marks the offscreen anchor invalid instead of claiming viewport row zero");
      expect(selected_text(fixture) == expected, "rectangle includes every row between the offscreen anchor and fresh wheel endpoint");
      mouse(fixture, GDK_MOTION_NOTIFY, 0.1, 0.5, GDK_BUTTON1_MASK);
      co_await barrier(fixture, false);
      expect(boxrig_ui_view_get_selection(view(fixture), &selection) && selection.row_count == 0,
          "scrolling rectangle can cross zero width without losing its anchor");
      mouse(fixture, GDK_MOTION_NOTIFY, 3.9, 0.5, GDK_BUTTON1_MASK);
      co_await barrier(fixture, false);
      expect(selected_text(fixture) == expected, "empty scrolling rectangle expands back from its original anchor");
    } else {
      expect(selected_text(fixture) == expected,
          "linear selection includes the endpoint and original anchor characters across the fresh viewport");
    }
    const auto copy = selected_text(fixture);
    mouse(fixture, GDK_BUTTON_RELEASE, 3.9, 0.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    expect(co_await clipboard(GDK_SELECTION_PRIMARY) == copy, "release copies the full selection spanning more than a viewport");
    mouse(fixture, GDK_BUTTON_PRESS, 0.1, 1.5, modifier);
    mouse(fixture, GDK_MOTION_NOTIFY, 3.9, 2.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    wheel(fixture, GDK_SCROLL_UP, 3.9, 0.5, modifier | GDK_BUTTON1_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, 3.9, 0.5, GDK_BUTTON1_MASK);
    mouse(fixture, GDK_MOTION_NOTIFY, 20.9, 8.5, 0);
    co_await barrier(fixture, false);
    expect(co_await clipboard(GDK_SELECTION_PRIMARY) == selected_text(fixture),
        "release queued with a wheel copies after layout and ignores later hover motion");
    mouse(fixture, GDK_BUTTON_PRESS, 0.1, 1.5, modifier);
    mouse(fixture, GDK_MOTION_NOTIFY, 3.9, 2.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    for (int step = 0; step != 40; ++step) wheel(fixture, GDK_SCROLL_UP, 3.9, 0.5, modifier | GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    expect(boxrig_scrollbar_model_read(terminal, &current) == 0 && current.position == 0,
        "selection wheel clamps at the oldest history row");
    for (int step = 0; step != 40; ++step) wheel(fixture, GDK_SCROLL_DOWN, 3.9, 0.5, modifier | GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    expect(boxrig_scrollbar_model_read(terminal, &current) == 0 && current.position == current.history_rows,
        "reversed selection wheel clamps at live output");
    mouse(fixture, GDK_BUTTON_RELEASE, 3.9, 0.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    mouse(fixture, GDK_BUTTON_PRESS, 0.1, 1.5, modifier);
    mouse(fixture, GDK_MOTION_NOTIFY, 3.9, 2.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    const auto retained_copy = co_await clipboard(GDK_SELECTION_PRIMARY);
    wheel(fixture, GDK_SCROLL_UP, 3.9, 0.5, modifier | GDK_BUTTON1_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, 3.9, 0.5, GDK_BUTTON1_MASK);
    key(fixture, GDK_KEY_Escape, GDK_KEY_PRESS, 0);
    key(fixture, GDK_KEY_Escape, GDK_KEY_RELEASE, 0);
    co_await barrier(fixture, false);
    expect(!selected(fixture) && co_await clipboard(GDK_SELECTION_PRIMARY) == retained_copy,
        "Escape cancels a queued release without copying an unfinished endpoint");
    mouse(fixture, GDK_BUTTON_PRESS, 0.1, 1.5, modifier);
    mouse(fixture, GDK_MOTION_NOTIFY, 3.9, 2.5, GDK_BUTTON1_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, 3.9, 2.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    expect(selected(fixture), "a cancelled release does not capture the next drag");
    wheel(fixture, GDK_SCROLL_UP, 3.9, 0.5, GDK_CONTROL_MASK);
    co_await barrier(fixture, false);
    expect(boxrig_gtk3_terminal_widget_get_effective_metrics(terminal_runtime_widget(runtime), &after) == BOXRIG_FONT_CATALOG_OK &&
        after.requested_size_dip > font.requested_size_dip, "completed selection does not intercept ordinary Ctrl wheel zoom");
    bool changed = false;
    expect(boxrig_gtk3_terminal_widget_visual_zoom_reset(terminal_runtime_widget(runtime), &changed) == BOXRIG_UI_OK,
        "restore font after ordinary zoom");
    terminal_runtime_refresh(runtime);
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  }
  expect(terminal_runtime_feed(runtime, "\033[?1049h\033[?1007h\033[Halternate selection"), "enter alternate screen");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, GDK_CONTROL_MASK);
  mouse(fixture, GDK_MOTION_NOTIFY, 3.9, 0.5, GDK_BUTTON1_MASK | GDK_CONTROL_MASK);
  co_await barrier(fixture, false);
  fixture->outgoing.clear();
  wheel(fixture, GDK_SCROLL_UP, 3.9, 0.5, GDK_CONTROL_MASK | GDK_BUTTON1_MASK);
  mouse(fixture, GDK_BUTTON_RELEASE, 3.9, 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(fixture->outgoing == "\r" && co_await clipboard(GDK_SELECTION_PRIMARY) == "alte",
      "selection wheel is harmless on alternate screens without sending arrows");
  fixture->outgoing.clear();
  wheel(fixture, GDK_SCROLL_UP, 3.9, 0.5, 0);
  co_await barrier(fixture, false);
  expect(fixture->outgoing.starts_with("\033[A"), "ordinary alternate-screen wheel still reaches the terminal application");
  expect(terminal_runtime_feed(runtime, "\033[?1049l\033[?1007l"), "restore primary screen");
}

static cardio::promise<void> observe_main_loop(Fixture *fixture) {
  struct Progress { Fixture *fixture; int ticks = 0; } progress{fixture};
  // Observe delivered main-loop callbacks rather than sleeping or assuming a
  // frame rate. A running animation need not become globally idle.
  g_timeout_add_full(G_PRIORITY_DEFAULT, 50, +[](gpointer context) -> gboolean {
    auto *progress = static_cast<Progress *>(context);
    const bool done = ++progress->ticks == 4;
    progress->fixture->changed.trigger();
    return done ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
  }, &progress, nullptr);
  while (progress.ticks < 4)
    co_await fixture->changed.wait();
}

static cardio::promise<void> autoscroll_scenarios(Fixture *fixture) {
  auto *runtime = fixture->window->runtime;
  auto *terminal = boxrig_ui_model_terminal(boxrig_ui_view_model(view(fixture)));
  for (const auto modifier : {GDK_SHIFT_MASK, GDK_CONTROL_MASK}) {
    for (const auto ending : {0, 1, 2, 3, 4, 5, 6, 7, 8}) {
      std::string history = "\033[3J\033[2J\033[H";
      for (int row = 0; row != 200; ++row) history += std::to_string(1000 + row) + " alpha beta" + (row == 199 ? "" : "\r\n");
      expect(terminal_runtime_feed(runtime, history), "prepare autoscroll history");
      while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
      boxrig_scrollbar_model initial{};
      expect(boxrig_scrollbar_model_read(terminal, &initial) == 0, "initial autoscroll position");
      mouse(fixture, GDK_BUTTON_PRESS, 0.1, 1.5, modifier);
      mouse(fixture, GDK_MOTION_NOTIFY, 3.9, -3.5, GDK_BUTTON1_MASK);
      co_await observe_main_loop(fixture);
      boxrig_scrollbar_model moving{};
      expect(boxrig_scrollbar_model_read(terminal, &moving) == 0 && moving.position < initial.position,
          "a held pointer above the grid continuously scrolls history");
      if (modifier == GDK_CONTROL_MASK) {
        mouse(fixture, GDK_MOTION_NOTIFY, 0.1, -3.5, GDK_BUTTON1_MASK);
        boxrig_ui_selection empty{};
        while (!boxrig_ui_view_get_selection(view(fixture), &empty) || empty.row_count != 0)
          co_await fixture->changed.wait();
        expect(boxrig_ui_view_get_selection(view(fixture), &empty) && empty.row_count == 0,
            "autoscroll retains an empty rectangle");
        co_await observe_main_loop(fixture);
        boxrig_scrollbar_model empty_moving{};
        expect(boxrig_scrollbar_model_read(terminal, &empty_moving) == 0 && empty_moving.position < moving.position,
            "autoscroll continues while rectangle width is zero");
        mouse(fixture, GDK_MOTION_NOTIFY, 3.9, -3.5, GDK_BUTTON1_MASK);
        while (!boxrig_ui_view_get_selection(view(fixture), &empty) || empty.row_count == 0)
          co_await fixture->changed.wait();
        expect(selected_text(fixture).find('\n') != std::string::npos, "empty autoscroll rectangle grows again");
      }
      gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), "autoscroll primary", -1);
      if (ending == 0) mouse(fixture, GDK_BUTTON_RELEASE, 3.9, -3.5, GDK_BUTTON1_MASK);
      else if (ending == 1) {
        key(fixture, GDK_KEY_Escape, GDK_KEY_PRESS, 0);
        key(fixture, GDK_KEY_Escape, GDK_KEY_RELEASE, 0);
      } else if (ending == 2) {
        GdkEventFocus focus{};
        focus.type = GDK_FOCUS_CHANGE;
        focus.window = gtk_widget_get_window(GTK_WIDGET(terminal_runtime_widget(runtime)));
        focus.in = FALSE;
        gboolean handled = FALSE;
        g_signal_emit_by_name(terminal_runtime_widget(runtime), "focus-out-event", &focus, &handled);
      } else if (ending == 3) {
        expect(terminal_runtime_feed(runtime, "\033[3J\033[2J\033[Hinvalidated"), "evict autoscroll selection text");
      } else if (ending == 4) {
        expect(terminal_runtime_visual_zoom(runtime, BOXRIG_FONT_ZOOM_INCREASE) == BOXRIG_UI_OK, "zoom invalidates an active gesture");
      } else if (ending == 5) {
        boxrig_ui_layout_result layout{};
        expect(boxrig_ui_view_get_layout(view(fixture), &layout) == BOXRIG_UI_OK, "grid before gesture invalidation");
        expect(terminal_runtime_set_preferred_grid(runtime, layout.rows, layout.columns - 1) == BOXRIG_UI_OK,
            "column change invalidates an active gesture");
      } else if (ending == 6) {
        gtk_widget_hide(fixture->window->window);
        gtk_widget_show(fixture->window->window);
      } else if (ending == 7) {
        terminal_runtime_request_input_enabled(runtime, false);
      } else {
        mouse(fixture, GDK_BUTTON_PRESS, 3.9, 1.5, GDK_BUTTON1_MASK, GDK_BUTTON_SECONDARY);
        mouse(fixture, GDK_BUTTON_RELEASE, 3.9, 1.5, GDK_BUTTON1_MASK | GDK_BUTTON3_MASK, GDK_BUTTON_SECONDARY);
        co_await observe_main_loop(fixture);
        auto *menu = gtk_grab_get_current();
        expect(menu && GTK_IS_MENU(menu) && gtk_widget_get_visible(menu),
            "right click during a held selection opens the context menu on release");
        gtk_menu_popdown(GTK_MENU(menu));
      }
      // Focus loss disables the simple IM context, so use state notifications
      // instead of the keyboard barrier until focus is restored.
      while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
      boxrig_scrollbar_model stopped{};
      expect(boxrig_scrollbar_model_read(terminal, &stopped) == 0, "position at gesture end");
      co_await observe_main_loop(fixture);
      boxrig_scrollbar_model after{};
      boxrig_scrollbar_model_read(terminal, &after);
      if (after.position != stopped.position)
        g_printerr("autoscroll ending=%d changed position from %zu to %zu\n", ending, stopped.position, after.position);
      expect(boxrig_scrollbar_model_read(terminal, &after) == 0 && after.position == stopped.position,
          "gesture endings, focus, text, font, grid, mapping and input inhibition stop autoscroll");
      if (ending != 0) {
        mouse(fixture, GDK_MOTION_NOTIFY, 8.9, -3.5, GDK_BUTTON1_MASK);
        mouse(fixture, GDK_BUTTON_RELEASE, 8.9, -3.5, GDK_BUTTON1_MASK);
        co_await observe_main_loop(fixture);
        while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
        expect(co_await clipboard(GDK_SELECTION_PRIMARY) == "autoscroll primary", "cancelled autoscroll never copies on release");
      }
      if (ending == 2) {
        expect(selected(fixture), "focus loss retains the displayed selection");
        GdkEventFocus focus{};
        focus.type = GDK_FOCUS_CHANGE;
        focus.window = gtk_widget_get_window(GTK_WIDGET(terminal_runtime_widget(runtime)));
        focus.in = TRUE;
        gboolean handled = FALSE;
        g_signal_emit_by_name(terminal_runtime_widget(runtime), "focus-in-event", &focus, &handled);
      }
      if (ending == 4) {
        bool changed = false;
        expect(boxrig_gtk3_terminal_widget_visual_zoom_reset(terminal_runtime_widget(runtime), &changed) == BOXRIG_UI_OK,
            "restore font after cancellation");
        terminal_runtime_refresh(runtime);
      } else if (ending == 5) {
        boxrig_ui_layout_result layout{};
        expect(boxrig_ui_view_get_layout(view(fixture), &layout) == BOXRIG_UI_OK, "grid after cancellation");
        expect(terminal_runtime_set_preferred_grid(runtime, layout.rows, layout.columns + 1) == BOXRIG_UI_OK, "restore columns");
      } else if (ending == 6) {
        gtk_widget_grab_focus(GTK_WIDGET(terminal_runtime_widget(runtime)));
        GdkEventFocus focus{};
        focus.type = GDK_FOCUS_CHANGE;
        focus.window = gtk_widget_get_window(GTK_WIDGET(terminal_runtime_widget(runtime)));
        focus.in = TRUE;
        gboolean handled = FALSE;
        g_signal_emit_by_name(terminal_runtime_widget(runtime), "focus-in-event", &focus, &handled);
      } else if (ending == 7) terminal_runtime_request_input_enabled(runtime, true);
      while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    }
  }
}

static cardio::promise<void> copy_policy_scenarios(Fixture *fixture) {
  auto *runtime = fixture->window->runtime;
  auto settings = create_default_settings(default_terminal_display_settings(16), "Copy policy");
  expect(apply_terminal_presentation(fixture->presentation, settings, nullptr), "apply default copy settings");
  expect(terminal_runtime_feed(runtime, "\033[3J\033[2J\033[Habc   \r\nabc    X\r\n        \r\n       X\r\n  ab    \r\n        "),
      "prepare trailing whitespace, unselected text, indentation and empty rows");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, GDK_CONTROL_MASK);
  mouse(fixture, GDK_MOTION_NOTIFY, 4.9, 5.5, GDK_BUTTON1_MASK);
  mouse(fixture, GDK_BUTTON_RELEASE, 4.9, 5.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  const std::string trimmed = "abc\nabc  \n\n     \n  ab\n";
  const std::string preserved = "abc  \nabc  \n     \n     \n  ab \n     ";
  expect(co_await clipboard(GDK_SELECTION_PRIMARY) == trimmed, "unspecified copy settings trim physical trailing whitespace");

  for (const bool trim : {false, true}) {
    boxrig_ui_selection before{}, after{};
    expect(boxrig_ui_view_get_selection(view(fixture), &before), "capture existing rectangular selection");
    set_setting_value(&settings, terminal_block_copy_trim_trailing_spaces_setting_key(), trim);
    expect(apply_terminal_presentation(fixture->presentation, settings, nullptr), "apply changed copy settings");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    const auto &expected = trim ? trimmed : preserved;
    expect(selected_text(fixture) == expected, "changed settings affect the same selection's copy text");
    expect(boxrig_ui_view_get_selection(view(fixture), &after) &&
        before.visual_start_column == after.visual_start_column && before.visual_column_count == after.visual_column_count &&
        before.row_count == after.row_count && before.shape == after.shape,
        "changing copy settings preserves the displayed rectangle");
    key(fixture, GDK_KEY_c, GDK_KEY_PRESS, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    key(fixture, GDK_KEY_c, GDK_KEY_RELEASE, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    co_await barrier(fixture, false);
    expect(co_await clipboard(GDK_SELECTION_CLIPBOARD) == expected, "keyboard copy uses the current setting");

    mouse(fixture, GDK_BUTTON_PRESS, 2.1, 0.5, 0, GDK_BUTTON_SECONDARY);
    mouse(fixture, GDK_BUTTON_RELEASE, 2.1, 0.5, GDK_BUTTON3_MASK, GDK_BUTTON_SECONDARY);
    co_await observe_main_loop(fixture);
    auto *menu = gtk_grab_get_current();
    expect(menu && GTK_IS_MENU(menu), "open the copy menu for a retained rectangle");
    auto *items = gtk_container_get_children(GTK_CONTAINER(menu));
    expect(items && gtk_widget_get_sensitive(GTK_WIDGET(items->data)), "the retained rectangle enables Copy");
    gtk_menu_item_activate(GTK_MENU_ITEM(items->data));
    g_list_free(items);
    gtk_menu_popdown(GTK_MENU(menu));
    co_await barrier(fixture, false);
    expect(co_await clipboard(GDK_SELECTION_CLIPBOARD) == expected, "context-menu copy uses the current setting");

    mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, GDK_CONTROL_MASK);
    mouse(fixture, GDK_MOTION_NOTIFY, 4.9, 5.5, GDK_BUTTON1_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, 4.9, 5.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    expect(co_await clipboard(GDK_SELECTION_PRIMARY) == expected, "selection completion applies the setting to PRIMARY");
  }
  mouse(fixture, GDK_BUTTON_PRESS, 0.1, 2.5, GDK_CONTROL_MASK);
  mouse(fixture, GDK_MOTION_NOTIFY, 4.9, 2.5, GDK_BUTTON1_MASK);
  mouse(fixture, GDK_BUTTON_RELEASE, 4.9, 2.5, GDK_BUTTON1_MASK);
  key(fixture, GDK_KEY_c, GDK_KEY_PRESS, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  key(fixture, GDK_KEY_c, GDK_KEY_RELEASE, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  co_await barrier(fixture, false);
  expect(selected_text(fixture).empty() && co_await clipboard(GDK_SELECTION_PRIMARY) == "" &&
      co_await clipboard(GDK_SELECTION_CLIPBOARD) == "", "a positive-width blank rectangle copies an empty string to both selections");
}

static cardio::promise<void> copy_scenarios(Fixture *fixture, bool trim) {
  auto *runtime = fixture->window->runtime;
  auto settings = create_default_settings(default_terminal_display_settings(16), "Copy policy");
  set_setting_value(&settings, terminal_block_copy_trim_trailing_spaces_setting_key(), trim);
  expect(apply_terminal_presentation(fixture->presentation, settings, nullptr), "apply history copy policy");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  const auto copy = [&] {
    key(fixture, GDK_KEY_c, GDK_KEY_PRESS, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    key(fixture, GDK_KEY_c, GDK_KEY_RELEASE, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  };
  const auto check_text = [&](const std::string &expected) -> cardio::promise<void> {
    expect(co_await clipboard(GDK_SELECTION_PRIMARY) == expected, "PRIMARY contains complete selected graphemes and physical rows");
    copy();
    co_await barrier(fixture, false);
    expect(co_await clipboard(GDK_SELECTION_CLIPBOARD) == expected, "Copy exports the same complete selection to CLIPBOARD");
  };
  boxrig_ui_layout_result layout{};
  expect(boxrig_ui_view_get_layout(view(fixture), &layout) == BOXRIG_UI_OK, "copy fixture grid");
  std::string history = "\033[3J\033[2J\033[H\033[?25l0123456789";
  std::string expected = "234";
  const std::string lines[] = {"A界é👩‍💻z  ", "AאבגדZ  ", ""};
  const std::string slices[] = {"界é👩‍💻", "אבג", trim ? "" : "   "};
  for (int row = 0; row != 90; ++row) {
    history += "\r\n" + lines[row % 3];
    expected += "\n" + slices[row % 3];
  }
  history += "\r\n0123456789";
  expected += "\n234";
  expect(terminal_runtime_feed(runtime, history), "feed Unicode history in one batch before rendering");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  boxrig_scrollbar_model model{};
  auto *terminal = boxrig_ui_model_terminal(boxrig_ui_view_model(view(fixture)));
  expect(boxrig_scrollbar_model_read(terminal, &model) == 0 && model.history_rows + layout.rows == 92,
      "Unicode fixture occupies 92 physical rows");
  bool changed = false;
  expect(boxrig_ui_view_apply_scrollbar_action(view(fixture), BOXRIG_SCROLLBAR_POSITION, 0, &changed) == BOXRIG_UI_OK,
      "show the first Unicode history rows");
  terminal_runtime_refresh(runtime);
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  mouse(fixture, GDK_BUTTON_PRESS, 2.1, 0.5, GDK_CONTROL_MASK);
  mouse(fixture, GDK_MOTION_NOTIFY, 4.9, 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  // Jump over the middle instead of rendering it. The native layout provider
  // must resolve its Unicode and bidi cells while materializing the rectangle.
  expect(boxrig_ui_view_apply_scrollbar_action(view(fixture), BOXRIG_SCROLLBAR_POSITION, model.history_rows, &changed) == BOXRIG_UI_OK,
      "jump across unvisited history");
  terminal_runtime_refresh(runtime);
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  mouse(fixture, GDK_MOTION_NOTIFY, 4.9, layout.rows - 0.5, GDK_BUTTON1_MASK);
  mouse(fixture, GDK_BUTTON_RELEASE, 4.9, layout.rows - 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(selected_text(fixture) == expected, "unvisited Unicode history preserves whole graphemes, bidi logical order and blank padding");
  co_await check_text(expected);

  const std::string wrapped = std::string(layout.columns, 'x') + "AB";
  expect(terminal_runtime_feed(runtime, "\033[3J\033[2J\033[H" + wrapped + "\r\ntail"), "prepare a soft-wrapped logical line");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  for (const auto modifier : {GDK_SHIFT_MASK, GDK_CONTROL_MASK}) {
    mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, modifier);
    mouse(fixture, GDK_MOTION_NOTIFY, 3.9, 2.5, GDK_BUTTON1_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, 3.9, 2.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    co_await check_text(modifier == GDK_CONTROL_MASK ? (trim ? "xxxx\nAB\ntail" : "xxxx\nAB  \ntail") : wrapped + "\ntail");
  }

  for (const bool stale : {false, true}) {
    expect(terminal_runtime_feed(runtime, "\033[2J\033[Halpha\r\nbeta"), "prepare empty or stale copy");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), "keep PRIMARY", -1);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), "keep CLIPBOARD", -1);
    mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, GDK_CONTROL_MASK);
    mouse(fixture, GDK_MOTION_NOTIFY, stale ? 3.9 : 0.1, 1.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    if (stale) {
      expect(terminal_runtime_feed(runtime, "\033[Hchanged"), "invalidate copy selection");
      while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    }
    mouse(fixture, GDK_BUTTON_RELEASE, stale ? 3.9 : 0.1, 1.5, GDK_BUTTON1_MASK);
    copy();
    co_await barrier(fixture, false);
    expect(co_await clipboard(GDK_SELECTION_PRIMARY) == "keep PRIMARY" &&
        co_await clipboard(GDK_SELECTION_CLIPBOARD) == "keep CLIPBOARD", "empty or stale copies preserve both OS selections");
    mouse(fixture, GDK_BUTTON_PRESS, 2.1, 0.5, 0, GDK_BUTTON_SECONDARY);
    mouse(fixture, GDK_BUTTON_RELEASE, 2.1, 0.5, GDK_BUTTON3_MASK, GDK_BUTTON_SECONDARY);
    co_await observe_main_loop(fixture);
    auto *menu = gtk_grab_get_current();
    expect(menu && GTK_IS_MENU(menu), "open Copy menu for an empty or stale selection");
    auto *items = gtk_container_get_children(GTK_CONTAINER(menu));
    const bool enabled = items && gtk_widget_get_sensitive(GTK_WIDGET(items->data));
    g_list_free(items);
    gtk_menu_popdown(GTK_MENU(menu));
    expect(!enabled, "Copy menu is disabled for empty and stale rectangles");
  }
}

static cardio::promise<void> limit_scenarios(Fixture *fixture) {
  auto *runtime = fixture->window->runtime;
  expect(terminal_runtime_feed(runtime, "\033[2J\033[Habcdefghijkl\r\nmnopqrstuvwx\r\nABCDEFGHIJKL\r\nMNOPQRSTUVWX"),
      "prepare bounded selection text");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), "bounded PRIMARY", -1);
  gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), "bounded CLIPBOARD", -1);
  mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, GDK_CONTROL_MASK);
  mouse(fixture, GDK_MOTION_NOTIFY, 1.9, 1.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(selected_text(fixture) == "ab\nmn", "retain a valid rectangle below every configured limit");
  selection_warnings.clear();
  if (selection_limit == "allocation") reject_selection_update = true;
  mouse(fixture, GDK_MOTION_NOTIFY, 3.9, 3.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  if (selection_limit != "bytes") {
    expect(selected_text(fixture) == "ab\nmn", "rejected rectangle growth preserves the complete previous selection");
  } else {
    boxrig_ui_selection selection{};
    expect(boxrig_ui_view_get_selection(view(fixture), &selection) && selection.row_count == 4 && !selection.stale,
        "a copy byte limit does not discard a valid displayed selection");
  }
  mouse(fixture, GDK_BUTTON_RELEASE, 3.9, 3.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(co_await clipboard(GDK_SELECTION_PRIMARY) == "bounded PRIMARY" &&
      co_await clipboard(GDK_SELECTION_CLIPBOARD) == "bounded CLIPBOARD", "failed selection or primary copy never publishes partial text");
  expect(!selection_warnings.empty(), "the application reports a selection or copy failure");
  if (selection_limit == "bytes") {
    selection_warnings.clear();
    key(fixture, GDK_KEY_c, GDK_KEY_PRESS, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    key(fixture, GDK_KEY_c, GDK_KEY_RELEASE, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    co_await barrier(fixture, false);
    expect(!selection_warnings.empty() && co_await clipboard(GDK_SELECTION_CLIPBOARD) == "bounded CLIPBOARD",
        "Copy reports its byte limit and preserves the previous clipboard");
  }
  mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, GDK_CONTROL_MASK);
  mouse(fixture, GDK_MOTION_NOTIFY, 1.9, 1.5, GDK_BUTTON1_MASK);
  mouse(fixture, GDK_BUTTON_RELEASE, 1.9, 1.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(co_await clipboard(GDK_SELECTION_PRIMARY) == "ab\nmn", "a new smaller selection still succeeds after a rejected operation");
  if (selection_limit == "bytes") {
    selection_warnings.clear();
    mouse(fixture, GDK_2BUTTON_PRESS, 0.1, 0.5, 0);
    mouse(fixture, GDK_BUTTON_RELEASE, 0.1, 0.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    expect(!selection_warnings.empty() && selected_text(fixture) == "ab\nmn" &&
        co_await clipboard(GDK_SELECTION_PRIMARY) == "ab\nmn", "a rejected word-selection start preserves the old selection and clipboard");
    selection_warnings.clear();
    mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, GDK_SHIFT_MASK);
    mouse(fixture, GDK_MOTION_NOTIFY, 11.9, 0.5, GDK_BUTTON1_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, 11.9, 0.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    boxrig_ui_selection selection{};
    expect(!selection_warnings.empty() && boxrig_ui_view_get_selection(view(fixture), &selection) && selection.range_valid &&
        co_await clipboard(GDK_SELECTION_PRIMARY) == "ab\nmn", "linear copy byte limits retain selection and the previous clipboard too");
  } else if (selection_limit == "rows" || selection_limit == "ranges") {
    std::string history = "\033[3J\033[2J\033[H";
    for (int row = 0; row != 100; ++row) history += std::to_string(1000 + row) + (row == 99 ? "" : "\r\n");
    expect(terminal_runtime_feed(runtime, history), "prepare bounded autoscroll");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    mouse(fixture, GDK_BUTTON_PRESS, 0.1, 1.5, GDK_CONTROL_MASK);
    mouse(fixture, GDK_MOTION_NOTIFY, 3.9, 0.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    const auto retained = selected_text(fixture);
    selection_warnings.clear();
    mouse(fixture, GDK_MOTION_NOTIFY, 3.9, -2.5, GDK_BUTTON1_MASK);
    co_await observe_main_loop(fixture);
    co_await barrier(fixture, false);
    expect(!selection_warnings.empty() && selected_text(fixture) == retained, "autoscroll stops when its deferred extension exceeds a limit");
    boxrig_scrollbar_model stopped{}, after{};
    auto *terminal = boxrig_ui_model_terminal(boxrig_ui_view_model(view(fixture)));
    expect(boxrig_scrollbar_model_read(terminal, &stopped) == 0, "bounded scroll position");
    co_await observe_main_loop(fixture);
    expect(boxrig_scrollbar_model_read(terminal, &after) == 0 && after.position == stopped.position,
        "rejected autoscroll leaves no continuing timer");
    mouse(fixture, GDK_BUTTON_RELEASE, 3.9, -2.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    expect(co_await clipboard(GDK_SELECTION_PRIMARY) == "ab\nmn", "releasing a rejected autoscroll does not copy partial text");
  }
}

static cardio::promise<void> scenarios(Fixture *fixture) {
  auto *runtime = fixture->window->runtime;
  TerminalTransportRequest request;
  request.open = [fixture](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<TerminalTransport> {
    co_return TerminalTransport{
      [fixture](std::span<unsigned char>, cardio::cancellation stop) -> cardio::promise<std::size_t> {
        co_await fixture->received.wait(stop);
        co_return 0;
      },
      [fixture](std::span<const unsigned char> bytes, cardio::cancellation stop) -> cardio::promise<void> {
        stop.throw_if_cancellation_requested();
        fixture->outgoing.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        fixture->changed.trigger();
        co_return;
      },
      [](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<void> { co_return; },
      []() -> cardio::promise<void> { co_return; }};
  };
  expect(terminal_runtime_connect_transport(runtime, std::move(request)), "connect observable selection peer");
  set_main_window_connection_phase(fixture->window, TerminalSessionConnectionPhase::connected);
  while (!terminal_runtime_is_settled(runtime) ||
      terminal_runtime_connection_state(runtime).phase != TerminalConnectionPhase::connected)
    co_await fixture->changed.wait();
  gtk_widget_grab_focus(GTK_WIDGET(terminal_runtime_widget(runtime)));
  GdkEventFocus focus{};
  focus.type = GDK_FOCUS_CHANGE;
  focus.window = gtk_widget_get_window(GTK_WIDGET(terminal_runtime_widget(runtime)));
  focus.in = TRUE;
  gboolean handled = FALSE;
  g_signal_emit_by_name(terminal_runtime_widget(runtime), "focus-in-event", &focus, &handled);
  gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), "retained primary", -1);
  gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), "retained clipboard", -1);

  if (!selection_limit.empty()) {
    co_await limit_scenarios(fixture);
    co_return;
  }

  for (const bool kitty : {false, true}) {
    expect(terminal_runtime_feed(runtime, kitty ? "\033[>11u" : "\033[=0u"), "set keyboard reporting");
    const std::string marker = kitty ? "\033[13u\033[13;1:3u" : "\r";
    for (const auto scenario : {0, 1, 2, 3}) {
      expect(terminal_runtime_feed(runtime, "\033[2J\033[HSELECTABLE TEXT"), "prepare selectable text");
      while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
      select(fixture, scenario == 0 ? BOXRIG_UI_SELECTION_LINEAR : BOXRIG_UI_SELECTION_RECTANGULAR, scenario == 2);
      if (scenario == 3) {
        expect(terminal_runtime_feed(runtime, "\033[Hchanged"), "invalidate selected text");
        while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
        boxrig_ui_selection selection{};
        expect(boxrig_ui_view_get_selection(view(fixture), &selection) && selection.stale, "retain stale rectangle");
      }
      fixture->outgoing.clear();
      key(fixture, GDK_KEY_Escape, GDK_KEY_PRESS, GDK_LOCK_MASK);
      key(fixture, GDK_KEY_Escape, GDK_KEY_PRESS, GDK_LOCK_MASK);
      key(fixture, GDK_KEY_Escape, GDK_KEY_RELEASE, GDK_LOCK_MASK);
      co_await barrier(fixture, kitty);
      expect(!selected(fixture), "Escape clears normal, rectangular, empty and stale selections despite Caps Lock");
      if (fixture->outgoing != marker) {
        g_printerr("keyboard kitty=%d scenario=%d received:", kitty, scenario);
        for (const unsigned char byte : fixture->outgoing) g_printerr(" %02x", byte);
        g_printerr("\n");
      }
      expect(fixture->outgoing == marker, "cancelled Escape repeat and release never reach the peer");
      expect(co_await clipboard(GDK_SELECTION_PRIMARY) == "retained primary", "Escape preserves PRIMARY");
      expect(co_await clipboard(GDK_SELECTION_CLIPBOARD) == "retained clipboard", "Escape preserves CLIPBOARD");
    }
    fixture->outgoing.clear();
    key(fixture, GDK_KEY_Escape, GDK_KEY_PRESS, 0);
    key(fixture, GDK_KEY_Escape, GDK_KEY_RELEASE, 0);
    co_await barrier(fixture, kitty);
    expect(fixture->outgoing == (kitty ? "\033[27u\033[27;1:3u" : "\033") + marker,
        "Escape without a selection retains the terminal protocol");
    for (const auto modifier : {GDK_SHIFT_MASK, GDK_CONTROL_MASK, GDK_MOD1_MASK, GDK_SUPER_MASK}) {
      select(fixture, BOXRIG_UI_SELECTION_LINEAR, false);
      fixture->outgoing.clear();
      key(fixture, GDK_KEY_Escape, GDK_KEY_PRESS, modifier);
      key(fixture, GDK_KEY_Escape, GDK_KEY_RELEASE, modifier);
      co_await barrier(fixture, kitty);
      expect(selected(fixture) && fixture->outgoing.size() > marker.size(), "modified Escape remains terminal input");
    }
    expect(boxrig_ui_view_clear_selection(view(fixture)) == BOXRIG_UI_OK, "clear modified-key selection");
    expect(terminal_runtime_feed(runtime, "\033[?1002h\033[?1006h"), "enable terminal mouse reports");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    for (const auto modifier : {GDK_SHIFT_MASK, GDK_CONTROL_MASK}) {
      fixture->outgoing.clear();
      mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, modifier);
      mouse(fixture, GDK_MOTION_NOTIFY, 8.9, 0.5, modifier | GDK_BUTTON1_MASK);
      co_await barrier(fixture, kitty);
      expect(selected(fixture), "a held override drag has a selection");
      fixture->outgoing.clear();
      key(fixture, GDK_KEY_Escape, GDK_KEY_PRESS, 0);
      key(fixture, GDK_KEY_Escape, GDK_KEY_RELEASE, 0);
      mouse(fixture, GDK_MOTION_NOTIFY, 12.9, 1.5, GDK_BUTTON1_MASK);
      mouse(fixture, GDK_BUTTON_RELEASE, 12.9, 1.5, GDK_BUTTON1_MASK);
      co_await barrier(fixture, kitty);
      expect(!selected(fixture) && fixture->outgoing == marker,
          "cancelled held drag never resumes selection or leaks mouse/key events");
      expect(co_await clipboard(GDK_SELECTION_PRIMARY) == "retained primary", "cancelled release does not copy");
    }
    expect(terminal_runtime_feed(runtime, "\033[?1002l\033[?1006l"), "restore ordinary mouse input");
  }
  co_await click_scenarios(fixture);
  co_await scroll_scenarios(fixture);
  co_await autoscroll_scenarios(fixture);
  co_await copy_policy_scenarios(fixture);
  for (const bool trim : {true, false}) co_await copy_scenarios(fixture, trim);
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  select(fixture, BOXRIG_UI_SELECTION_RECTANGULAR, false);
  auto *composition = boxrig_gtk3_terminal_widget_composition(terminal_runtime_widget(runtime));
  expect(boxrig_composition_set_commit_sink(composition, +[](const char *text, std::size_t size, void *context) {
    auto *fixture = static_cast<Fixture *>(context);
    if (!fixture->accept_commit) return BOXRIG_STATUS_WOULD_BLOCK;
    fixture->committed.append(text, size);
    return BOXRIG_STATUS_OK;
  }, fixture) == BOXRIG_COMPOSITION_OK, "install a bounded commit destination");
  boxrig_composition_event commit{};
  commit.type = BOXRIG_COMPOSITION_COMMIT;
  commit.text = "確定";
  commit.text_length = std::char_traits<char>::length(commit.text);
  expect(boxrig_composition_apply_event(composition, &commit) == BOXRIG_COMPOSITION_WOULD_BLOCK,
      "composition retains an accepted commit while the destination is full");
  boxrig_key_event escape{};
  escape.key = BOXRIG_KEY_ESCAPE;
  escape.action = BOXRIG_KEY_PRESS;
  expect(terminal_runtime_route_selection_key(runtime, escape) == BOXRIG_INPUT_KEY_ROUTE_TERMINAL && selected(fixture),
      "pending commit keeps Escape in the IME path and retains selection");
  fixture->accept_commit = true;
  expect(boxrig_composition_retry_commit(composition) == BOXRIG_COMPOSITION_OK && fixture->committed == "確定",
      "selection routing does not discard or duplicate the accepted commit");
  expect(terminal_runtime_route_selection_key(runtime, escape) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION && !selected(fixture),
      "selection cancellation resumes after commit delivery");

  auto *terminal = boxrig_ui_model_terminal(boxrig_ui_view_model(view(fixture)));
  for (const bool fail : {false, true}) {
    std::string history = "\033[3J\033[2J\033[H";
    for (int row = 0; row != 200; ++row) history += "retained history" + std::string(row == 199 ? "" : "\r\n");
    expect(terminal_runtime_feed(runtime, history), "prepare selection before connection lifecycle changes");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    mouse(fixture, GDK_BUTTON_PRESS, 0.1, 1.5, GDK_CONTROL_MASK);
    mouse(fixture, GDK_MOTION_NOTIFY, 3.9, -3.5, GDK_BUTTON1_MASK);
    co_await observe_main_loop(fixture);
    boxrig_scrollbar_model moving{};
    expect(boxrig_scrollbar_model_read(terminal, &moving) == 0 && moving.position < moving.history_rows,
        "autoscroll is running before disconnect or view failure");
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), "lifecycle primary", -1);
    auto *previous = terminal_runtime_widget(runtime);
    g_object_ref(previous);
    if (fail) {
      reject_frame = true;
      boxrig_ui_view_invalidate_colors(view(fixture));
      while (!rejected_frame) co_await fixture->changed.wait();
    }
    co_await terminal_runtime_disconnect(runtime);
    boxrig_scrollbar_model stopped{};
    expect(boxrig_scrollbar_model_read(terminal, &stopped) == 0, "position after disconnect");
    co_await observe_main_loop(fixture);
    boxrig_scrollbar_model after{};
    expect(boxrig_scrollbar_model_read(terminal, &after) == 0 && after.position == stopped.position,
        "disconnect or view failure removes the active scroll callback");
    expect(terminal_runtime_connect_local(runtime, {{"/bin/cat"}}), "reconnect after cancelled autoscroll");
    if (fail) expect(terminal_runtime_widget(runtime) != previous, "view failure replaces the native widget");
    while (terminal_runtime_connection_state(runtime).phase != TerminalConnectionPhase::connected ||
        !terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    expect(boxrig_ui_model_terminal(boxrig_ui_view_model(view(fixture))) == terminal, "view replacement retains the terminal core");
    boxrig_ui_rectangular_copy_policy copy_policy;
    expect(boxrig_ui_view_get_rectangular_copy_policy(view(fixture), &copy_policy) == BOXRIG_UI_OK &&
        copy_policy == BOXRIG_UI_RECTANGULAR_COPY_PRESERVE_SPACES,
        "reconnected and replacement views retain the explicitly disabled trimming setting");
    boxrig_scrollbar_model reconnected{};
    expect(boxrig_scrollbar_model_read(terminal, &reconnected) == 0, "position after reconnect");
    co_await observe_main_loop(fixture);
    expect(boxrig_scrollbar_model_read(terminal, &after) == 0 && after.position == reconnected.position &&
        co_await clipboard(GDK_SELECTION_PRIMARY) == "lifecycle primary",
        "the retired gesture cannot scroll or copy into the replacement view");
    g_object_unref(previous);
  }
}

static cardio::promise<void> run(Fixture *fixture, std::exception_ptr *failure) {
  try {
    co_await scenarios(fixture);
    co_await terminal_runtime_disconnect(fixture->window->runtime);
    if (!selection_limit.empty()) {
      gtk_main_quit();
      co_return;
    }
    auto *terminal = boxrig_ui_model_terminal(boxrig_ui_view_model(view(fixture)));
    boxrig_scrollbar_model before{};
    expect(boxrig_scrollbar_model_read(terminal, &before) == 0, "history before destruction");
    mouse(fixture, GDK_BUTTON_PRESS, 0.1, 1.5, GDK_CONTROL_MASK);
    mouse(fixture, GDK_MOTION_NOTIFY, 3.9, -3.5, GDK_BUTTON1_MASK);
    co_await observe_main_loop(fixture);
    boxrig_scrollbar_model moving{};
    expect(boxrig_scrollbar_model_read(terminal, &moving) == 0 && moving.position < before.position,
        "a disconnected terminal can still select retained history before destruction");
    terminal_runtime_set_changed_callback(fixture->window->runtime, nullptr, nullptr);
    terminal_runtime_set_host_callbacks(fixture->window->runtime, {});
    destroy_terminal_presentation(fixture->presentation);
    fixture->presentation = nullptr;
    destroy_terminal_runtime(fixture->window->runtime);
    fixture->window->runtime = nullptr;
    fixture->window->terminal = nullptr;
    // Deliver more main-loop callbacks after destroying an actively scrolling
    // widget. G_DEBUG=fatal-criticals also diagnoses late GTK object access.
    co_await observe_main_loop(fixture);
  }
  catch (...) { *failure = std::current_exception(); }
  if (fixture->window->runtime) co_await terminal_runtime_disconnect(fixture->window->runtime);
  gtk_main_quit();
}

extern "C" int __wrap_main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  if (argc > 1) selection_limit = argv[1];
  g_log_set_handler(nullptr, G_LOG_LEVEL_WARNING, +[](const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer data) {
    selection_warnings += message;
    g_log_default_handler(domain, level, message, data);
  }, nullptr);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto window = load_main_window();
  if (!window) return 1;
  Fixture fixture{&*window, create_terminal_presentation(&*window, {}, {}, {}), {}, {}, {}, false, {}};
  terminal_runtime_set_host_callbacks(window->runtime, {
      [&fixture](BoxrigGtk3TerminalWidget *) {
        attach_main_window_terminal(fixture.window);
        attach_terminal_presentation(fixture.presentation);
      }, {}, {}, [&fixture](const boxrig_mouse_event *event, const boxrig_ui_hit_test_result *) {
        const auto route = route_main_window_terminal_mouse(fixture.window, *event);
        return route == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL ?
            route_terminal_presentation_mouse(fixture.presentation, *event) : route;
      }});
  terminal_runtime_set_changed_callback(window->runtime, [](void *context) {
    static_cast<Fixture *>(context)->changed.trigger();
  }, &fixture);
  gtk_widget_show(window->window);
  std::exception_ptr failure;
  auto completion = run(&fixture, &failure);
  gtk_main();
  if (window->runtime) {
    terminal_runtime_set_changed_callback(window->runtime, nullptr, nullptr);
    terminal_runtime_set_host_callbacks(window->runtime, {});
    destroy_terminal_presentation(fixture.presentation);
    destroy_terminal_runtime(window->runtime);
  }
  window->runtime = nullptr;
  window->terminal = nullptr;
  gtk_widget_destroy(window->window);
  release_main_window(&*window);
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { g_printerr("%s\n", error.what()); }
    return 1;
  }
  std::puts("PASS selection cancellation, retained clipboards and complete native input sequences");
  return 0;
}

// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-presentation.h"
#include "../../src/terminal-runtime.h"

#include <cstdio>
#include <exception>
#include <stdexcept>

using namespace elder_terms;

struct Fixture {
  MainWindow *window;
  TerminalPresentation *presentation = nullptr;
  SettingsStore store;
  cardio::primitives::conditional changed;
};

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> scenarios(Fixture *fixture) {
  auto *runtime = fixture->window->runtime;
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  refresh_terminal_presentation(fixture->presentation);
  struct Snapshot { gint width, height; float font; std::size_t rows, columns; };
  const auto snapshot = [&] {
    auto *widget = terminal_runtime_widget(runtime);
    Snapshot result{};
    gtk_window_get_size(GTK_WINDOW(fixture->window->window), &result.width, &result.height);
    boxrig_font_effective_metrics metrics{};
    expect(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) == BOXRIG_FONT_CATALOG_OK,
        "read effective zoom metrics");
    result.font = metrics.requested_size_dip;
    boxrig_terminal_size(boxrig_ui_model_terminal(boxrig_ui_view_model(boxrig_gtk3_terminal_widget_view(widget))),
        &result.rows, &result.columns);
    return result;
  };
  boxrig_mouse_event wheel{};
  wheel.action = BOXRIG_MOUSE_PRESS;
  wheel.button = BOXRIG_MOUSE_BUTTON_WHEEL_UP;
  wheel.modifiers = BOXRIG_MODIFIER_CONTROL;
  const auto initial = snapshot();
  set_setting_value(&fixture->store, terminal_font_zoom_mode_setting_key(), std::string("preserve-window"));
  expect(apply_terminal_presentation(fixture->presentation, fixture->store, nullptr), "apply window mode");
  expect(route_terminal_presentation_mouse(fixture->presentation, wheel) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION,
      "queue input under window mode");
  set_setting_value(&fixture->store, terminal_font_zoom_mode_setting_key(), std::string("preserve-grid"));
  expect(apply_terminal_presentation(fixture->presentation, fixture->store, nullptr), "change mode before queued input dispatch");
  do { co_await fixture->changed.wait(); }
  while (!terminal_runtime_is_settled(runtime) || snapshot().font == initial.font);
  const auto window_zoom = snapshot();
  expect(window_zoom.width == initial.width && window_zoom.height == initial.height &&
      (window_zoom.rows != initial.rows || window_zoom.columns != initial.columns),
      "already accepted input must retain its original mode after settings apply");

  expect(route_terminal_presentation_mouse(fixture->presentation, wheel) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION,
      "queue input under the applied grid mode");
  do { co_await fixture->changed.wait(); }
  while (!terminal_runtime_is_settled(runtime) || snapshot().font == window_zoom.font);
  const auto grid_zoom = snapshot();
  expect(grid_zoom.rows == window_zoom.rows && grid_zoom.columns == window_zoom.columns &&
      (grid_zoom.width > window_zoom.width || grid_zoom.height > window_zoom.height),
      "subsequent input must retain the effective grid inherited from window mode");

  set_setting_value(&fixture->store, terminal_font_zoom_mode_setting_key(), std::string("preserve-window"));
  expect(apply_terminal_presentation(fixture->presentation, fixture->store, nullptr), "restore window mode");
  expect(terminal_runtime_visual_zoom(runtime, BOXRIG_FONT_ZOOM_INCREASE, true) == BOXRIG_UI_OK,
      "begin a window-preserving zoom before direct font editing");
  set_setting_value(&fixture->store, terminal_font_size_dip_setting_key(), gdouble{20});
  expect(apply_terminal_presentation(fixture->presentation, fixture->store, nullptr), "edit font before zoom completion");
  while (!terminal_runtime_is_settled(runtime) || snapshot().font != 20) co_await fixture->changed.wait();
  const auto direct = snapshot();
  expect(direct.rows == grid_zoom.rows && direct.columns == grid_zoom.columns &&
      (direct.width > grid_zoom.width || direct.height > grid_zoom.height),
      "a direct font edit must resize the window rather than inherit the unfinished zoom's policy");

  expect(terminal_runtime_visual_zoom(runtime, BOXRIG_FONT_ZOOM_INCREASE, true) == BOXRIG_UI_OK,
      "begin window zoom before an external resize");
  gtk_window_resize(GTK_WINDOW(fixture->window->window), direct.width + 39, direct.height + 25);
  while (!terminal_runtime_is_settled(runtime) || snapshot().font == direct.font) co_await fixture->changed.wait();
  const auto resized = snapshot();
  expect(resized.width == direct.width + 39 && resized.height == direct.height + 25,
      "later external allocation must not be undone by an earlier zoom");
  set_setting_value(&fixture->store, terminal_font_zoom_mode_setting_key(), std::string("preserve-grid"));
  expect(apply_terminal_presentation(fixture->presentation, fixture->store, nullptr), "return to grid mode after resize");
  wheel.button = BOXRIG_MOUSE_BUTTON_WHEEL_DOWN;
  expect(route_terminal_presentation_mouse(fixture->presentation, wheel) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION,
      "queue grid zoom after an external resize");
  do { co_await fixture->changed.wait(); }
  while (!terminal_runtime_is_settled(runtime) || snapshot().font == resized.font);
  const auto final = snapshot();
  expect(final.rows == resized.rows && final.columns == resized.columns,
      "the next grid zoom must keep the externally resized effective grid");
}

static cardio::promise<void> run(Fixture *fixture, std::exception_ptr *failure) {
  try { co_await scenarios(fixture); }
  catch (...) { *failure = std::current_exception(); }
  co_await terminal_runtime_disconnect(fixture->window->runtime);
  gtk_main_quit();
}

extern "C" int __wrap_main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto window = load_main_window(TerminalAmbiguousWidth::narrow);
  if (!window) return 1;
  Fixture fixture{&*window, nullptr, create_default_settings(default_terminal_display_settings(12), "Zoom"), {}};
  fixture.presentation = create_terminal_presentation(&*window, {}, [&fixture](TerminalDisplaySettings display) {
    set_setting_value(&fixture.store, terminal_width_setting_key(), gint64{display.width});
    set_setting_value(&fixture.store, terminal_height_setting_key(), gint64{display.height});
    set_setting_value(&fixture.store, terminal_font_size_dip_setting_key(), gdouble{display.font_size_dip});
  }, {});
  terminal_runtime_set_host_callbacks(window->runtime, {
      [&fixture](BoxrigGtk3TerminalWidget *) {
        attach_main_window_terminal(fixture.window);
        attach_terminal_presentation(fixture.presentation);
      }, {}, {}});
  terminal_runtime_set_changed_callback(window->runtime, [](void *context) {
    auto *current = static_cast<Fixture *>(context);
    refresh_terminal_presentation(current->presentation);
    current->changed.trigger();
  }, &fixture);
  expect(apply_terminal_presentation(fixture.presentation, fixture.store, nullptr), "apply initial settings");
  gtk_widget_show(window->window);
  std::exception_ptr failure;
  auto completion = run(&fixture, &failure);
  gtk_main();
  terminal_runtime_set_changed_callback(window->runtime, nullptr, nullptr);
  terminal_runtime_set_host_callbacks(window->runtime, {});
  destroy_terminal_presentation(fixture.presentation);
  destroy_terminal_runtime(window->runtime);
  window->runtime = nullptr;
  window->terminal = nullptr;
  gtk_widget_destroy(window->window);
  release_main_window(&*window);
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { g_printerr("%s\n", error.what()); }
    return 1;
  }
  std::puts("PASS queued zoom modes, direct edits and external resizes preserve their own sizing policies");
  return 0;
}

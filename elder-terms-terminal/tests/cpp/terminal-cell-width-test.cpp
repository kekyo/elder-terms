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
#include <vector>

using namespace elder_terms;

static bool reject_frame = false;
static bool rejected_frame = false;

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
  SettingsStore settings;
  cardio::primitives::conditional changed;
  size_t width;
  int attachments = 0;
};

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static boxrig_terminal *core(Fixture *fixture) {
  return boxrig_ui_model_terminal(boxrig_ui_view_model(
      boxrig_gtk3_terminal_widget_view(terminal_runtime_widget(fixture->window->runtime))));
}

static void check_text(Fixture *fixture) {
  auto *terminal = core(fixture);
  // Inspect both offscreen history and the current viewport without scrolling.
  for (const bool history : {true, false}) {
    boxrig_cell_view digit{}, following{};
    expect((history
        ? boxrig_terminal_get_text_fragment_cell(terminal, BOXRIG_TEXT_SCREEN_PRIMARY, 0, 1, &digit)
        : boxrig_terminal_get_cell(terminal, 0, 1, &digit)) == BOXRIG_STATUS_OK,
        "read the circled digit");
    expect(digit.column_width == fixture->width && !digit.continuation &&
        std::string_view(digit.text, digit.text_length) == "①", "retain complete digit and its original width");
    expect((history
        ? boxrig_terminal_get_text_fragment_cell(terminal, BOXRIG_TEXT_SCREEN_PRIMARY, 0, 1 + fixture->width, &following)
        : boxrig_terminal_get_cell(terminal, 0, 1 + fixture->width, &following)) == BOXRIG_STATUS_OK &&
        std::string_view(following.text, following.text_length) == "B", "following text keeps its assigned column");
  }
}

static cardio::promise<void> scenarios(Fixture *fixture) {
  auto *runtime = fixture->window->runtime;
  expect(apply_terminal_presentation(fixture->presentation, fixture->settings, nullptr), "apply initial settings");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  if (fixture->width == 3) {
    const struct { const char *text; unsigned width; } samples[] = {
        {"④", 1}, {"②", 2}, {"③", 2}, {"①", 3}, {"⑤", 5}};
    for (const auto &sample : samples) {
      boxrig_text_cluster_measurement measured{};
      expect(boxrig_terminal_measure_text_cluster(core(fixture),
          reinterpret_cast<const uint8_t *>(sample.text), 3, &measured) == BOXRIG_STATUS_OK &&
          measured.column_width == sample.width, "round actual fallback font advances up to columns");
    }
  }
  std::string history = "\033[3J\033[2J\033[H";
  for (int row = 0; row != 100; ++row) history += "A①B" + std::string(row == 99 ? "" : "\r\n");
  expect(terminal_runtime_feed(runtime, history), "feed visible and offscreen ambiguous text");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  auto *terminal = core(fixture);
  const auto fragments = boxrig_terminal_text_fragment_count(terminal, BOXRIG_TEXT_SCREEN_PRIMARY);
  expect(fragments == 100, "fixture includes retained history");
  const auto revision = boxrig_terminal_text_revision(terminal);
  check_text(fixture);

  set_setting_value(&fixture->settings, terminal_ambiguous_width_setting_key(),
      std::string(fixture->width == 1 ? "wide" : "narrow"));
  expect(apply_terminal_presentation(fixture->presentation, fixture->settings, nullptr), "apply changed width preference");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  expect(core(fixture) == terminal && boxrig_terminal_text_revision(terminal) == revision,
      "changing the preference preserves existing terminal text and history");
  check_text(fixture);

  for (const bool replace_widget : {false, true}) {
    auto *previous = terminal_runtime_widget(runtime);
    const auto attachments = fixture->attachments;
    // Keep identity comparable even after GTK destroys the replaced widget.
    g_object_ref(previous);
    if (replace_widget) {
      reject_frame = true;
      boxrig_ui_view_invalidate_colors(boxrig_gtk3_terminal_widget_view(previous));
      while (!rejected_frame) co_await fixture->changed.wait();
    }
    expect(terminal_runtime_connect_local(runtime, {{"/bin/true"}}), "reconnect the terminal");
    expect(fixture->attachments == attachments + (replace_widget ? 1 : 0) &&
        (terminal_runtime_widget(runtime) != previous) == replace_widget, "replace only the failed native widget");
    g_object_unref(previous);
    while (terminal_runtime_connection_state(runtime).phase != TerminalConnectionPhase::closed ||
        !terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    expect(core(fixture) == terminal &&
        boxrig_terminal_text_fragment_count(terminal, BOXRIG_TEXT_SCREEN_PRIMARY) == fragments,
        "reconnection and widget replacement preserve the terminal and history");
    check_text(fixture);
    expect(terminal_runtime_feed(runtime, "\033[H\033[2KA①B"), "write fresh text after reconnection");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    check_text(fixture);
  }
  if (boxrig_terminal_ambiguous_width(terminal) == BOXRIG_AMBIGUOUS_WIDTH_FONT) {
    for (const auto size : {30.0, 12.0}) {
      set_setting_value(&fixture->settings, terminal_font_size_dip_setting_key(), size);
      expect(apply_terminal_presentation(fixture->presentation, fixture->settings, nullptr), "apply changed font size");
      while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
      expect(core(fixture) == terminal && boxrig_terminal_text_fragment_count(terminal,
          BOXRIG_TEXT_SCREEN_PRIMARY) == fragments, "font size change preserves terminal and history");
      check_text(fixture);
    }
    set_setting_value(&fixture->settings, terminal_font_families_setting_key(),
        std::vector<std::string>{"Boxrig Width Narrow"});
    expect(apply_terminal_presentation(fixture->presentation, fixture->settings, nullptr), "apply a font with narrower advances");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    fixture->width = 1;
    expect(core(fixture) == terminal && boxrig_terminal_text_fragment_count(terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY) == fragments, "font change preserves terminal and history");
    check_text(fixture);
  }
}

static cardio::promise<void> run(Fixture *fixture, std::exception_ptr *failure) {
  try { co_await scenarios(fixture); }
  catch (...) { *failure = std::current_exception(); }
  co_await terminal_runtime_disconnect(fixture->window->runtime);
  gtk_main_quit();
}

// Exercise the application's window creation and presentation with real GTK.
extern "C" int __wrap_main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto settings = create_default_settings(default_terminal_display_settings(16), "Cell width");
  const std::string width = argc > 1 ? argv[1] : "default";
  if (width != "default") set_setting_value(&settings, terminal_ambiguous_width_setting_key(), width);
  if (width == "font" || width == "default") {
    set_setting_value(&settings, terminal_font_families_setting_key(),
        std::vector<std::string>{"Boxrig Width ASCII", "Boxrig Width Fixture"});
    set_setting_value(&settings, terminal_font_size_dip_setting_key(), 20.0);
  }
  auto window = load_main_window(terminal_ambiguous_width(settings));
  if (!window) return 1;
  Fixture fixture{&*window, create_terminal_presentation(&*window, {}, {}, {}), settings, {},
      width == "font" || width == "default" ? 3u : width == "wide" ? 2u : 1u};
  terminal_runtime_set_host_callbacks(window->runtime, {
      [&fixture](BoxrigGtk3TerminalWidget *) {
        ++fixture.attachments;
        attach_main_window_terminal(fixture.window);
        attach_terminal_presentation(fixture.presentation);
      }, {}, {}});
  terminal_runtime_set_changed_callback(window->runtime, [](void *context) {
    static_cast<Fixture *>(context)->changed.trigger();
  }, &fixture);
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
  std::puts("PASS ambiguous width and history through setting changes, reconnect and native replacement");
  return 0;
}

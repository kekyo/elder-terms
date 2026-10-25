// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-presentation.h"
#include "../../src/terminal-runtime.h"

#include <cassert>
#include <cstdio>
#include <exception>
#include <stdexcept>

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
  cardio::primitives::conditional changed;
  int attachments = 0;
  cardio::primitives::conditional painted;
  guint paint_source = 0;
};

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static void check_appearance(Fixture *fixture, boxrig_ui_cursor_shape_policy shape) {
  auto *widget = terminal_runtime_widget(fixture->window->runtime);
  auto *view = boxrig_gtk3_terminal_widget_view(widget);
  expect(fixture->window->terminal == GTK_WIDGET(widget), "host routes input to the replacement widget");
  boxrig_ui_cursor_style cursor{};
  expect(boxrig_ui_view_get_cursor_style(view, &cursor) == BOXRIG_UI_OK && cursor.shape == shape &&
      cursor.presentation == BOXRIG_UI_CURSOR_PRESENTATION_FOCUSED && cursor.box_width_dip == 4 &&
      cursor.line_width_dip == 1 && cursor.blink == BOXRIG_UI_CURSOR_BLINK_STEADY,
      "shape, independent thickness, focus presentation and blinking survive reattachment");
  const auto image = boxrig_ui_view_get_background_image(view);
  expect(image.width == 2 && image.height == 1 && image.fit == BOXRIG_UI_BACKGROUND_CONTAIN &&
      image.pixels[0] == 0x40400000 && image.pixels[1] == 0x80008000,
      "cached image pixels, alpha and placement survive without reopening the source");
  const auto tint = boxrig_ui_view_get_background_color(view);
  expect(tint.red == 32 && tint.green == 64 && tint.blue == 96 && tint.alpha == 128,
      "tint remains independent of image and color correction");
  boxrig_ui_transparency_state transparency{};
  expect(boxrig_ui_view_get_transparency(view, &transparency) == BOXRIG_UI_OK &&
      transparency.config.request == BOXRIG_UI_TRANSPARENCY_PREFERRED && transparency.config.background_opacity == 102,
      "requested background mode and opacity survive reattachment");
  expect(terminal_presentation_background_status(fixture->presentation).find("desktop transparency is unavailable") != std::string::npos,
      "uncomposited display reports its actual opaque fallback");
  auto *terminal = boxrig_ui_model_terminal(boxrig_ui_view_model(view));
  boxrig_cell_view cell{};
  expect(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK, "retained cell is readable");
  boxrig_ui_color_filter_input input{};
  boxrig_ui_color_filter_result colors{};
  expect(boxrig_ui_color_resolve_cell(terminal, &cell, nullptr, 128, &colors, &input) == BOXRIG_UI_OK,
      "resolve the original retained cell colors");
  expect(colors.foreground.red == 0 && colors.foreground.green == 0 && colors.foreground.blue == 255,
      "core retains the original blue while image and tint are enabled");
  for (const bool focused : {true, false}) {
    expect(boxrig_ui_view_set_focused(view, focused) == BOXRIG_UI_OK, "set retained view focus");
    expect(boxrig_ui_view_resolve_colors(view, &input, &colors) == BOXRIG_UI_OK &&
        colors.foreground.red == 76 && colors.foreground.green == 130 && colors.foreground.blue == 255 &&
        colors.background.red == 32 && colors.background.green == 32 && colors.background.blue == 32,
        "reattached filter composes with connection defaults and uses the original blue");
  }
  boxrig_font_effective_metrics metrics{};
  expect(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) == BOXRIG_FONT_CATALOG_OK &&
      metrics.requested_size_dip == 16, "font preference survives replacement and display scaling");
}

static cardio::promise<void> check_selection_pixels(Fixture *fixture) {
  auto *runtime = fixture->window->runtime;
  auto *widget = terminal_runtime_widget(runtime);
  auto *view = boxrig_gtk3_terminal_widget_view(widget);
  set_main_window_connection_phase(fixture->window, TerminalSessionConnectionPhase::connected);
  expect(terminal_runtime_feed(runtime, "\033[?25l\033[2;1H\033[0m██  ██"), "feed selection sample");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  boxrig_font_effective_metrics metrics{};
  expect(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) == BOXRIG_FONT_CATALOG_OK,
      "read native selection metrics");
  for (const auto shape : {BOXRIG_UI_SELECTION_LINEAR, BOXRIG_UI_SELECTION_RECTANGULAR}) {
    boxrig_ui_selection_options options{};
    options.kind = BOXRIG_UI_SELECTION_CHARACTER;
    options.shape = shape;
    boxrig_ui_hit_test_result hit{};
    expect(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE,
        (metrics.cell_height * 3 / 2) * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK, "hit selection start");
    expect(boxrig_ui_view_begin_selection_with_options(view, &options, &hit) == BOXRIG_UI_OK, "begin selection");
    expect(boxrig_ui_view_hit_test(view, (metrics.cell_width * 6 - 1) * BOXRIG_UI_FIXED_ONE,
        (metrics.cell_height * 3 / 2) * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK, "hit selection end");
    expect(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_OK, "extend selection");
    for (const bool focused : {true, false}) {
      expect(boxrig_ui_view_set_focused(view, focused) == BOXRIG_UI_OK, "change selection focus");
      terminal_runtime_refresh(runtime);
      while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
      // The prepared component image can precede GTK's actual window paint.
      gtk_widget_queue_draw(GTK_WIDGET(widget));
      co_await fixture->painted.wait();
      gint x = 0, y = 0;
      expect(gtk_widget_translate_coordinates(GTK_WIDGET(widget), fixture->window->window, 0, 0, &x, &y),
          "locate native selection pixels");
      auto *pixels = gdk_pixbuf_get_from_window(gtk_widget_get_window(fixture->window->window), x, y,
          gtk_widget_get_allocated_width(GTK_WIDGET(widget)), gtk_widget_get_allocated_height(GTK_WIDGET(widget)));
      expect(pixels != nullptr, "capture restored selection");
      const auto *data = gdk_pixbuf_read_pixels(pixels);
      const auto stride = gdk_pixbuf_get_rowstride(pixels);
      const auto channels = gdk_pixbuf_get_n_channels(pixels);
      const auto *ink = data + metrics.cell_height * 3 / 2 * stride + metrics.cell_width / 2 * channels;
      const auto *fill = data + metrics.cell_height * 3 / 2 * stride + metrics.cell_width * 5 / 2 * channels;
      const bool black = ink[0] == 0 && ink[1] == 0 && ink[2] == 0;
      const bool gray = fill[0] == 179 && fill[1] == 179 && fill[2] == 179;
      if (!black || !gray) std::fprintf(stderr, "selection shape=%d focus=%d ink=%02x%02x%02x fill=%02x%02x%02x\n",
          shape, focused, ink[0], ink[1], ink[2], fill[0], fill[1], fill[2]);
      g_object_unref(pixels);
      expect(black && gray, "opaque gray and black selection survives reattachment, remap and focus with tint/image settings");
    }
  }
  expect(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK, "clear selection sample");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
}

static cardio::promise<void> scenarios(Fixture *fixture) {
  auto *runtime = fixture->window->runtime;
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  expect(terminal_runtime_feed(runtime, "\033[2J\033[H\033[38;2;0;0;255mRETAINED BLUE"), "feed original text");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  auto *core = boxrig_ui_model_terminal(boxrig_ui_view_model(boxrig_gtk3_terminal_widget_view(terminal_runtime_widget(runtime))));
  auto store = create_default_settings(default_terminal_display_settings(16), "Appearance");
  set_setting_value(&store, general_background_setting_key(), std::string("#202020"));
  set_setting_value(&store, terminal_cursor_blink_setting_key(), false);
  set_setting_value(&store, terminal_background_mode_setting_key(), std::string("blur"));
  set_setting_value(&store, terminal_background_opacity_setting_key(), gint64{40});
  set_setting_value(&store, terminal_background_tint_color_setting_key(), std::string("#204060"));
  set_setting_value(&store, terminal_background_tint_opacity_setting_key(), gint64{50});
  set_setting_value(&store, terminal_background_image_setting_key(), std::string("/missing-cached-image.png"));
  set_setting_value(&store, terminal_background_image_opacity_setting_key(), gint64{50});
  set_setting_value(&store, terminal_background_image_fit_setting_key(), std::string("contain"));
  set_setting_value(&store, terminal_color_filter_setting_key(), std::string("oklch"));
  const auto image = std::make_shared<TerminalBackgroundImage>(TerminalBackgroundImage{
      "/missing-cached-image.png", 2, 1, {255, 0, 0, 128, 0, 255, 0, 255}});
  const std::pair<const char *, boxrig_ui_cursor_shape_policy> shapes[] = {
      {"bar", BOXRIG_UI_CURSOR_SHAPE_BAR}, {"underline", BOXRIG_UI_CURSOR_SHAPE_UNDERLINE}, {"block", BOXRIG_UI_CURSOR_SHAPE_BLOCK}};
  for (const auto &[name, shape] : shapes) {
    set_setting_value(&store, terminal_cursor_shape_setting_key(), std::string(name));
    expect(apply_terminal_presentation(fixture->presentation, store, image), "apply combined appearance");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    check_appearance(fixture, shape);
    co_await check_selection_pixels(fixture);
    const auto attachments = fixture->attachments;
    auto *previous = terminal_runtime_widget(runtime);
    // Retain identity through replacement; never dereference the destroyed GTK widget.
    g_object_ref(previous);
    reject_frame = true;
    rejected_frame = false;
    boxrig_ui_view_invalidate_colors(boxrig_gtk3_terminal_widget_view(previous));
    while (!rejected_frame) co_await fixture->changed.wait();
    expect(terminal_runtime_connect_local(runtime, {{"/bin/true"}}), "reconnect after a native frame failure");
    expect(fixture->attachments == attachments + 1 && terminal_runtime_widget(runtime) != previous,
        "recovery creates a new native widget rather than replaying setters on the old one");
    g_object_unref(previous);
    while (terminal_runtime_connection_state(runtime).phase != TerminalConnectionPhase::closed ||
        !terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    expect(boxrig_ui_model_terminal(boxrig_ui_view_model(boxrig_gtk3_terminal_widget_view(terminal_runtime_widget(runtime)))) == core,
        "recovery retains the original terminal core");
    check_appearance(fixture, shape);
    co_await check_selection_pixels(fixture);
    gtk_widget_hide(fixture->window->window);
    gtk_widget_show(fixture->window->window);
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    check_appearance(fixture, shape);
    co_await check_selection_pixels(fixture);
  }
}

static cardio::promise<void> run(Fixture *fixture, std::exception_ptr *failure) {
  try { co_await scenarios(fixture); }
  catch (...) { *failure = std::current_exception(); }
  co_await terminal_runtime_disconnect(fixture->window->runtime);
  gtk_main_quit();
}

// Link the real application objects, replacing only the entry point and one
// native failure boundary. All presentation restoration remains application code.
extern "C" int __wrap_main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto window = load_main_window(TerminalAmbiguousWidth::narrow);
  if (!window) return 1;
  Fixture fixture{&*window, create_terminal_presentation(&*window, {}, {}, {}), {}, 0, {}, 0};
  terminal_runtime_set_host_callbacks(window->runtime, {
      [&fixture](BoxrigGtk3TerminalWidget *) {
        ++fixture.attachments;
        attach_main_window_terminal(fixture.window);
        attach_terminal_presentation(fixture.presentation);
        g_signal_connect_after(terminal_runtime_widget(fixture.window->runtime), "draw",
            G_CALLBACK(+[](GtkWidget *, cairo_t *, gpointer context) -> gboolean {
              auto *current = static_cast<Fixture *>(context);
              if (!current->paint_source) current->paint_source = g_idle_add(+[](gpointer context) -> gboolean {
                auto *current = static_cast<Fixture *>(context);
                current->paint_source = 0;
                current->painted.trigger();
                return G_SOURCE_REMOVE;
              }, current);
              return FALSE;
            }), &fixture);
      }, {}, {}});
  terminal_runtime_set_changed_callback(window->runtime, [](void *context) {
    static_cast<Fixture *>(context)->changed.trigger();
  }, &fixture);
  gtk_widget_show(window->window);
  std::exception_ptr failure;
  auto completion = run(&fixture, &failure);
  gtk_main();
  if (fixture.paint_source) g_source_remove(fixture.paint_source);
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
  std::puts("PASS all cursor shapes, background, tint, cached image and filter through native replacement and remap");
  return 0;
}

// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-runtime.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>

using namespace elder_terms;

struct BackgroundCase {
  const char *name;
  const char *output;
  boxrig_ui_color_source_role role;
  boxrig_rgb color;
};

// Each row starts from SGR 0. The sampled cell is a blank inside the swatch,
// including the row whose previously printed text is erased with ECH.
static constexpr std::array cases{
    BackgroundCase{"default", "        ", BOXRIG_UI_COLOR_DEFAULT_BACKGROUND, {32, 32, 32}},
    BackgroundCase{"SGR 49", "\033[42m\033[49m        ", BOXRIG_UI_COLOR_DEFAULT_BACKGROUND, {32, 32, 32}},
    BackgroundCase{"SGR reset", "\033[42m\033[0m        ", BOXRIG_UI_COLOR_DEFAULT_BACKGROUND, {32, 32, 32}},
    BackgroundCase{"SGR 42", "\033[42mGREEN   ", BOXRIG_UI_COLOR_INDEXED, {0, 255, 0}},
    BackgroundCase{"SGR 102", "\033[102mGREEN   ", BOXRIG_UI_COLOR_INDEXED, {0, 255, 0}},
    BackgroundCase{"256 color", "\033[48;5;46mGREEN   ", BOXRIG_UI_COLOR_INDEXED, {0, 255, 0}},
    BackgroundCase{"true color", "\033[48;2;0;255;0mGREEN   ", BOXRIG_UI_COLOR_RGB, {0, 255, 0}},
    BackgroundCase{"same RGB", "\033[48;2;32;32;32mSAME    ", BOXRIG_UI_COLOR_RGB, {32, 32, 32}},
    BackgroundCase{"explicit blank", "\033[42m        ", BOXRIG_UI_COLOR_INDEXED, {0, 255, 0}},
    BackgroundCase{"erased blank", "\033[42mERASE ME\033[8D\033[8X", BOXRIG_UI_COLOR_INDEXED, {0, 255, 0}},
    BackgroundCase{"inverse default", "\033[7mINVERSE ", BOXRIG_UI_COLOR_DEFAULT_FOREGROUND, {224, 224, 224}},
    BackgroundCase{"inverse green", "\033[32;7mINVERSE ", BOXRIG_UI_COLOR_INDEXED, {0, 255, 0}},
};
static constexpr std::array<boxrig_rgb, 2> backdrops{{{255, 0, 0}, {0, 0, 255}}};
static constexpr std::size_t sample_column = 27;

struct Fixture {
  TerminalRuntime *runtime = nullptr;
  GtkWidget *window = nullptr;
  boxrig_gtk_render_backend backend = BOXRIG_GTK_RENDER_BACKEND_OPENGL;
  std::string backend_name;
  std::string evidence_directory;
  std::array<std::array<boxrig_rgb, cases.size()>, backdrops.size()> samples{};
  guint observation = 0;
  std::uint64_t generation = 0;
  int backdrop = -1;
  bool require_opaque = false;
  bool success = true;
};

static bool same_rgb(boxrig_rgb left, boxrig_rgb right) {
  return left.red == right.red && left.green == right.green && left.blue == right.blue;
}

static boxrig_rgb composite(boxrig_rgb source, boxrig_rgb backdrop, unsigned int alpha) {
  // Independent eight-bit source-over expectation; do not reuse the renderer's
  // pixel helpers when testing the displayed result.
  const auto channel = [alpha](unsigned int front, unsigned int back) {
    return static_cast<uint8_t>((front * alpha + 127) / 255 + (back * (255 - alpha) + 127) / 255);
  };
  return {channel(source.red, backdrop.red), channel(source.green, backdrop.green), channel(source.blue, backdrop.blue)};
}

static void prepare_scene(Fixture *fixture) {
  auto *widget = terminal_runtime_widget(fixture->runtime);
  auto *view = boxrig_gtk3_terminal_widget_view(widget);
  if (fixture->backdrop == 0) {
    auto transparency = boxrig_ui_transparency_config_default();
    transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    transparency.background_opacity = 128;
    assert(boxrig_gtk3_terminal_widget_set_transparency(widget, &transparency) == BOXRIG_UI_OK);
    // Pin the palette and dynamic defaults so host themes cannot affect pixels.
    assert(terminal_runtime_feed(fixture->runtime,
        "\033[?25l\033[0m\033[2J\033[H\033]10;#e0e0e0\a\033]11;#202020\a"
        "\033]4;2;#00ff00\a\033]4;10;#00ff00\a\033]4;46;#00ff00\a"));
    for (std::size_t row = 0; row < cases.size(); ++row) {
      const auto output = "\033[" + std::to_string(row + 1) + ";1H\033[0m" + cases[row].name +
          "\033[21G" + cases[row].output + "\033[0m";
      assert(terminal_runtime_feed(fixture->runtime, output));
    }
  }
  // Only replace the lower layer. The second presentation must reuse the same
  // logical cells, making a backdrop-dependent color directly observable.
  const auto color = backdrops[fixture->backdrop];
  assert(boxrig_ui_view_set_background_color(view, {color.red, color.green, color.blue, 255}) == BOXRIG_UI_OK);
  terminal_runtime_refresh(fixture->runtime);
}

static void check_scene(Fixture *fixture) {
  auto *widget = terminal_runtime_widget(fixture->runtime);
  auto *view = boxrig_gtk3_terminal_widget_view(widget);
  const auto *terminal = boxrig_ui_model_terminal(boxrig_ui_view_model(view));
  boxrig_ui_transparency_state transparency{};
  assert(boxrig_gtk3_terminal_widget_get_transparency(widget, &transparency) == BOXRIG_UI_OK);
  assert(transparency.effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA);
  assert(transparency.effective_background_opacity == 128);
  boxrig_font_effective_metrics metrics{};
  assert(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) == BOXRIG_FONT_CATALOG_OK);
  gint x = 0, y = 0;
  assert(gtk_widget_translate_coordinates(GTK_WIDGET(widget), fixture->window, 0, 0, &x, &y));
  auto *pixels = gdk_pixbuf_get_from_window(gtk_widget_get_window(fixture->window), x, y,
      gtk_widget_get_allocated_width(GTK_WIDGET(widget)), gtk_widget_get_allocated_height(GTK_WIDGET(widget)));
  assert(pixels);
  const auto *data = gdk_pixbuf_read_pixels(pixels);
  const auto stride = gdk_pixbuf_get_rowstride(pixels);
  const auto channels = gdk_pixbuf_get_n_channels(pixels);
  for (std::size_t row = 0; row < cases.size(); ++row) {
    const auto &item = cases[row];
    boxrig_cell_view cell{};
    assert(boxrig_terminal_get_cell(terminal, row, sample_column, &cell) == BOXRIG_STATUS_OK);
    assert(cell.text_length == 0 || (cell.text_length == 1 && cell.text[0] == ' '));
    boxrig_ui_color_filter_result colors{};
    boxrig_ui_color_filter_input input{};
    assert(boxrig_ui_color_resolve_cell(terminal, &cell, nullptr, 128, &colors, &input) == BOXRIG_UI_OK);
    assert(input.background_source.role == item.role);
    assert(same_rgb(colors.background, item.color));
    const auto px = (sample_column * 2 + 1) * metrics.cell_width / 2;
    const auto py = (row * 2 + 1) * metrics.cell_height / 2;
    assert(px < static_cast<std::size_t>(gdk_pixbuf_get_width(pixels)));
    assert(py < static_cast<std::size_t>(gdk_pixbuf_get_height(pixels)));
    const auto *pixel = data + py * stride + px * channels;
    const boxrig_rgb actual{pixel[0], pixel[1], pixel[2]};
    fixture->samples[fixture->backdrop][row] = actual;
    const auto alpha = fixture->require_opaque && item.role != BOXRIG_UI_COLOR_DEFAULT_BACKGROUND ? 255u : 128u;
    const auto expected = composite(item.color, backdrops[fixture->backdrop], alpha);
    const bool passed = same_rgb(actual, expected);
    fixture->success &= passed;
    std::printf("%s backdrop=%s case=%s role=%d actual=#%02x%02x%02x expected=#%02x%02x%02x\n",
        passed ? "PASS" : "FAIL", fixture->backdrop == 0 ? "red" : "blue", item.name,
        static_cast<int>(input.background_source.role), actual.red, actual.green, actual.blue,
        expected.red, expected.green, expected.blue);
  }
  if (!fixture->evidence_directory.empty()) {
    const auto path = fixture->evidence_directory + "/" + fixture->backend_name +
        (fixture->backdrop == 0 ? "-red.png" : "-blue.png");
    assert(gdk_pixbuf_save(pixels, path.c_str(), "png", nullptr, static_cast<const char *>(nullptr)));
  }
  g_object_unref(pixels);
}

static gboolean observe(gpointer context) {
  auto *fixture = static_cast<Fixture *>(context);
  fixture->observation = 0;
  auto *widget = terminal_runtime_widget(fixture->runtime);
  if (!terminal_runtime_is_settled(fixture->runtime) ||
      !boxrig_gtk3_terminal_widget_has_presentation(widget)) return G_SOURCE_REMOVE;
  const auto generation = boxrig_gtk3_terminal_widget_presentation_generation(widget);
  if (generation <= fixture->generation) return G_SOURCE_REMOVE;
  fixture->generation = generation;
  boxrig_gtk_render_state render{};
  assert(boxrig_gtk3_terminal_widget_get_render_state(widget, &render) == BOXRIG_UI_OK);
  assert(render.active_backend == fixture->backend);
  if (fixture->backdrop >= 0) check_scene(fixture);
  if (++fixture->backdrop == static_cast<int>(backdrops.size())) gtk_main_quit();
  else prepare_scene(fixture);
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  if (argc < 3 || argc > 4 ||
      (std::string_view(argv[1]) != "cairo" && std::string_view(argv[1]) != "opengl") ||
      (std::string_view(argv[2]) != "transparent" && std::string_view(argv[2]) != "require-opaque")) {
    std::fprintf(stderr, "Usage: %s cairo|opengl transparent|require-opaque [evidence-directory]\n", argv[0]);
    return 2;
  }
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  Fixture fixture;
  fixture.backend_name = argv[1];
  if (fixture.backend_name == "cairo") fixture.backend = BOXRIG_GTK_RENDER_BACKEND_CAIRO;
  // This selects the acceptance assertion, not a boxrig setting: the pinned
  // public API has no per-origin opacity policy. Keep the unmet requirement RED.
  fixture.require_opaque = std::string_view(argv[2]) == "require-opaque";
  if (argc == 4) fixture.evidence_directory = argv[3];
  fixture.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(fixture.window), content);
  fixture.runtime = create_terminal_runtime(GTK_WINDOW(fixture.window), GTK_BOX(content), GTK_BOX(content), TerminalAmbiguousWidth::narrow);
  assert(fixture.runtime);
  // Observe after GTK draws a completed presentation, without timed sleeps.
  g_signal_connect_after(terminal_runtime_widget(fixture.runtime), "draw",
      G_CALLBACK(+[](GtkWidget *, cairo_t *, gpointer context) {
        auto *fixture = static_cast<Fixture *>(context);
        if (!fixture->observation) fixture->observation = g_idle_add(observe, fixture);
        return FALSE;
      }), &fixture);
  gtk_widget_show_all(fixture.window);
  gtk_main();
  if (fixture.observation) g_source_remove(fixture.observation);
  for (std::size_t row = 0; row < cases.size(); ++row) {
    const bool invariant = same_rgb(fixture.samples[0][row], fixture.samples[1][row]);
    const bool expected = fixture.require_opaque && cases[row].role != BOXRIG_UI_COLOR_DEFAULT_BACKGROUND;
    const bool passed = invariant == expected;
    fixture.success &= passed;
    std::printf("%s backdrop-invariance case=%s actual=%d expected=%d\n",
        passed ? "PASS" : "FAIL", cases[row].name, invariant, expected);
  }
  destroy_terminal_runtime(fixture.runtime);
  gtk_widget_destroy(fixture.window);
  return fixture.success ? 0 : 1;
}

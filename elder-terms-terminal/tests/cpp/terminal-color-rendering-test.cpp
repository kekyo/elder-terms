// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-runtime.h"
#include "../../src/terminal-presentation.h"
#include <elder-terms/terminal-color-filter.h>

#include <cassert>
#include <cstdio>
#include <string_view>
#include <vector>

using namespace elder_terms;

struct Fixture {
  TerminalRuntime *runtime = nullptr;
  GtkWidget *window = nullptr;
  TerminalColorFilterContext filter{RgbColor{32, 32, 32}, TerminalColorFilter::none, TerminalColorVision::protan, 1};
  boxrig_gtk_render_backend backend = BOXRIG_GTK_RENDER_BACKEND_OPENGL;
  std::vector<boxrig_rgb> expected;
  unsigned int targets = 0;
  guint observation = 0;
  std::uint64_t generation = 0;
  int scenario = -1;
  bool identity = false;
};

static boxrig_ui_color_filter_result filter_colors(const boxrig_ui_color_filter_input *input, void *context) {
  auto *fixture = static_cast<Fixture *>(context);
  const auto colors = filter_terminal_colors(input, &fixture->filter);
  fixture->targets |= 1u << input->target;
  const auto phase = fixture->scenario % 4;
  if (phase == 0 && input->target == BOXRIG_UI_COLOR_CELL &&
      (input->foreground_source.role == BOXRIG_UI_COLOR_RGB || input->background_source.role == BOXRIG_UI_COLOR_RGB)) {
    fixture->expected.push_back(colors.foreground);
    fixture->expected.push_back(colors.background);
    fixture->expected.push_back(colors.underline);
  } else if (phase == 1 && input->target == BOXRIG_UI_COLOR_CELL &&
      input->background_source.role == BOXRIG_UI_COLOR_UI) {
    fixture->expected.push_back(colors.foreground);
    fixture->expected.push_back(colors.background);
  } else if ((phase == 2 && input->target == BOXRIG_UI_COLOR_SEARCH_MARKER) ||
      (phase == 3 && input->target == BOXRIG_UI_COLOR_IME_MARKER)) {
    fixture->expected.push_back(colors.marker);
  } else if (phase == 3 && input->target == BOXRIG_UI_COLOR_IME) {
    fixture->expected.push_back(colors.foreground);
    fixture->expected.push_back(colors.background);
  }
  // The identity option is a negative control: the expected corrected pixels
  // must not be found when the renderer receives the original colors instead.
  return fixture->identity ? boxrig_ui_color_filter_result_default(input) : colors;
}

static void check_pixels(Fixture *fixture) {
  const unsigned int required[] = {
    1u << BOXRIG_UI_COLOR_CELL, 1u << BOXRIG_UI_COLOR_CELL,
    1u << BOXRIG_UI_COLOR_SEARCH_MARKER, (1u << BOXRIG_UI_COLOR_IME) | (1u << BOXRIG_UI_COLOR_IME_MARKER)};
  const auto targets = required[fixture->scenario % 4];
  assert((fixture->targets & targets) == targets);
  auto *widget = GTK_WIDGET(terminal_runtime_widget(fixture->runtime));
  gint x = 0, y = 0;
  assert(gtk_widget_translate_coordinates(widget, fixture->window, 0, 0, &x, &y));
  auto *pixels = gdk_pixbuf_get_from_window(gtk_widget_get_window(fixture->window), x, y,
      gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget));
  assert(pixels && !fixture->expected.empty());
  const auto *data = gdk_pixbuf_read_pixels(pixels);
  const auto stride = gdk_pixbuf_get_rowstride(pixels);
  const auto channels = gdk_pixbuf_get_n_channels(pixels);
  for (const auto color : fixture->expected) {
    bool found = false;
    for (int row = 0; row < gdk_pixbuf_get_height(pixels) && !found; row++) {
      for (int column = 0; column < gdk_pixbuf_get_width(pixels); column++) {
        const auto *pixel = data + row * stride + column * channels;
        if (pixel[0] == color.red && pixel[1] == color.green && pixel[2] == color.blue) {
          found = true;
          break;
        }
      }
    }
    if (!found) std::fprintf(stderr, "scenario=%d missing displayed RGB #%02x%02x%02x\n",
        fixture->scenario, color.red, color.green, color.blue);
    assert(found);
  }
  g_object_unref(pixels);
}

static void prepare_scenario(Fixture *fixture) {
  auto *widget = terminal_runtime_widget(fixture->runtime);
  auto *view = boxrig_gtk3_terminal_widget_view(widget);
  const auto phase = fixture->scenario % 4;
  if (fixture->scenario < 24) {
    fixture->filter.color_vision = static_cast<TerminalColorVision>(1 + fixture->scenario / 8);
    fixture->filter.filter = fixture->scenario % 8 >= 4 ? TerminalColorFilter::oklch : TerminalColorFilter::none;
  } else {
    const TerminalColorFilter modes[] = {TerminalColorFilter::reverse_lightness, TerminalColorFilter::reverse_contrast,
        TerminalColorFilter::reverse_roles, TerminalColorFilter::reverse_cam16, TerminalColorFilter::reverse_palette};
    fixture->filter.filter = modes[(fixture->scenario - 24) / 16];
    fixture->filter.color_vision = static_cast<TerminalColorVision>((fixture->scenario - 24) % 16 / 4);
  }
  fixture->expected.clear();
  fixture->targets = 0;
  boxrig_composition_event cancel{};
  cancel.type = BOXRIG_COMPOSITION_CANCEL;
  assert(boxrig_composition_apply_event(boxrig_gtk3_terminal_widget_composition(widget), &cancel) == BOXRIG_COMPOSITION_OK);
  assert(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
  assert(boxrig_ui_view_clear_search(view) == BOXRIG_UI_OK);
  assert(terminal_runtime_feed(fixture->runtime, "\033[?25l\033[0m\033[2J\033[H"
      "\033]10;#c85050\a\033]11;#202020\a"));
  if (phase == 0) {
    // Explicit foreground, background, independent underline, then inverse.
    assert(terminal_runtime_feed(fixture->runtime,
        "\033[38;2;200;80;80;48;2;80;128;80;58;2;20;40;160;4m██  \033[7m██  \033[0m"));
  } else if (phase == 1) {
    assert(terminal_runtime_feed(fixture->runtime, "\033[48;2;80;128;80m██  \033[0m"));
    assert(boxrig_ui_model_synchronize(boxrig_ui_view_model(view)) == BOXRIG_UI_OK);
    assert(apply_terminal_selection_style(view) == BOXRIG_UI_OK);
    boxrig_ui_hit_test_result hit{};
    assert(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    boxrig_ui_selection_options selection{};
    selection.kind = BOXRIG_UI_SELECTION_CHARACTER;
    selection.shape = fixture->scenario % 8 < 4 ? BOXRIG_UI_SELECTION_LINEAR : BOXRIG_UI_SELECTION_RECTANGULAR;
    assert(boxrig_ui_view_begin_selection_with_options(view, &selection, &hit) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_hit_test(view, 40 * BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_OK);
  } else if (phase == 2) {
    assert(terminal_runtime_feed(fixture->runtime, "\033[48;2;80;128;80mFIND\033[0m"));
    assert(boxrig_ui_model_synchronize(boxrig_ui_view_model(view)) == BOXRIG_UI_OK);
    boxrig_text_search_options search{};
    search.pattern = reinterpret_cast<const uint8_t *>("FIND");
    search.pattern_length = 4;
    assert(boxrig_ui_view_begin_search(view, &search) == BOXRIG_UI_OK);
    boxrig_text_search_progress progress{};
    std::size_t used = 0;
    assert(boxrig_ui_view_resume_search(view, 4096, &used, &progress) == BOXRIG_UI_OK);
    assert(progress == BOXRIG_TEXT_SEARCH_MATCH);
  } else {
    boxrig_composition_segment segment{0, 4, BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE};
    boxrig_composition_event event{};
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = "MM  ";
    event.text_length = 4;
    event.segments = &segment;
    event.segment_count = 1;
    event.selection_length = 1;
    event.caret_offset = 3;
    event.caret_visible = true;
    assert(boxrig_composition_apply_event(boxrig_gtk3_terminal_widget_composition(widget), &event) == BOXRIG_COMPOSITION_OK);
  }
  assert(boxrig_ui_view_set_color_filter(view, filter_colors, fixture) == BOXRIG_UI_OK);
  terminal_runtime_refresh(fixture->runtime);
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
  if (fixture->scenario >= 0) check_pixels(fixture);
  if (++fixture->scenario == 104) gtk_main_quit();
  else prepare_scenario(fixture);
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  Fixture fixture;
  if (argc > 1 && std::string_view(argv[1]) == "cairo") fixture.backend = BOXRIG_GTK_RENDER_BACKEND_CAIRO;
  fixture.identity = argc > 2 && std::string_view(argv[2]) == "--identity";
  fixture.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(fixture.window), content);
  fixture.runtime = create_terminal_runtime(GTK_WINDOW(fixture.window), GTK_BOX(content), GTK_BOX(content), elder_terms::TerminalAmbiguousWidth::narrow);
  assert(fixture.runtime);
  terminal_runtime_set_prepare_frame_callback(fixture.runtime, [&fixture] {
    auto *view = boxrig_gtk3_terminal_widget_view(terminal_runtime_widget(fixture.runtime));
    const auto previous = fixture.filter.reverse_palette;
    assert(prepare_terminal_color_filter(&fixture.filter, boxrig_ui_model_terminal(boxrig_ui_view_model(view))));
    if (previous != fixture.filter.reverse_palette) assert(boxrig_ui_view_invalidate_colors(view) == BOXRIG_UI_OK);
  });
  g_signal_connect_after(terminal_runtime_widget(fixture.runtime), "draw",
      G_CALLBACK(+[](GtkWidget *, cairo_t *, gpointer context) -> gboolean {
        auto *fixture = static_cast<Fixture *>(context);
        if (!fixture->observation) fixture->observation = g_idle_add(observe, fixture);
        return FALSE;
      }), &fixture);
  gtk_widget_show_all(fixture.window);
  gtk_main();
  if (fixture.observation) g_source_remove(fixture.observation);
  destroy_terminal_runtime(fixture.runtime);
  gtk_widget_destroy(fixture.window);
  std::puts("PASS Oklch and five reverse filters with P/D/T: background, inverse, underline, selection, search and IME pixels");
}

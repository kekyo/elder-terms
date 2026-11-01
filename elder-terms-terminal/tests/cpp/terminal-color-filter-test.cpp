// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/terminal-color-filter.h>
#include <boxrig/ui.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstring>
#include <string>

using namespace elder_terms;

static bool equal(RgbColor a, RgbColor b) { return a.red == b.red && a.green == b.green && a.blue == b.blue; }
static double luminance(RgbColor color) {
  const auto linear = [](double x) { x /= 255; return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4); };
  return .2126 * linear(color.red) + .7152 * linear(color.green) + .0722 * linear(color.blue);
}
static double contrast(RgbColor a, RgbColor b) {
  const auto x = luminance(a) + .05, y = luminance(b) + .05;
  return std::max(x, y) / std::min(x, y);
}
static RgbColor rgb(boxrig_rgb color) { return {color.red, color.green, color.blue}; }

static void test_color_vision_hook() {
  const RgbColor connection{70, 20, 80};
  for (const auto profile : {TerminalColorVision::none, TerminalColorVision::protan, TerminalColorVision::deutan, TerminalColorVision::tritan}) {
    for (const double strength : {0.0, .5, 1.0}) for (const bool oklch : {false, true}) {
      TerminalColorFilterContext context{connection, oklch ? TerminalColorFilter::oklch : TerminalColorFilter::none, profile, strength};
      const auto recolor = [&](boxrig_rgb value) { return correct_color_vision(rgb(value), profile, strength); };
      for (const auto target : {BOXRIG_UI_COLOR_CELL, BOXRIG_UI_COLOR_IME}) {
        boxrig_ui_color_filter_input input{};
        input.target = target;
        input.colors = {{190, 60, 30}, {20, 90, 60}, {30, 80, 160}, {}, false};
        input.foreground_source.role = input.background_source.role = input.underline_source.role = BOXRIG_UI_COLOR_RGB;
        const auto background = recolor(input.colors.background);
        const auto foreground = recolor(input.colors.foreground);
        const auto underline = recolor(input.colors.underline);
        auto result = filter_terminal_colors(&input, &context);
        assert(equal(rgb(result.background), background));
        assert(equal(rgb(result.foreground), oklch ? improve_color_visibility(foreground, background) : foreground));
        assert(equal(rgb(result.underline), oklch ? improve_color_visibility(underline, background) : underline));
        if (oklch) assert(contrast(rgb(result.foreground), rgb(result.background)) >= 4.5);
        input.background_source.role = BOXRIG_UI_COLOR_DEFAULT_BACKGROUND;
        result = filter_terminal_colors(&input, &context);
        assert(equal(rgb(result.background), connection));
        input.colors.underline_follows_foreground = true;
        assert(filter_terminal_colors(&input, &context).underline_follows_foreground);
        // The default background remains a connection identifier when reversed.
        input.attributes = BOXRIG_CELL_INVERSE;
        std::swap(input.colors.foreground, input.colors.background);
        std::swap(input.foreground_source, input.background_source);
        result = filter_terminal_colors(&input, &context);
        const auto reverse_background = recolor(input.colors.background);
        assert(equal(rgb(result.background), reverse_background));
        assert(equal(rgb(result.foreground), oklch ? improve_color_visibility(connection, reverse_background) : connection));
      }
      for (const auto target : {BOXRIG_UI_COLOR_CURSOR, BOXRIG_UI_COLOR_SELECTION_MARKER, BOXRIG_UI_COLOR_SEARCH_MARKER,
          BOXRIG_UI_COLOR_HOVER_MARKER, BOXRIG_UI_COLOR_IME_MARKER}) {
        boxrig_ui_color_filter_input input{};
        input.target = target;
        input.colors.marker = {200, 60, 40};
        input.marker_source.role = BOXRIG_UI_COLOR_RGB;
        input.underlay_valid = true;
        input.underlay.background = {50, 75, 120};
        const auto marker = recolor(input.colors.marker);
        const auto result = filter_terminal_colors(&input, &context);
        assert(equal(rgb(result.marker), oklch ? improve_color_visibility(marker, rgb(input.underlay.background)) : marker));
        assert(equal(rgb(input.underlay.background), {50, 75, 120}));
      }
      boxrig_ui_color_filter_input surface{};
      surface.target = BOXRIG_UI_COLOR_SURFACE;
      surface.background_source.role = BOXRIG_UI_COLOR_DEFAULT_BACKGROUND;
      assert(equal(rgb(filter_terminal_colors(&surface, &context).background), connection));
    }
  }
}

static void test_link_dimming() {
  for (const auto filter : {TerminalColorFilter::none, TerminalColorFilter::oklch,
      TerminalColorFilter::reverse_lightness, TerminalColorFilter::reverse_contrast,
      TerminalColorFilter::reverse_roles, TerminalColorFilter::reverse_cam16, TerminalColorFilter::reverse_palette}) {
    for (const auto background : {boxrig_rgb{10, 10, 10}, boxrig_rgb{235, 235, 235}}) {
      TerminalColorFilterContext context{};
      context.filter = filter;
      context.dim_links = true;
      boxrig_ui_color_filter_input input{};
      input.target = BOXRIG_UI_COLOR_CELL;
      input.attributes = BOXRIG_CELL_INVERSE | BOXRIG_CELL_BOLD;
      input.colors = {{170, 100, 110}, background, {75, 140, 170}, {}, false};
      input.foreground_source.role = input.background_source.role = input.underline_source.role = BOXRIG_UI_COLOR_RGB;
      const auto original = filter_terminal_colors(&input, &context);
      input.annotation_color_tag = terminal_dim_link_color_tag;
      const auto dim = filter_terminal_colors(&input, &context);
      assert(equal(rgb(dim.background), rgb(original.background)));
      const auto bg = srgb_to_oklch(rgb(original.background));
      for (const auto &pair : {std::pair{original.foreground, dim.foreground}, std::pair{original.underline, dim.underline}}) {
        const auto before = srgb_to_oklch(rgb(pair.first));
        const auto after = srgb_to_oklch(rgb(pair.second));
        assert(std::abs(after.lightness - bg.lightness) < std::abs(before.lightness - bg.lightness) * .65);
        const auto hue_difference = std::abs(std::remainder(after.hue - before.hue, 360));
        if (after.chroma > .02) assert(hue_difference < 4);
      }
      assert(!dim.underline_follows_foreground && input.attributes == (BOXRIG_CELL_INVERSE | BOXRIG_CELL_BOLD));
      context.dim_links = false;
      assert(equal(rgb(filter_terminal_colors(&input, &context).foreground), rgb(original.foreground)));
    }
  }
}

int main() {
  test_link_dimming();
  test_color_vision_hook();
  // Reference sRGB primaries from the D65 Oklab matrices, independent of the filter.
  const auto red = srgb_to_oklch({255, 0, 0});
  const auto green = srgb_to_oklch({0, 255, 0});
  const auto blue = srgb_to_oklch({0, 0, 255});
  assert(std::abs(red.lightness - .62795536) < 1e-6 && std::abs(red.hue - 29.233885) < .001);
  assert(std::abs(green.lightness - .86643961) < 1e-6 && std::abs(green.hue - 142.495339) < .001);
  assert(std::abs(blue.lightness - .45201372) < 1e-6 && std::abs(blue.chroma - .31321437) < 1e-6);
  assert(std::abs(srgb_to_oklch({255, 255, 255}).lightness - 1) < 1e-6);
  assert(srgb_to_oklch({0, 0, 0}).lightness == 0);
  for (int r = 0; r <= 255; r += 17) for (int g = 0; g <= 255; g += 17) for (int b = 0; b <= 255; b += 17) {
    const RgbColor color{static_cast<guint8>(r), static_cast<guint8>(g), static_cast<guint8>(b)};
    const auto perceptual = srgb_to_oklch(color);
    assert(std::isfinite(perceptual.lightness) && std::isfinite(perceptual.chroma) && std::isfinite(perceptual.hue));
    assert(equal(color, oklch_to_srgb(perceptual)));
  }
  assert(equal(oklch_to_srgb({std::numeric_limits<double>::quiet_NaN(), 1, 270}), {0, 0, 0}));
  for (const RgbColor background : {RgbColor{0, 0, 0}, RgbColor{32, 32, 32}, RgbColor{117, 117, 117},
      RgbColor{255, 255, 255}, RgbColor{64, 16, 80}, RgbColor{190, 210, 170}}) {
    for (const RgbColor color : {RgbColor{0, 0, 128}, RgbColor{0, 0, 255}, RgbColor{80, 128, 255}, RgbColor{128, 0, 255},
        RgbColor{255, 0, 0}, RgbColor{128, 0, 0}, RgbColor{0, 128, 0}, RgbColor{0, 255, 255},
        RgbColor{128, 128, 0}, RgbColor{128, 0, 128}, RgbColor{80, 80, 80}, RgbColor{128, 128, 128},
        RgbColor{0, 0, 0}, RgbColor{255, 255, 255}}) {
      const auto adjusted = improve_color_visibility(color, background);
      const auto before = srgb_to_oklch(color), after = srgb_to_oklch(adjusted);
      const auto delta = std::abs(before.hue - after.hue);
      if (before.chroma > .02 && after.chroma > .02) assert(std::min(delta, 360 - delta) < 3);
      assert(after.chroma <= before.chroma + .003);
      if (color.red == color.green && color.green == color.blue)
        assert(adjusted.red == adjusted.green && adjusted.green == adjusted.blue);
      assert(contrast(adjusted, background) >= contrast(color, background) - 1e-8);
      assert(contrast(adjusted, background) >= 4.5);
      if (contrast({0, 0, 0}, background) >= 4.5 && contrast({255, 255, 255}, background) >= 4.5)
        assert(std::abs(after.lightness - before.lightness) <= std::min(before.lightness, 1 - before.lightness) + 1e-6);
      if (contrast(color, background) >= 4.5) assert(equal(adjusted, color));
      assert(equal(adjusted, improve_color_visibility(adjusted, background)));
      for (int frame = 0; frame < 100; frame++) assert(equal(adjusted, improve_color_visibility(color, background)));
      std::printf("bg=%02x%02x%02x #%02x%02x%02x -> #%02x%02x%02x contrast=%.3f->%.3f hue_delta=%.3f\n",
          background.red, background.green, background.blue, color.red, color.green, color.blue,
          adjusted.red, adjusted.green, adjusted.blue, contrast(color, background), contrast(adjusted, background), delta);
    }
    for (int r = 0; r <= 255; r += 17) for (int g = 0; g <= 255; g += 17) for (int b = 0; b <= 255; b += 17) {
      const RgbColor color{static_cast<guint8>(r), static_cast<guint8>(g), static_cast<guint8>(b)};
      const auto adjusted = improve_color_visibility(color, background);
      assert(contrast(adjusted, background) >= 4.5);
      if (contrast(color, background) >= 4.5) assert(equal(adjusted, color));
    }
  }
  TerminalColorFilterContext context{RgbColor{255, 255, 255}, TerminalColorFilter::none};
  boxrig_ui_color_filter_input input{};
  input.target = BOXRIG_UI_COLOR_CELL;
  input.foreground_source.role = BOXRIG_UI_COLOR_DEFAULT_FOREGROUND;
  input.background_source.role = BOXRIG_UI_COLOR_DEFAULT_BACKGROUND;
  auto result = filter_terminal_colors(&input, &context);
  assert(equal(rgb(result.foreground), {0, 0, 0}) && equal(rgb(result.background), {255, 255, 255}));
  std::swap(input.foreground_source, input.background_source);
  input.attributes = BOXRIG_CELL_INVERSE;
  result = filter_terminal_colors(&input, &context);
  assert(equal(rgb(result.foreground), {255, 255, 255}) && equal(rgb(result.background), {0, 0, 0}));
  context = {std::nullopt, TerminalColorFilter::oklch};
  input = {};
  input.colors = {{128, 0, 0}, {0, 0, 0}, {0, 96, 0}, {80, 80, 80}, false};
  input.foreground_source.role = input.underline_source.role = input.marker_source.role = BOXRIG_UI_COLOR_RGB;
  for (const auto target : {BOXRIG_UI_COLOR_CELL, BOXRIG_UI_COLOR_IME}) {
    input.target = target;
    result = filter_terminal_colors(&input, &context);
    assert(contrast(rgb(result.foreground), {0, 0, 0}) >= 4.5);
    assert(contrast(rgb(result.underline), {0, 0, 0}) >= 4.5);
    assert(equal(rgb(result.background), {0, 0, 0}));
  }
  input.colors.underline_follows_foreground = true;
  result = filter_terminal_colors(&input, &context);
  assert(result.underline_follows_foreground);
  input.underlay_valid = true;
  input.underlay = result;
  for (const auto target : {BOXRIG_UI_COLOR_CURSOR, BOXRIG_UI_COLOR_SELECTION_MARKER, BOXRIG_UI_COLOR_SEARCH_MARKER,
      BOXRIG_UI_COLOR_HOVER_MARKER, BOXRIG_UI_COLOR_IME_MARKER}) {
    input.target = target;
    const auto underlay = input.underlay;
    result = filter_terminal_colors(&input, &context);
    assert(contrast(rgb(result.marker), rgb(underlay.background)) >= 4.5);
    assert(equal(rgb(input.underlay.foreground), rgb(underlay.foreground)));
  }
  input.target = BOXRIG_UI_COLOR_SURFACE;
  assert(equal(rgb(filter_terminal_colors(&input, &context).background), {0, 0, 0}));
  context.filter = TerminalColorFilter::none;
  assert(equal(rgb(filter_terminal_colors(&input, &context).foreground), rgb(input.colors.foreground)));

  auto config = boxrig_terminal_config_default();
  config.rows = 2; config.columns = 20;
  boxrig_terminal *terminal = nullptr;
  boxrig_ui_model *model = nullptr;
  boxrig_ui_view *view = nullptr;
  const auto view_config = boxrig_ui_view_config_default();
  assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
  assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
  assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
  const char bytes[] = "\033]4;4;rgb:0000/0000/8080\a\033]10;rgb:0000/0000/ffff\a\033[34;4mABC\033[0mDEF"
      "\033[38;5;166;48;5;37;58;2;10;80;160;4mG\033[38;2;200;80;80;48;2;30;90;60;7mH\033[0m";
  std::size_t consumed = 0;
  assert(boxrig_terminal_feed(terminal, reinterpret_cast<const uint8_t *>(bytes), std::strlen(bytes), &consumed) == BOXRIG_STATUS_OK);
  assert(consumed == std::strlen(bytes));
  assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
  assert(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
  boxrig_ui_allocation_input allocation{};
  allocation.allocation_width = 200 * BOXRIG_UI_FIXED_ONE;
  allocation.allocation_height = 20 * BOXRIG_UI_FIXED_ONE;
  allocation.cell_width = allocation.cell_height = 10 * BOXRIG_UI_FIXED_ONE;
  allocation.cell_metrics_revision = 1;
  allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
  boxrig_ui_layout_result layout{};
  assert(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
  boxrig_ui_hit_test_result begin{}, end{};
  assert(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &begin) == BOXRIG_UI_OK);
  assert(boxrig_ui_view_hit_test(view, 51 * BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &end) == BOXRIG_UI_OK);
  assert(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER, &begin) == BOXRIG_UI_OK);
  assert(boxrig_ui_view_update_selection(view, &end) == BOXRIG_UI_OK);
  const boxrig_text_snapshot_options options{BOXRIG_TEXT_SCREEN_PRIMARY, nullptr, 1024, 16};
  const auto text = [&] {
    boxrig_text_snapshot *snapshot = nullptr;
    assert(boxrig_terminal_create_text_snapshot(terminal, &options, &snapshot) == BOXRIG_STATUS_OK);
    std::size_t length = 0;
    const auto *data = boxrig_text_snapshot_data(snapshot, &length);
    const std::string result(reinterpret_cast<const char *>(data), length);
    boxrig_text_snapshot_destroy(snapshot);
    return result;
  };
  const auto before_text = text();
  const auto color_revision = boxrig_terminal_color_revision(terminal);
  context = {std::nullopt, TerminalColorFilter::oklch};
  assert(boxrig_ui_view_set_color_filter(view, filter_terminal_colors, &context) == BOXRIG_UI_OK);
  for (const auto filter : {TerminalColorFilter::oklch, TerminalColorFilter::reverse_lightness,
      TerminalColorFilter::reverse_contrast, TerminalColorFilter::reverse_roles, TerminalColorFilter::reverse_cam16,
      TerminalColorFilter::reverse_palette}) {
  context.filter = filter;
  const bool readable = filter != TerminalColorFilter::reverse_lightness && filter != TerminalColorFilter::reverse_contrast;
  for (const auto profile : {TerminalColorVision::none, TerminalColorVision::protan, TerminalColorVision::deutan, TerminalColorVision::tritan}) {
    context.color_vision = profile;
    assert(prepare_terminal_color_filter(&context, terminal));
    assert(boxrig_ui_view_set_color_filter(view, filter_terminal_colors, &context) == BOXRIG_UI_OK);
    for (const bool focused : {true, false}) {
    assert(boxrig_ui_view_set_focused(view, focused) == BOXRIG_UI_OK);
    for (int frame = 0; frame < 100; frame++) for (std::size_t column = 0; column < 8; column++) {
      boxrig_cell_view cell{};
      assert(boxrig_terminal_get_cell(terminal, 0, column, &cell) == BOXRIG_STATUS_OK);
      assert(boxrig_ui_color_resolve_cell(terminal, &cell, nullptr, 128, &result, &input) == BOXRIG_UI_OK);
      input.state_flags = BOXRIG_UI_COLOR_SELECTED | BOXRIG_UI_COLOR_SEARCH_MATCH | BOXRIG_UI_COLOR_HYPERLINK_HOVER |
          (focused ? BOXRIG_UI_COLOR_FOCUSED : 0);
      assert(boxrig_ui_view_resolve_colors(view, &input, &result) == BOXRIG_UI_OK);
      if (readable) assert(contrast(rgb(result.foreground), rgb(result.background)) >= 4.5);
      if (column < 6) assert(equal(rgb(result.underline), rgb(result.foreground)));
      else if (readable) assert(contrast(rgb(result.underline), rgb(result.background)) >= 4.5);
      boxrig_cell_view unchanged{};
      assert(boxrig_terminal_get_cell(terminal, 0, column, &unchanged) == BOXRIG_STATUS_OK);
      assert(unchanged.foreground.kind == cell.foreground.kind && unchanged.background.kind == cell.background.kind);
    }
    }
  }
  }
  boxrig_rgb palette{}, dynamic{};
  assert(boxrig_terminal_get_palette_color(terminal, 4, &palette) == BOXRIG_STATUS_OK && equal(rgb(palette), {0, 0, 128}));
  assert(boxrig_terminal_get_dynamic_color(terminal, BOXRIG_DYNAMIC_COLOR_FOREGROUND, &dynamic) == BOXRIG_STATUS_OK && equal(rgb(dynamic), {0, 0, 255}));
  assert(text() == before_text && text().starts_with("ABCDEF"));
  boxrig_text_snapshot *copied = nullptr;
  assert(boxrig_ui_view_create_selection_snapshot(view, &copied) == BOXRIG_UI_OK);
  std::size_t copy_length = 0;
  const auto *copy_bytes = boxrig_text_snapshot_data(copied, &copy_length);
  assert(std::string(reinterpret_cast<const char *>(copy_bytes), copy_length) == "ABCDEF");
  boxrig_text_snapshot_destroy(copied);
  assert(boxrig_terminal_color_revision(terminal) == color_revision);
  assert(boxrig_ui_view_set_color_filter(view, nullptr, nullptr) == BOXRIG_UI_OK);
  assert(boxrig_ui_view_resolve_colors(view, &input, &result) == BOXRIG_UI_OK && equal(rgb(result.foreground), rgb(input.colors.foreground)));
  boxrig_ui_view_destroy(view); boxrig_ui_model_destroy(model); boxrig_terminal_destroy(terminal);
  std::puts("PASS Oklch conversion, hue/contrast, defaults, inverse, underline and marker targets");
}

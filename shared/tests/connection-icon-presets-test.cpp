// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/connection-icon-presets.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
    const auto &catalog = elder_terms::connection_icon_presets();
    std::unordered_set<std::string> specifications;
    std::size_t noto_count = 0;
    std::size_t material_count = 0;
    std::size_t glyphs_count = 0;
    for (const auto &preset : catalog) {
      expect(!preset.name.empty() && !preset.specification.empty(),
             "every preset needs a stable ID and English name");
      expect(specifications.insert(preset.specification).second,
             "preset specifications must be unique");
      if (preset.family == elder_terms::ConnectionIconFamily::Noto) {
        ++noto_count;
        expect(preset.specification.starts_with("noto:"),
               "Noto IDs must use their source artwork identifier");
      } else if (preset.family == elder_terms::ConnectionIconFamily::Material) {
        ++material_count;
        expect(preset.specification.starts_with("material:"),
               "Material IDs must use their source category and icon name");
      } else {
        ++glyphs_count;
        expect(preset.specification.starts_with("glyphs-poly:"),
               "Glyphs Poly IDs must use their source component name");
        expect(elder_terms::connection_icon_preset_thumbnail(preset.specification, 32) != nullptr,
               "every Glyphs Poly selection must resolve to a bundled image");
      }
    }
    expect(noto_count > 0 && material_count > 0 && glyphs_count > 0,
           "all preset families must offer selectable icons");
    for (const char *specification : {
             "noto:emoji_u1f44d", "material:action/home", "material:white:action/home",
             "glyphs-poly:folder"}) {
      const auto image = elder_terms::connection_icon_preset_thumbnail(
          specification, 64);
      expect(image != nullptr && gdk_pixbuf_get_width(image.get()) == 64 &&
                 gdk_pixbuf_get_height(image.get()) == 64 &&
                 gdk_pixbuf_get_has_alpha(image.get()),
             "all preset families must resolve to square RGBA thumbnails");
    }
    const auto black = elder_terms::connection_icon_preset_thumbnail("material:action/home", 256);
    const auto white = elder_terms::connection_icon_preset_thumbnail("material:white:action/home", 256);
    const auto *black_pixels = gdk_pixbuf_read_pixels(black.get());
    const auto *white_pixels = gdk_pixbuf_read_pixels(white.get());
    const int black_stride = gdk_pixbuf_get_rowstride(black.get());
    const int white_stride = gdk_pixbuf_get_rowstride(white.get());
    bool has_black = false, has_white = false;
    for (int y = 0; y < 256; ++y) {
      for (int x = 0; x < 256; ++x) {
        const auto *left = black_pixels + y * black_stride + x * 4;
        const auto *right = white_pixels + y * white_stride + x * 4;
        expect(left[3] == right[3], "glyph colors must preserve the same transparent shape");
        if (x == 0 || y == 0 || x == 255 || y == 255)
          expect(left[3] == 0, "Material outlines must leave transparent padding");
        if (left[3] == 255) {
          has_black |= left[0] == 0;
          has_white |= left[0] == 255;
          expect(std::abs(int(left[0]) + right[0] - 255) <= 1,
                 "white glyphs must invert the black glyph and its outline");
        }
      }
    }
    expect(has_black && has_white, "Material icons must contain a glyph and contrasting outline");
    expect(black_pixels[128 * black_stride + 32 * 4 + 3] == 0,
           "Material icons must not retain the old background badge");
    expect(elder_terms::connection_icon_preset_thumbnail(
               "material:action/unknown", 24) == nullptr &&
               elder_terms::connection_icon_preset_thumbnail(
                   "material:white:action/unknown", 24) == nullptr &&
               elder_terms::connection_icon_preset_thumbnail(
                   "noto:emoji_u1f44d", 0) == nullptr,
           "unknown IDs and invalid sizes must not yield an image");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}

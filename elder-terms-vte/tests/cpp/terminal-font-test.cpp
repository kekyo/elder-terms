#include "../../src/terminal-font.h"

#include <cstring>
#include <iostream>

#include <elder-terms/settings.h>

namespace elder_terms {

static bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "terminal-font-test: FAIL: " << message << '\n';
    return false;
  }
  return true;
}

static PangoFontDescription *create_base_font() {
  PangoFontDescription *font = pango_font_description_new();
  pango_font_description_set_family(font, "Runtime Default Mono");
  pango_font_description_set_size(font, 17 * PANGO_SCALE);
  pango_font_description_set_style(font, PANGO_STYLE_ITALIC);
  pango_font_description_set_weight(font, PANGO_WEIGHT_SEMIBOLD);
  pango_font_description_set_stretch(font, PANGO_STRETCH_CONDENSED);
  pango_font_description_set_variant(font, PANGO_VARIANT_SMALL_CAPS);
  return font;
}

static bool ordered_families_preserve_every_non_family_field() {
  PangoFontDescription *base = create_base_font();
  PangoFontDescription *font = create_terminal_font_description(
      base,
      {
          .primary_family = std::string("Latin Mono"),
          .fallback_family = std::string("CJK Gothic"),
      });

  const char *family = pango_font_description_get_family(font);
  const bool passed =
      expect(family != nullptr &&
                 std::strcmp(family, "Latin Mono,CJK Gothic") == 0,
             "the fallback family should follow the primary family") &&
      expect(pango_font_description_get_size(font) ==
                 pango_font_description_get_size(base),
             "family selection should preserve the existing font size") &&
      expect(pango_font_description_get_size_is_absolute(font) ==
                 pango_font_description_get_size_is_absolute(base),
             "family selection should preserve relative size semantics") &&
      expect(pango_font_description_get_style(font) ==
                 pango_font_description_get_style(base),
             "family selection should preserve font style") &&
      expect(pango_font_description_get_weight(font) ==
                 pango_font_description_get_weight(base),
             "family selection should preserve font weight") &&
      expect(pango_font_description_get_stretch(font) ==
                 pango_font_description_get_stretch(base),
             "family selection should preserve font stretch") &&
      expect(pango_font_description_get_variant(font) ==
                 pango_font_description_get_variant(base),
             "family selection should preserve font variant");

  pango_font_description_free(font);
  pango_font_description_free(base);
  return passed;
}

static bool built_in_families_preserve_the_runtime_font_size() {
  const SettingsStore store = create_default_settings(
      default_terminal_display_settings(1.0), "elder-terms");
  const TerminalFontFamilies families = terminal_font_families(store);
  PangoFontDescription *base = create_base_font();
  PangoFontDescription *font =
      create_terminal_font_description(base, families);

  const char *family = pango_font_description_get_family(font);
  const bool passed =
      expect(family != nullptr &&
                 std::strcmp(family, "Noto Sans Mono,Monospace") == 0,
             "the built-in families should be ordered for fallback") &&
      expect(pango_font_description_get_size(font) ==
                 pango_font_description_get_size(base),
             "the built-in families should preserve the runtime font size");

  pango_font_description_free(font);
  pango_font_description_free(base);
  return passed;
}

static bool fallback_only_keeps_the_runtime_family_first() {
  PangoFontDescription *base = create_base_font();
  PangoFontDescription *font = create_terminal_font_description(
      base,
      {
          .primary_family = std::nullopt,
          .fallback_family = std::string("CJK Gothic"),
      });
  const char *family = pango_font_description_get_family(font);
  const bool passed = expect(
      family != nullptr &&
          std::strcmp(family, "Runtime Default Mono,CJK Gothic") == 0,
      "fallback-only settings should follow VTE's runtime default family");

  pango_font_description_free(font);
  pango_font_description_free(base);
  return passed;
}

static bool unspecified_families_leave_the_runtime_font_unchanged() {
  PangoFontDescription *base = create_base_font();
  PangoFontDescription *font = create_terminal_font_description(
      base,
      {
          .primary_family = std::nullopt,
          .fallback_family = std::nullopt,
      });
  const bool passed = expect(
      pango_font_description_equal(base, font) != FALSE,
      "unspecified families should preserve the complete runtime font");

  pango_font_description_free(font);
  pango_font_description_free(base);
  return passed;
}

} // namespace elder_terms

int main() {
  return elder_terms::ordered_families_preserve_every_non_family_field() &&
                 elder_terms::
                     built_in_families_preserve_the_runtime_font_size() &&
                 elder_terms::fallback_only_keeps_the_runtime_family_first() &&
                 elder_terms::
                     unspecified_families_leave_the_runtime_font_unchanged()
             ? 0
             : 1;
}

#include "terminal-font.h"

#include <string>

namespace elder_terms {

PangoFontDescription *create_terminal_font_description(
    const PangoFontDescription *runtime_font,
    const TerminalFontFamilies &font_families) {
  PangoFontDescription *font =
      runtime_font == nullptr ? pango_font_description_new()
                              : pango_font_description_copy(runtime_font);

  const char *runtime_family =
      runtime_font == nullptr
          ? nullptr
          : pango_font_description_get_family(runtime_font);
  std::string primary =
      font_families.primary_family.has_value()
          ? font_families.primary_family.value()
          : std::string(runtime_family == nullptr ? "" : runtime_family);
  if (primary.empty() && font_families.fallback_family.has_value()) {
    primary = font_families.fallback_family.value();
  } else if (font_families.fallback_family.has_value() &&
             !font_families.fallback_family->empty() &&
             font_families.fallback_family.value() != primary) {
    primary += "," + font_families.fallback_family.value();
  }

  if (!primary.empty()) {
    pango_font_description_set_family(font, primary.c_str());
  }
  return font;
}

} // namespace elder_terms

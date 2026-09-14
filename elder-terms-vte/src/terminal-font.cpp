#include "terminal-font.h"

#include <string>

namespace elder_terms {

PangoFontDescription *create_terminal_font_description(
    const PangoFontDescription *runtime_font,
    const TerminalFontFamilies &font_families) {
  PangoFontDescription *font =
      runtime_font == nullptr ? pango_font_description_new()
                              : pango_font_description_copy(runtime_font);

  std::string ordered;
  for (const auto &family : font_families.families) {
    if (!ordered.empty()) ordered += ',';
    ordered += family;
  }

  if (!ordered.empty()) {
    pango_font_description_set_family(font, ordered.c_str());
  }
  return font;
}

} // namespace elder_terms

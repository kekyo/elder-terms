#pragma once

#include <pango/pango.h>

#include <elder-terms/settings.h>

namespace elder_terms {

/**
 * Creates a terminal font description with ordered family overrides.
 *
 * All non-family fields, including font size, are copied from the runtime
 * base description. An empty family vector leaves that description unchanged.
 *
 * @param runtime_font VTE's unscaled runtime font description, or null.
 * @param font_families Family names in descending priority.
 * @returns A newly allocated font description. The caller must free it with
 * pango_font_description_free().
 */
PangoFontDescription *create_terminal_font_description(
    const PangoFontDescription *runtime_font,
    const TerminalFontFamilies &font_families);

} // namespace elder_terms

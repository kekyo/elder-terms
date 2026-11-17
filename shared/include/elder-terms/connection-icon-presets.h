// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <elder-terms/export.h>

namespace elder_terms {

/** Family of a bundled connection icon preset. */
enum class ConnectionIconFamily {
  /** Noto Color Emoji artwork. */
  Noto,
  /** Classic Material Icons filled artwork. */
  Material,
  /** Glyphs Poly multicolor artwork. */
  GlyphsPoly,
};

/** Metadata for a bundled connection icon. */
struct ConnectionIconPreset {
  /** Stable value saved in the connection settings. */
  std::string specification;
  /** English display name for the selection browser. */
  std::string name;
  /** Source artwork family. */
  ConnectionIconFamily family;
};

/**
 * Lists all bundled connection icons in a stable display order.
 *
 * @returns Process-lifetime catalog of preset identifiers, names, and families.
 */
ELDER_TERMS_API const std::vector<ConnectionIconPreset> &connection_icon_presets();

/**
 * Decodes one bundled preset at the requested square thumbnail size.
 *
 * @param specification Stable `noto:id` or `material:id` catalog value, or
 * `material:white:id` for a white Material glyph, or `glyphs-poly:name`.
 * Material icons have a
 * transparent background and an opposite-color outline.
 * @param pixel_size Width and height in pixels, from 1 through 256.
 * @returns Owned RGBA thumbnail, or null for an unknown value or invalid size.
 * @remarks Callers may cache but must not mutate the returned pixels.
 */
ELDER_TERMS_API std::shared_ptr<GdkPixbuf> connection_icon_preset_thumbnail(
    const std::string &specification, int pixel_size);

} // namespace elder_terms

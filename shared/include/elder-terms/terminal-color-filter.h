// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once
#include <boxrig/ui/color.h>
#include <elder-terms/settings/terminal-settings.h>
#include <memory>
#include <cstdint>

namespace elder_terms {
/** Perceptual color in D65 Oklch, used for display correction. */
struct Oklch {
  double lightness; /**< Lightness, normally zero through one. */
  double chroma;    /**< Nonnegative chroma. */
  double hue;       /**< Hue angle in degrees, zero through 360. */
};
/** @param color Unassociated eight-bit sRGB. @returns D65 Oklch color. */
ELDER_TERMS_API Oklch srgb_to_oklch(RgbColor color);
/** Reduces chroma at fixed hue and lightness to fit sRGB.
 * @param color Oklch color; lightness is clamped, nonfinite input yields black.
 * @returns Eight-bit sRGB color. */
ELDER_TERMS_API RgbColor oklch_to_srgb(Oklch color);
/** Adjusts Oklch lightness to reach 4.5:1 contrast, reducing chroma only to fit sRGB.
 * @param foreground Original sRGB foreground. @param background Reference background.
 * @returns Hue-preserving contrast correction for all hues and neutrals, or the unchanged foreground. */
ELDER_TERMS_API RgbColor improve_color_visibility(RgbColor foreground, RgbColor background);
/** Recolors a display color using a type-specific redistribution of lost color information.
 * @param color Original unassociated eight-bit sRGB.
 * @param profile Color-vision profile, or none for identity.
 * @param strength Correction amount clamped to zero through one; nonfinite means zero.
 * @returns Recolored sRGB. Exact neutrals and zero-strength colors are unchanged.
 * @remarks Apply contrast correction after this function; strength is not clinical severity. */
ELDER_TERMS_API RgbColor correct_color_vision(RgbColor color, TerminalColorVision profile, double strength);
/** Approximates dichromatic appearance for evaluation, not for display correction.
 * @param color Original unassociated eight-bit sRGB.
 * @param profile Brettel 1997 profile adapted to sRGB, or none for identity.
 * @returns Simulated sRGB with gamut clipping; exact neutrals are preserved.
 * @remarks A simulation is an evaluation aid and does not establish individual usability. */
ELDER_TERMS_API RgbColor simulate_color_vision(RgbColor color, TerminalColorVision profile);
/** Immutable precomputed palette and interpolation data for filter E. */
struct ReversePalette;
/** Application annotation tag requesting local-link dimming after color correction. */
inline constexpr std::uint64_t terminal_dim_link_color_tag = 1;
/** Stable application-owned color hook context. */
struct TerminalColorFilterContext {
  std::optional<RgbColor> background; /**< Connection override for the default background. */
  TerminalColorFilter filter = TerminalColorFilter::none; /**< Exclusive correction after default-color resolution. */
  TerminalColorVision color_vision = TerminalColorVision::none; /**< Independent recoloring profile. */
  double color_vision_strength = 1; /**< Recoloring amount from zero through one. */
  std::shared_ptr<const ReversePalette> reverse_palette{}; /**< Prepared outside the display callback. */
  bool dim_links = false; /**< Applies Oklch dimming to tagged local-link cells. */
};
/** Prepares the palette filter when settings or the terminal's color revision change.
 * @param context Application-owned context, outside a rendering callback.
 * @param terminal Terminal supplying palette/default colors, or nullptr for the preview's standard palette.
 * @returns Whether preparation succeeded; an unchanged palette retains its shared pointer.
 * @remarks Call before rendering. If the pointer changes, invalidate view colors.
 * Other modes release the precomputed data. Logical terminal colors are never modified. */
ELDER_TERMS_API bool prepare_terminal_color_filter(TerminalColorFilterContext *context, const boxrig_terminal *terminal);
/** Pure boxrig display-color callback; never changes the terminal or its underlay.
 * @param input Original resolved colors and already-filtered underlay.
 * @param context Borrowed TerminalColorFilterContext, valid throughout registration.
 * @returns Display colors with connection defaults and selected correction. */
ELDER_TERMS_API boxrig_ui_color_filter_result filter_terminal_colors(const boxrig_ui_color_filter_input *input, void *context);
}

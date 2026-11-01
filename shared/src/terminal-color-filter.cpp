// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/terminal-color-filter.h>
#include "color-appearance.h"
#include "terminal-color-palette.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace elder_terms {
struct LinearRgb { double red, green, blue; };

static double linear_channel(double value) {
  value /= 255;
  return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
}
static LinearRgb linear_rgb(RgbColor color) {
  return {linear_channel(color.red), linear_channel(color.green), linear_channel(color.blue)};
}
static double luminance(LinearRgb color) { return .2126 * color.red + .7152 * color.green + .0722 * color.blue; }
static double contrast(double a, double b) { return (std::max(a, b) + .05) / (std::min(a, b) + .05); }

Oklch srgb_to_oklch(RgbColor color) {
  // D65 matrices published by Björn Ottosson (public domain):
  // https://bottosson.github.io/posts/oklab/#converting-from-linear-srgb-to-oklab
  const auto rgb = linear_rgb(color);
  const double l = std::cbrt(.4122214708 * rgb.red + .5363325363 * rgb.green + .0514459929 * rgb.blue);
  const double m = std::cbrt(.2119034982 * rgb.red + .6806995451 * rgb.green + .1073969566 * rgb.blue);
  const double s = std::cbrt(.0883024619 * rgb.red + .2817188376 * rgb.green + .6299787005 * rgb.blue);
  const double a = 1.9779984951 * l - 2.4285922050 * m + .4505937099 * s;
  const double b = .0259040371 * l + .7827717662 * m - .8086757660 * s;
  const double hue = std::atan2(b, a) * 180 / std::numbers::pi;
  return {.2104542553 * l + .7936177850 * m - .0040720468 * s, std::hypot(a, b), hue < 0 ? hue + 360 : hue};
}

static LinearRgb oklab_to_linear(double lightness, double a, double b) {
  const double l0 = lightness + .3963377774 * a + .2158037573 * b;
  const double m0 = lightness - .1055613458 * a - .0638541728 * b;
  const double s0 = lightness - .0894841775 * a - 1.2914855480 * b;
  const double l = l0 * l0 * l0, m = m0 * m0 * m0, s = s0 * s0 * s0;
  return {4.0767416621 * l - 3.3077115913 * m + .2309699292 * s,
      -1.2684380046 * l + 2.6097574011 * m - .3413193965 * s,
      -.0041960863 * l - .7034186147 * m + 1.7076147010 * s};
}

static bool in_gamut(LinearRgb color) {
  // Allow matrix roundoff at exact primary/white boundaries, not visible clipping.
  return std::min({color.red, color.green, color.blue}) >= -1e-7 &&
      std::max({color.red, color.green, color.blue}) <= 1.0000001;
}

static LinearRgb gamut_map(double lightness, double chroma, double cosine, double sine) {
  auto color = oklab_to_linear(lightness, chroma * cosine, chroma * sine);
  if (in_gamut(color)) return color;
  double low = 0, high = chroma;
  for (int iteration = 0; iteration < 16; iteration++) {
    const double middle = (low + high) / 2;
    if (in_gamut(oklab_to_linear(lightness, middle * cosine, middle * sine))) low = middle;
    else high = middle;
  }
  return oklab_to_linear(lightness, low * cosine, low * sine);
}

static RgbColor encode(LinearRgb color) {
  const auto channel = [](double value) {
    value = std::clamp(value, 0.0, 1.0);
    const auto encoded = value <= .0031308 ? value * 12.92 : 1.055 * std::pow(value, 1 / 2.4) - .055;
    return static_cast<guint8>(std::lround(encoded * 255));
  };
  return {channel(color.red), channel(color.green), channel(color.blue)};
}

RgbColor oklch_to_srgb(Oklch color) {
  if (!std::isfinite(color.lightness) || !std::isfinite(color.chroma) || !std::isfinite(color.hue)) return {};
  const auto angle = std::fmod(color.hue, 360) * std::numbers::pi / 180;
  // All sRGB chroma is below one; cap larger inputs before cubing the matrices.
  return encode(gamut_map(std::clamp(color.lightness, 0.0, 1.0), std::clamp(color.chroma, 0.0, 1.0), std::cos(angle), std::sin(angle)));
}

RgbColor improve_color_visibility(RgbColor foreground, RgbColor background) {
  const auto bg = luminance(linear_rgb(background));
  const auto original_contrast = contrast(luminance(linear_rgb(foreground)), bg);
  if (original_contrast >= 4.5) return foreground;
  auto color = srgb_to_oklch(foreground);
  // Exact neutrals must remain neutral despite matrix roundoff.
  if (foreground.red == foreground.green && foreground.green == foreground.blue) color.chroma = 0;
  const double cosine = std::cos(color.hue * std::numbers::pi / 180), sine = std::sin(color.hue * std::numbers::pi / 180);
  const auto toward = [&](double endpoint) {
    const double direction = endpoint > color.lightness ? 1 : -1;
    const auto limit = gamut_map(endpoint, color.chroma, cosine, sine);
    auto best = limit;
    // Keep rounding headroom where possible. Near mid-gray, neither endpoint
    // may reach 4.6, so reduce the margin without dropping the 4.5 requirement.
    const double target = std::min(4.6, 4.5 + (contrast(luminance(limit), bg) - 4.5) / 2);
    double low = 0, high = std::abs(endpoint - color.lightness);
    for (int iteration = 0; iteration < 16; iteration++) {
      const double distance = (low + high) / 2;
      const auto candidate = gamut_map(color.lightness + direction * distance, color.chroma, cosine, sine);
      if (contrast(luminance(candidate), bg) >= target) { high = distance; best = candidate; }
      else low = distance;
    }
    const auto corrected = encode(best);
    return contrast(luminance(linear_rgb(corrected)), bg) >= 4.5 ? corrected : encode(limit);
  };
  if (contrast(0, bg) < 4.5) return toward(1);
  if (contrast(1, bg) < 4.5) return toward(0);
  // If both directions work, retain the closest perceptual lightness instead
  // of turning dark text nearly white just because white has more contrast.
  const auto darker = toward(0), lighter = toward(1);
  return std::abs(srgb_to_oklch(darker).lightness - color.lightness) <=
      std::abs(srgb_to_oklch(lighter).lightness - color.lightness) ? darker : lighter;
}

struct ColorVisionProjection {
  double first[9];
  double second[9];
  double normal[3];
};

static const ColorVisionProjection *color_vision_projection(TerminalColorVision profile) {
  // Brettel 1997, Smith-Pokorny LMS, sRGB D65 white as the neutral axis.
  // Precomputed coefficients by Nicolas Burrus, released to the public domain:
  // https://github.com/DaltonLens/libDaltonLens/blob/1a01fc1bf8d8dd419af8343b80b05e98ba50a75d/libDaltonLens.c
  static const ColorVisionProjection protan{
      {.14980, 1.19548, -.34528, .10764, .84864, .04372, .00384, -.00540, 1.00156},
      {.14570, 1.16172, -.30742, .10816, .85291, .03892, .00386, -.00524, 1.00139},
      {.00048, .00393, -.00441}};
  static const ColorVisionProjection deutan{
      {.36477, .86381, -.22858, .26294, .64245, .09462, -.02006, .02728, .99278},
      {.37298, .88166, -.25464, .25954, .63506, .10540, -.01980, .02784, .99196},
      {-.00281, -.00611, .00892}};
  static const ColorVisionProjection tritan{
      {1.01277, .13548, -.14826, -.01243, .86812, .14431, .07589, .80500, .11911},
      {.93678, .18979, -.12657, .06154, .81526, .12320, -.37562, 1.12767, .24796},
      {.03901, -.02788, -.01113}};
  switch (profile) {
    case TerminalColorVision::protan: return &protan;
    case TerminalColorVision::deutan: return &deutan;
    case TerminalColorVision::tritan: return &tritan;
    default: return nullptr;
  }
}

static LinearRgb project_color_vision(LinearRgb color, const ColorVisionProjection &projection) {
  const auto *normal = projection.normal;
  const auto *matrix = color.red * normal[0] + color.green * normal[1] + color.blue * normal[2] >= 0
      ? projection.first : projection.second;
  return {matrix[0] * color.red + matrix[1] * color.green + matrix[2] * color.blue,
      matrix[3] * color.red + matrix[4] * color.green + matrix[5] * color.blue,
      matrix[6] * color.red + matrix[7] * color.green + matrix[8] * color.blue};
}

RgbColor simulate_color_vision(RgbColor color, TerminalColorVision profile) {
  const auto *projection = color_vision_projection(profile);
  if (!projection || (color.red == color.green && color.green == color.blue)) return color;
  return encode(project_color_vision(linear_rgb(color), *projection));
}
RgbColor correct_color_vision(RgbColor color, TerminalColorVision profile, double strength) {
  const auto *projection = color_vision_projection(profile);
  if (!projection || !std::isfinite(strength) || strength <= 0 ||
      (color.red == color.green && color.green == color.blue)) return color;
  // Limit the transfer gain so saturated ANSI colors remain distinct.
  strength = 0.75 * std::min(strength, 1.0);
  auto rgb = linear_rgb(color);
  const auto simulated = project_color_vision(rgb, *projection);
  // Redistribute a lost component onto the other chromatic axis. The green
  // adjustment balances sRGB luminance before gamut clipping. Unlike a common
  // equal green/blue error shift, this retains ANSI red/green separation after
  // Oklch contrast correction. Tritan uses the opposite red/green direction.
  // These are application choices evaluated in plan5, not clinical parameters.
  // Use a fixed normalization per profile to avoid a change in strength when
  // crossing the two Brettel projection planes.
  if (profile == TerminalColorVision::protan || profile == TerminalColorVision::deutan) {
    const double loss = profile == TerminalColorVision::protan
        ? (rgb.red - simulated.red) / (1 - projection->first[0])
        : (rgb.green - simulated.green) / (1 - projection->first[4]);
    rgb.green -= strength * loss * (.0722 / .7152);
    rgb.blue += strength * loss;
  } else {
    const double loss = (rgb.blue - simulated.blue) / (1 - projection->first[8]);
    rgb.red -= strength * loss;
    rgb.green += strength * loss * (.2126 / .7152);
  }
  // Clip only the final linear RGB. Clipping the simulation before computing
  // its error would discard the signal used to recolor saturated colors.
  return encode(rgb);
}

static RgbColor color_at_luminance(Oklch color, double target) {
  const double radians = color.hue * std::numbers::pi / 180;
  const double cosine = std::cos(radians), sine = std::sin(radians);
  double low = 0, high = 1;
  for (int iteration = 0; iteration < 24; iteration++) {
    const auto middle = (low + high) / 2;
    if (luminance(gamut_map(middle, color.chroma, cosine, sine)) < target) low = middle;
    else high = middle;
  }
  return encode(gamut_map((low + high) / 2, color.chroma, cosine, sine));
}

static boxrig_rgb reverse_color(boxrig_rgb value, TerminalColorFilter filter) {
  auto color = srgb_to_oklch({value.red, value.green, value.blue});
  color.lightness = 1 - color.lightness;
  if (value.red == value.green && value.green == value.blue) color.chroma = 0;
  // Yoon-Buck's continuous luminance involution preserves all pairwise WCAG
  // ratios. Here we find that Y in Oklch instead of HSL; eight-bit rounding
  // introduces a small measurable error. No visibility floor belongs in B.
  // https://jordan.yoonbuck.com/post/contrast-preserving-inversion/derivation/
  const auto y = luminance(linear_rgb({value.red, value.green, value.blue}));
  const auto result = filter == TerminalColorFilter::reverse_contrast
      ? color_at_luminance(color, (1 - y) / (1 + 20 * y)) : oklch_to_srgb(color);
  return {result.red, result.green, result.blue};
}

static double reverse_tone(double lightness) {
  // Reflect the encoded neutral axis, then return to perceptual lightness.
  // This maps near-black backgrounds near white without an RGB equality test.
  const auto y = std::pow(std::clamp(lightness, 0.0, 1.0), 3);
  const auto encoded = y <= .0031308 ? 12.92 * y : 1.055 * std::pow(y, 1 / 2.4) - .055;
  return std::cbrt(linear_channel(255 * (1 - encoded)));
}

static boxrig_rgb reverse_background(boxrig_rgb value, TerminalColorFilter filter) {
  auto color = srgb_to_oklch({value.red, value.green, value.blue});
  color.lightness = reverse_tone(color.lightness);
  auto result = oklch_to_srgb(color);
  if (filter == TerminalColorFilter::reverse_cam16)
    result = compensate_color_appearance({value.red, value.green, value.blue}, result);
  return {result.red, result.green, result.blue};
}

static boxrig_rgb reverse_foreground(boxrig_rgb value, boxrig_rgb background, TerminalColorFilter filter) {
  auto color = srgb_to_oklch({value.red, value.green, value.blue});
  const RgbColor bg{background.red, background.green, background.blue};
  const auto y = luminance(linear_rgb(bg));
  const bool dark = contrast(0, y) >= contrast(1, y);
  const double target = std::min(4.6, 4.5 + (contrast(dark ? 0 : 1, y) - 4.5) / 2);
  const double limit_y = dark ? (y + .05) / target - .05 : (y + .05) * target - .05;
  const auto boundary = srgb_to_oklch(color_at_luminance(color, limit_y)).lightness;
  const auto limit = dark ? boundary : 1 - boundary;
  const auto distance = dark ? color.lightness : 1 - color.lightness;
  // A soft knee leaves dark chromatic ink unchanged, then compresses bright
  // shades into the readable range without flattening all of them at 4.5:1.
  const auto knee = .7 * limit;
  const auto compressed = distance <= knee ? distance :
      limit - (limit - knee) * std::exp(-(distance - knee) / std::max(1e-6, limit - knee));
  auto amount = std::clamp(color.chroma / .08, 0.0, 1.0);
  amount = amount * amount * (3 - 2 * amount);
  color.lightness = std::lerp(reverse_tone(color.lightness), dark ? compressed : 1 - compressed, amount);
  auto candidate = oklch_to_srgb(color);
  if (filter == TerminalColorFilter::reverse_cam16)
    candidate = compensate_color_appearance({value.red, value.green, value.blue}, candidate);
  const auto result = improve_color_visibility(candidate, bg);
  return {result.red, result.green, result.blue};
}

static boxrig_ui_color_filter_result filter_terminal_base_colors(const boxrig_ui_color_filter_input *input, void *data) {
  const auto &context = *static_cast<const TerminalColorFilterContext *>(data);
  auto result = boxrig_ui_color_filter_result_default(input);
  if (context.background) {
    const auto background = *context.background;
    const auto y = luminance(linear_rgb(background));
    const guint8 text = contrast(1, y) >= contrast(0, y) ? 255 : 0;
    const auto replace_default = [&](boxrig_rgb value, boxrig_ui_color_source_role role) {
      if (role == BOXRIG_UI_COLOR_DEFAULT_BACKGROUND) return boxrig_rgb{background.red, background.green, background.blue};
      if (role == BOXRIG_UI_COLOR_DEFAULT_FOREGROUND || role == BOXRIG_UI_COLOR_DEFAULT_CURSOR) return boxrig_rgb{text, text, text};
      return value;
    };
    result.foreground = replace_default(result.foreground, input->foreground_source.role);
    result.background = replace_default(result.background, input->background_source.role);
    result.underline = replace_default(result.underline, input->underline_source.role);
    result.marker = replace_default(result.marker, input->marker_source.role);
  }
  if (context.color_vision != TerminalColorVision::none && context.color_vision_strength > 0) {
    const auto recolor = [&](boxrig_rgb value, boxrig_ui_color_source_role role) {
      // Origins are already swapped for inverse cells. Preserve the connection
      // background even when it is being used as the displayed foreground.
      if (role == BOXRIG_UI_COLOR_DEFAULT_BACKGROUND || role == BOXRIG_UI_COLOR_UNUSED ||
          role == BOXRIG_UI_COLOR_FOLLOWS_FOREGROUND) return value;
      const auto color = correct_color_vision({value.red, value.green, value.blue},
          context.color_vision, context.color_vision_strength);
      return boxrig_rgb{color.red, color.green, color.blue};
    };
    if (input->target == BOXRIG_UI_COLOR_CELL || input->target == BOXRIG_UI_COLOR_IME ||
        input->target == BOXRIG_UI_COLOR_SURFACE) {
      result.foreground = recolor(result.foreground, input->foreground_source.role);
      result.background = recolor(result.background, input->background_source.role);
      if (!result.underline_follows_foreground)
        result.underline = recolor(result.underline, input->underline_source.role);
    } else result.marker = recolor(result.marker, input->marker_source.role);
  }
  if (context.filter == TerminalColorFilter::reverse_lightness || context.filter == TerminalColorFilter::reverse_contrast) {
    if (input->target == BOXRIG_UI_COLOR_CELL || input->target == BOXRIG_UI_COLOR_IME ||
        input->target == BOXRIG_UI_COLOR_SURFACE) {
      result.background = reverse_color(result.background, context.filter);
      result.foreground = reverse_color(result.foreground, context.filter);
      if (!result.underline_follows_foreground) result.underline = reverse_color(result.underline, context.filter);
    } else result.marker = reverse_color(result.marker, context.filter);
    return result;
  }
  if (context.filter == TerminalColorFilter::reverse_roles || context.filter == TerminalColorFilter::reverse_cam16 ||
      context.filter == TerminalColorFilter::reverse_palette) {
    const auto foreground = [&](boxrig_rgb value, boxrig_rgb background) {
      const auto candidate = reverse_foreground(value, background, context.filter);
      if (context.filter != TerminalColorFilter::reverse_palette || !context.reverse_palette) return candidate;
      const auto adjusted = apply_reverse_palette({value.red, value.green, value.blue},
          {candidate.red, candidate.green, candidate.blue}, {background.red, background.green, background.blue}, *context.reverse_palette);
      return boxrig_rgb{adjusted.red, adjusted.green, adjusted.blue};
    };
    if (input->target == BOXRIG_UI_COLOR_CELL || input->target == BOXRIG_UI_COLOR_IME ||
        input->target == BOXRIG_UI_COLOR_SURFACE) {
      result.background = reverse_background(result.background, context.filter);
      result.foreground = foreground(result.foreground, result.background);
      if (!result.underline_follows_foreground)
        result.underline = foreground(result.underline, result.background);
    } else {
      // Marker underlays are display colors, already filtered exactly once.
      const auto background = input->underlay_valid ? input->underlay.background : reverse_background(result.background, context.filter);
      result.marker = foreground(result.marker, background);
    }
    return result;
  }
  if (context.filter != TerminalColorFilter::oklch) return result;
  const auto correct = [](boxrig_rgb foreground, boxrig_rgb background) {
    const auto color = improve_color_visibility({foreground.red, foreground.green, foreground.blue},
        {background.red, background.green, background.blue});
    return boxrig_rgb{color.red, color.green, color.blue};
  };
  if (input->target == BOXRIG_UI_COLOR_CELL || input->target == BOXRIG_UI_COLOR_IME) {
    result.foreground = correct(result.foreground, result.background);
    if (!result.underline_follows_foreground) result.underline = correct(result.underline, result.background);
  } else if (input->target != BOXRIG_UI_COLOR_SURFACE) {
    // The underlay has already passed through this hook. Use it only as a
    // contrast reference; do not apply defaults or correction to it again.
    result.marker = correct(result.marker, input->underlay_valid ? input->underlay.background : result.background);
  }
  return result;
}

boxrig_ui_color_filter_result filter_terminal_colors(const boxrig_ui_color_filter_input *input, void *data) {
  auto result = filter_terminal_base_colors(input, data);
  const auto &context = *static_cast<const TerminalColorFilterContext *>(data);
  if (!context.dim_links || input->target != BOXRIG_UI_COLOR_CELL ||
      input->annotation_color_tag != terminal_dim_link_color_tag) return result;
  // Work on final display colors, after inverse/default resolution and other
  // corrections. Moving toward background lightness also dims dark text on a
  // light background; reducing lightness alone would increase its contrast.
  const auto background = srgb_to_oklch({result.background.red, result.background.green, result.background.blue});
  const auto dim = [&](boxrig_rgb value) {
    auto color = srgb_to_oklch({value.red, value.green, value.blue});
    color.lightness = background.lightness + .5 * (color.lightness - background.lightness);
    const auto adjusted = oklch_to_srgb(color);
    return boxrig_rgb{adjusted.red, adjusted.green, adjusted.blue};
  };
  result.foreground = dim(result.foreground);
  result.underline = result.underline_follows_foreground ? result.foreground : dim(result.underline);
  return result;
}
}

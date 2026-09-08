#include "indicator-color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace elder_terms {

GdkPixbuf *create_colored_indicator_pixbuf(const GdkPixbuf *source,
                                          const RgbColor &color) {
  if (source == nullptr) return nullptr;
  auto *result = gdk_pixbuf_copy(source);
  if (result == nullptr) return nullptr;
  const auto width = gdk_pixbuf_get_width(result);
  const auto height = gdk_pixbuf_get_height(result);
  const auto stride = gdk_pixbuf_get_rowstride(result);
  const auto channels = gdk_pixbuf_get_n_channels(result);
  auto *pixels = gdk_pixbuf_get_pixels(result);
  // A small neutral reflection keeps even black lamps distinguishable. Bright
  // highlights remain neutral, while the source brightness retains the relief.
  const std::array<double, 3> tint{
      0.2 + 0.8 * color.red / 255.0,
      0.2 + 0.8 * color.green / 255.0,
      0.2 + 0.8 * color.blue / 255.0};
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      auto *pixel = pixels + static_cast<std::size_t>(y) * stride +
                    static_cast<std::size_t>(x) * channels;
      const double brightness = std::max({pixel[0], pixel[1], pixel[2]});
      const double neutral = std::min({pixel[0], pixel[1], pixel[2]});
      const double reflection = std::clamp((neutral - 160.0) / 95.0, 0.0, 1.0);
      for (std::size_t channel = 0; channel < tint.size(); ++channel) {
        pixel[channel] = static_cast<guint8>(std::lround(
            brightness * (reflection + (1.0 - reflection) * tint[channel])));
      }
    }
  }
  return result;
}

} // namespace elder_terms

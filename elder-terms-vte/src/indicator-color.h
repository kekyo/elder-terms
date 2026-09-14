#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <elder-terms/settings/general-settings.h>

namespace elder_terms {

/**
 * Creates a tinted copy while retaining lamp shading and transparency.
 * @param source Original RGB or RGBA indicator image; never modified.
 * @param color Shared indicator tint.
 * @returns Owned pixbuf to release with g_object_unref(), or null on failure.
 */
GdkPixbuf *create_colored_indicator_pixbuf(const GdkPixbuf *source,
                                          const RgbColor &color);

} // namespace elder_terms

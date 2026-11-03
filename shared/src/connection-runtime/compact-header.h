// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <gtk/gtk.h>

namespace elder_terms {

/**
 * Installs title-bar styling that applies geometry changes atomically.
 *
 * @param header Header bar receiving the style class.
 * @param style_class CSS class identifying this kind of connection header.
 * @returns Owned screen provider; remove it from the screen and unref it before
 *          destroying the header.
 */
GtkCssProvider *create_compact_header_provider(
    GtkWidget *header, const char *style_class);

/**
 * Sets a connection title bar to 75% of its themed height or restores it.
 *
 * @param header Header bar whose children are visible before measurement.
 * @param provider Provider returned by create_compact_header_provider.
 * @param style_class CSS class used when creating the provider.
 * @param enabled True to compact the header and its buttons.
 * @remarks Icons and text retain their natural size. Reapplying the current
 *          mode does not recalculate the height from an already compact header.
 */
void set_compact_header_enabled(GtkWidget *header, GtkCssProvider *provider,
                                const char *style_class, bool enabled);

} // namespace elder_terms

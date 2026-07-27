#pragma once

#include <gtk/gtk.h>

#include <elder-terms/settings.h>

namespace elder_terms {

/**
 * Creates a CSS provider that paints one opaque RGB background.
 *
 * @param color RGB color to paint.
 * @param target_name Diagnostic name used when CSS parsing fails.
 * @returns Owned provider, or null when the CSS could not be loaded.
 */
GtkCssProvider *create_widget_background_provider(
    const RgbColor &color, const char *target_name);

/**
 * Adds one background provider to a widget and its application-owned children.
 *
 * @param widget Root widget whose tree receives the provider.
 * @param provider Background provider to add.
 */
void add_widget_tree_background_provider(
    GtkWidget *widget, GtkCssProvider *provider);

/**
 * Removes one background provider from a widget and its application-owned
 * children.
 *
 * @param widget Root widget whose tree loses the provider.
 * @param provider Background provider to remove.
 */
void remove_widget_tree_background_provider(
    GtkWidget *widget, GtkCssProvider *provider);

} // namespace elder_terms

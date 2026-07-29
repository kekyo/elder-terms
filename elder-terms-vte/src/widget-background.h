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
 * Creates a CSS provider for interactive surfaces derived from a background.
 *
 * @param color RGB content background used as the derivation source.
 * @param target_name Diagnostic name used when CSS parsing fails.
 * @returns Owned provider, or null when the CSS could not be loaded.
 * @remarks The derived color preserves hue and saturation while moving
 * lightness four percent toward the contrasting extreme.
 */
GtkCssProvider *create_widget_component_background_provider(
    const RgbColor &color, const char *target_name);

/**
 * Creates a component provider scoped below one style class.
 *
 * @param color RGB surface background used as the derivation source.
 * @param style_class Style class whose descendants receive component colors.
 * @param target_name Diagnostic name used when CSS parsing fails.
 * @returns Owned provider, or null when the CSS could not be loaded.
 * @remarks The provider can be registered for a screen without affecting
 * controls outside the classed surface, including children created later.
 */
GtkCssProvider *create_scoped_widget_component_background_provider(
    const RgbColor &color, const char *style_class,
    const char *target_name);

/**
 * Creates a screen provider for popup surfaces derived from a background.
 *
 * @param color RGB content background used as the derivation source.
 * @param target_name Diagnostic name used when CSS parsing fails.
 * @returns Owned provider, or null when the CSS could not be loaded.
 * @remarks The derived color uses the same four-percent lightness adjustment
 * as regular interactive controls.
 */
GtkCssProvider *create_widget_popup_component_background_provider(
    const RgbColor &color, const char *target_name);

/**
 * Creates a screen-safe CSS provider scoped below one style class.
 *
 * @param color RGB color to paint.
 * @param style_class Style class whose node and descendants are painted.
 * @param transparent_descendants_style_class Style class whose descendants
 * retain transparent CSS backgrounds for custom drawing, or null when every
 * descendant should be painted.
 * @param target_name Diagnostic name used when CSS parsing fails.
 * @returns Owned provider, or null when the CSS could not be loaded.
 */
GtkCssProvider *create_scoped_widget_background_provider(
    const RgbColor &color, const char *style_class,
    const char *transparent_descendants_style_class,
    const char *target_name);

/**
 * Creates a screen-safe CSS provider for one classed surface only.
 *
 * @param color RGB color to paint.
 * @param style_class Style class whose own node is painted.
 * @param target_name Diagnostic name used when CSS parsing fails.
 * @returns Owned provider, or null when the CSS could not be loaded.
 * @remarks Descendant controls retain their own component backgrounds.
 */
GtkCssProvider *create_scoped_widget_surface_background_provider(
    const RgbColor &color, const char *style_class,
    const char *target_name);

/**
 * Adds one background provider to a widget and all GTK children.
 *
 * @param widget Root widget whose tree receives the provider.
 * @param provider Background provider to add.
 * @remarks Internal children are included so compound controls are painted.
 */
void add_widget_tree_background_provider(
    GtkWidget *widget, GtkCssProvider *provider);

/**
 * Adds one background provider at an explicit priority to a widget tree.
 *
 * @param widget Root widget whose tree receives the provider.
 * @param provider Background provider to add.
 * @param priority GTK style provider priority used for every widget.
 * @remarks Internal children are included so compound controls are painted.
 */
void add_widget_tree_background_provider_at_priority(
    GtkWidget *widget, GtkCssProvider *provider, guint priority);

/**
 * Removes one background provider from a widget and all GTK children.
 *
 * @param widget Root widget whose tree loses the provider.
 * @param provider Background provider to remove.
 * @remarks Internal children are included so compound controls are restored.
 */
void remove_widget_tree_background_provider(
    GtkWidget *widget, GtkCssProvider *provider);

} // namespace elder_terms

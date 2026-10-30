// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <gtk/gtk.h>

#include <elder-terms/settings.h>

namespace elder_terms {

/**
 * Applies connection colors to a runtime settings dialog and its controls.
 * @param dialog Dialog owning the CSS resources, or null for no operation.
 * @param settings_root Root containing the settings pages and action buttons.
 * @param colors Effective connection colors; absent colors restore the theme.
 * @remarks Resources are replaced on Apply and released on dialog destruction.
 * Popup colors are supplied by the owning connection window.
 */
void set_settings_dialog_colors(GtkWidget *dialog, GtkWidget *settings_root,
    const GeneralColorSettings &colors);

/**
 * Selects black or white text for the greatest contrast with a background.
 *
 * @param background Opaque RGB surface color.
 * @returns Black or white RGB foreground color.
 * @remarks The comparison uses relative sRGB luminance and contrast ratios.
 */
RgbColor contrasting_foreground(const RgbColor &background);

/**
 * Keeps a terminal scrollbar distinguishable from its trough without resizing it.
 * @param scrollbar Native GTK scrollbar owned by its containing widget.
 * @param color Connection background, or no value to follow the GTK theme.
 * @remarks Enabled thumbs maintain at least 3:1 contrast. Theme changes are
 * observed automatically; CSS resources are released with the scrollbar.
 */
void set_terminal_scrollbar_background(GtkWidget *scrollbar,
    const std::optional<RgbColor> &color);

/**
 * Creates a CSS provider that paints one opaque RGB background and readable text.
 *
 * @param color RGB color to paint.
 * @param target_name Diagnostic name used when CSS parsing fails.
 * @returns Owned provider, or null when the CSS could not be loaded.
 * @remarks Selected descendants receive a stronger lightness adjustment so
 * selection remains visible when this provider overrides the GTK theme.
 * Text uses black or white according to each surface's contrast. Theme
 * background images on glyph nodes are preserved.
 */
GtkCssProvider *create_widget_background_provider(
    const RgbColor &color, const char *target_name);

/**
 * Creates a CSS provider for interactive surfaces and their text.
 *
 * @param color RGB content background used as the derivation source.
 * @param target_name Diagnostic name used when CSS parsing fails.
 * @returns Owned provider, or null when the CSS could not be loaded.
 * @remarks The derived color preserves hue and saturation while moving
 * lightness four percent toward the contrasting extreme. Foreground text
 * is selected from the derived surface color.
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
 * Creates a screen provider for popup surfaces and their text.
 *
 * @param color RGB content background used as the derivation source.
 * @param target_name Diagnostic name used when CSS parsing fails.
 * @returns Owned provider, or null when the CSS could not be loaded.
 * @remarks Regular surfaces use the same four-percent lightness adjustment
 * as other controls. Hovered and selected surfaces use a stronger adjustment
 * so their interaction state remains visible with custom colors.
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
 * Creates a screen-safe CSS provider for a classed surface and its direct labels.
 *
 * @param color RGB color to paint.
 * @param style_class Style class whose own node and direct labels are painted.
 * @param target_name Diagnostic name used when CSS parsing fails.
 * @returns Owned provider, or null when the CSS could not be loaded.
 * @remarks Direct labels share the surface color. Descendant controls retain
 * their own component backgrounds.
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

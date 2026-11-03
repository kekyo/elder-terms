// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "compact-header.h"

#include <algorithm>
#include <string>

namespace elder_terms {

static constexpr const char *compact_header_style_class = "compact-header";

static std::string header_transition_css(const char *style_class) {
  const std::string selector = std::string(".") + style_class;
  // Apply geometry changes atomically in both directions. Intermediate theme
  // sizes can otherwise become terminal resize requests. Keep visual transitions.
  return selector + ", " + selector + " button {"
      "transition-property: color, background, border-color, box-shadow,"
      " text-shadow, -gtk-icon-shadow, -gtk-icon-transform, opacity;"
      "}";
}

GtkCssProvider *create_compact_header_provider(
    GtkWidget *header, const char *style_class) {
  gtk_style_context_add_class(gtk_widget_get_style_context(header), style_class);
  auto *provider = gtk_css_provider_new();
  const std::string css = header_transition_css(style_class);
  gtk_css_provider_load_from_data(provider, css.c_str(), -1, nullptr);
  // A screen provider also reaches GTK-created window control buttons.
  gtk_style_context_add_provider_for_screen(
      gtk_widget_get_screen(header), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  return provider;
}

void set_compact_header_enabled(GtkWidget *header, GtkCssProvider *provider,
                                const char *style_class, bool enabled) {
  GtkStyleContext *context = gtk_widget_get_style_context(header);
  if (gtk_style_context_has_class(context, compact_header_style_class) == enabled) {
    return;
  }
  if (!enabled) {
    gtk_style_context_remove_class(context, compact_header_style_class);
    return;
  }

  int natural_height = 0;
  gtk_widget_get_preferred_height(header, nullptr, &natural_height);
  GtkBorder border{};
  gtk_style_context_get_border(
      context, gtk_style_context_get_state(context), &border);

  // Measure the unmodified theme with the indicators in place. GTK's CSS
  // minimum height excludes borders. Reset vertical margins as well as
  // padding: solid-CSD themes can use a negative top margin. The existing
  // icons and labels then fit without resampling or shrinking text.
  const int content_height = std::max(
      0, (natural_height * 3 + 2) / 4 - border.top - border.bottom);
  // Keep the standard 16px icons and equal padding on all sides. Reset both
  // dimensions and margins together so themed circular controls stay round.
  const std::string selector =
      std::string(".") + style_class + "." + compact_header_style_class;
  const std::string css = header_transition_css(style_class) + selector + " {"
      "min-height: " + std::to_string(content_height) + "px;"
      "padding-top: 0; padding-bottom: 0;"
      "margin-top: 0; margin-bottom: 0;"
      "} " + selector + " button {"
      "min-width: 16px; min-height: 16px;"
      "padding: 4px;"
      "margin: 0;"
      "}";
  gtk_css_provider_load_from_data(provider, css.c_str(), -1, nullptr);
  gtk_style_context_add_class(context, compact_header_style_class);
}

} // namespace elder_terms

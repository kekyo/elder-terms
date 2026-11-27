// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

#include "terminal-tab-row.h"
#include "terminal-tab-caption.h"
#include "../../shared/src/connection-runtime/titlebar-effects.h"
#include "../../shared/src/connection-runtime/widget-background.h"
#include <elder-terms/terminal-color-filter.h>
#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

struct TabPalette {
  MainWindow &page;
  GtkCssProvider *provider = gtk_css_provider_new();
  int last_color = -1;
  ~TabPalette() { g_object_unref(provider); }
};

static int packed(RgbColor color) {
  return color.red << 16 | color.green << 8 | color.blue;
}

static void update_palette(GtkWidget *row) {
  auto &palette = *static_cast<TabPalette *>(g_object_get_data(G_OBJECT(row), "tab-palette"));
  auto base = palette.page.colors.exterior_background;
  if (!base) base = sample_titlebar_background(palette.page.header_bar);
  if (!base || palette.last_color == packed(*base)) return;
  palette.last_color = packed(*base);
  auto lch = srgb_to_oklch(*base);
  lch.chroma *= .85;
  const auto hover = oklch_to_srgb(lch);
  lch.lightness *= .94;
  const auto normal = oklch_to_srgb(lch);
  auto *css = g_strdup_printf(
      "row.terminal-tab { background-color:#%06x; color:#%06x; }"
      "row.terminal-tab:hover { background-color:#%06x; color:#%06x; }",
      packed(normal), packed(contrasting_foreground(normal)), packed(hover), packed(contrasting_foreground(hover)));
  gtk_css_provider_load_from_data(palette.provider, css, -1, nullptr);
  g_free(css);
}

void update_terminal_tab_colors(MainWindow &page) {
  if (page.tab_row) update_palette(page.tab_row);
}

TerminalTabRow create_terminal_tab_row(MainWindow &page) {
  auto *row = gtk_list_box_row_new();
  auto *line = gtk_overlay_new();
  auto *pick = gtk_button_new();
  auto *caption = create_terminal_tab_caption(page.header_title_label);
  g_object_bind_property(page.header_title_label, "label", pick, "tooltip-text", G_BINDING_SYNC_CREATE);
  auto *contents = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  auto *icon = gtk_image_new();
  g_object_bind_property(page.header_connection_icon, "surface", icon, "surface", G_BINDING_SYNC_CREATE);
  gtk_box_pack_start(GTK_BOX(contents), icon, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(contents), caption, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(pick), contents);
  auto *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
  gtk_widget_set_tooltip_text(close, _("Close"));
  gtk_widget_set_halign(close, GTK_ALIGN_END);
  gtk_widget_set_opacity(close, 0);
  gtk_widget_set_sensitive(close, FALSE);
  gtk_container_add(GTK_CONTAINER(line), pick);
  gtk_overlay_add_overlay(GTK_OVERLAY(line), close);
  gtk_container_add(GTK_CONTAINER(row), line);
  gtk_style_context_add_class(gtk_widget_get_style_context(row), "terminal-tab");
  g_signal_connect(row, "state-flags-changed", G_CALLBACK(+[](GtkWidget *row, GtkStateFlags previous, gpointer data) {
    const auto flags = gtk_widget_get_state_flags(row);
    if (!((flags ^ previous) & GTK_STATE_FLAG_PRELIGHT)) return;
    auto *button = GTK_WIDGET(data);
    const bool hovered = flags & GTK_STATE_FLAG_PRELIGHT;
    // Keep the overlay allocated so a press arriving with the crossing event
    // hits Close immediately. Only a hovered row gives it input or text space.
    gtk_widget_set_sensitive(button, hovered);
    gtk_widget_set_opacity(button, hovered ? 1 : 0);
    int width = 0;
    gtk_widget_get_preferred_width(button, nullptr, &width);
    auto *pick = gtk_bin_get_child(GTK_BIN(gtk_widget_get_parent(button)));
    gtk_widget_set_margin_end(gtk_bin_get_child(GTK_BIN(pick)), hovered ? width : 0);
  }), close);
  auto *palette = new TabPalette{page};
  g_object_set_data_full(G_OBJECT(row), "tab-palette", palette,
      +[](gpointer data) { delete static_cast<TabPalette *>(data); });
  gtk_style_context_add_provider(gtk_widget_get_style_context(row), GTK_STYLE_PROVIDER(palette->provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  g_signal_connect_object(page.header_bar, "style-updated", G_CALLBACK(update_palette), row, G_CONNECT_SWAPPED);
  g_signal_connect(row, "map", G_CALLBACK(+[](GtkWidget *row, gpointer) { update_palette(row); }), nullptr);
  page.tab_row = row;
  update_palette(row);
  share_titlebar_effects_with_tab(page.header_bar, row);
  return {row, pick, close};
}
}

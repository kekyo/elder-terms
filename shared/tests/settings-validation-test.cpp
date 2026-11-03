// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings-widget.h>
#include <cassert>
#include <cstring>
#include <iostream>

static GtkWidget *find(GtkWidget *widget, const char *id) {
  if (std::strcmp(gtk_widget_get_name(widget), id) == 0) return widget;
  if (!GTK_IS_CONTAINER(widget)) return nullptr;
  auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
  GtkWidget *result = nullptr;
  for (auto *item = children; item && !result; item = item->next)
    result = find(GTK_WIDGET(item->data), id);
  g_list_free(children);
  return result;
}

static std::string tooltip(GtkWidget *widget) {
  auto *text = gtk_widget_get_tooltip_text(widget);
  const std::string result = text ? text : "";
  g_free(text);
  return result;
}

int main(int argc, char **argv) {
  using namespace elder_terms;
  gtk_init(&argc, &argv);
  SettingsWidgetOptions options;
  options.store = create_default_settings(default_terminal_display_settings(12), "Validation test");
  auto *state = create_settings_widget(std::move(options));
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *root = settings_widget_root(state);
  gtk_container_add(GTK_CONTAINER(window), root);
  g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    destroy_settings_widget(static_cast<SettingsWidgetState *>(data));
  }), state);
  auto *apply = find(root, "settings_apply_button");
  auto *width = find(root, "settings_terminal_width_entry");
  assert(width && apply);
  gtk_entry_set_text(GTK_ENTRY(width), "invalid");
  assert(!gtk_widget_get_sensitive(apply));
  assert(gtk_style_context_has_class(gtk_widget_get_style_context(width), GTK_STYLE_CLASS_ERROR));
  // The whole input must explain why Apply is blocked, including away from its icon.
  assert(!tooltip(width).empty());
  gtk_entry_set_text(GTK_ENTRY(width), "88");
  assert(gtk_widget_get_sensitive(apply));
  assert(!gtk_style_context_has_class(gtk_widget_get_style_context(width), GTK_STYLE_CLASS_ERROR));
  assert(tooltip(width).empty());

  auto *strength = find(root, "settings_terminal_color_vision_strength_entry");
  assert(strength);
  const auto original = tooltip(strength);
  gtk_entry_set_text(GTK_ENTRY(strength), "101");
  assert(!gtk_widget_get_sensitive(apply));
  assert(!tooltip(strength).empty());
  assert(original != tooltip(strength));
  gtk_entry_set_text(GTK_ENTRY(strength), "50");
  assert(gtk_widget_get_sensitive(apply));
  assert(original == tooltip(strength));

  gtk_widget_destroy(window);
  std::cout << "PASS field errors explain blocked settings and restore normal help\n";
}

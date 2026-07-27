#include "widget-background.h"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace elder_terms {

static std::string rgb_color_css(const RgbColor &color,
                                 const std::string &selector) {
  std::ostringstream stream;
  stream << selector << " { background-image: none; background-color: #"
         << std::uppercase << std::hex << std::setfill('0') << std::setw(2)
         << static_cast<unsigned int>(color.red) << std::setw(2)
         << static_cast<unsigned int>(color.green) << std::setw(2)
         << static_cast<unsigned int>(color.blue) << "; }";
  return stream.str();
}

static GtkCssProvider *create_background_provider(
    const RgbColor &color, const std::string &selector,
    const char *target_name) {
  GtkCssProvider *provider = gtk_css_provider_new();
  const std::string css = rgb_color_css(color, selector);
  GError *error = nullptr;
  if (gtk_css_provider_load_from_data(provider, css.c_str(), -1, &error)) {
    return provider;
  }

  std::cerr << "Failed to apply configured " << target_name
            << " background" << '\n';
  if (error != nullptr) {
    std::cerr << error->message << '\n';
    g_clear_error(&error);
  }
  g_object_unref(provider);
  return nullptr;
}

GtkCssProvider *create_widget_background_provider(
    const RgbColor &color, const char *target_name) {
  return create_background_provider(color, "*", target_name);
}

GtkCssProvider *create_scoped_widget_background_provider(
    const RgbColor &color, const char *style_class,
    const char *target_name) {
  const std::string class_selector = "." + std::string(style_class);
  return create_background_provider(
      color, class_selector + ", " + class_selector + " *", target_name);
}

static void add_widget_tree_background_provider_callback(
    GtkWidget *widget, gpointer data) {
  add_widget_tree_background_provider(
      widget, GTK_CSS_PROVIDER(data));
}

void add_widget_tree_background_provider(
    GtkWidget *widget, GtkCssProvider *provider) {
  if (widget == nullptr || provider == nullptr) {
    return;
  }

  gtk_style_context_add_provider(
      gtk_widget_get_style_context(widget),
      GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  if (GTK_IS_CONTAINER(widget)) {
    gtk_container_forall(
        GTK_CONTAINER(widget),
        add_widget_tree_background_provider_callback, provider);
  }
}

static void remove_widget_tree_background_provider_callback(
    GtkWidget *widget, gpointer data) {
  remove_widget_tree_background_provider(
      widget, GTK_CSS_PROVIDER(data));
}

void remove_widget_tree_background_provider(
    GtkWidget *widget, GtkCssProvider *provider) {
  if (widget == nullptr || provider == nullptr) {
    return;
  }

  gtk_style_context_remove_provider(
      gtk_widget_get_style_context(widget),
      GTK_STYLE_PROVIDER(provider));
  if (GTK_IS_CONTAINER(widget)) {
    gtk_container_forall(
        GTK_CONTAINER(widget),
        remove_widget_tree_background_provider_callback, provider);
  }
}

} // namespace elder_terms

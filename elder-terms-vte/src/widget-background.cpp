#include "widget-background.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace elder_terms {

static constexpr double component_lightness_delta = 0.04;
static constexpr double component_highlight_lightness_delta = 0.20;
static constexpr char component_background_selectors[] =
    "notebook > header > tabs > tab, "
    "button, "
    "button > box, "
    "button > label, "
    "button > image, "
    "button > box > label, "
    "button > box > image, "
    "entry, "
    "entry > image, "
    "spinbutton, "
    "spinbutton > entry, "
    "combobox, "
    "combobox > box, "
    "combobox > box > button, "
    "combobox > box > button > box, "
    "combobox > box > button > box > arrow, "
    "cellview, "
    "scrollbar > contents > trough > slider, "
    "progressbar > trough, "
    "scale > contents > trough, "
    "scale > contents > trough > slider, "
    "switch > slider";
static constexpr char popup_component_background_selectors[] =
    "window.popup, "
    "window.popup *, "
    "menu, "
    "menu > menuitem, "
    "menu > menuitem > box, "
    "menu > menuitem > label, "
    "menu > menuitem > box > label, "
    "popover, "
    "popover > contents, "
    "popover modelbutton, "
    "tooltip.background, "
    "tooltip.background > box, "
    "tooltip.background > box > label";
static constexpr char popup_component_highlight_selectors[] =
    "window.popup *:hover, "
    "window.popup *:hover *, "
    "window.popup *:selected, "
    "window.popup *:selected *, "
    "menu > menuitem:hover, "
    "menu > menuitem:hover > box, "
    "menu > menuitem:hover > label, "
    "menu > menuitem:hover > box > label, "
    "popover modelbutton:hover, "
    "popover modelbutton:hover > label";

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

static GtkCssProvider *create_css_provider(
    const std::string &css, const char *target_name) {
  GtkCssProvider *provider = gtk_css_provider_new();
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

static GtkCssProvider *create_background_provider(
    const RgbColor &color, const std::string &selector,
    const char *target_name) {
  return create_css_provider(
      rgb_color_css(color, selector), target_name);
}

static std::string scoped_descendant_selectors(
    const char *style_class, const char *selectors) {
  const std::string prefix =
      "." + std::string(style_class) + " ";
  std::string scoped = prefix + selectors;
  std::size_t separator = 0;
  while ((separator = scoped.find(", ", separator)) !=
         std::string::npos) {
    scoped.replace(separator, 2, ", " + prefix);
    separator += prefix.size() + 2;
  }
  return scoped;
}

static guint8 normalized_color_channel(double value) {
  return static_cast<guint8>(
      std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

static RgbColor derive_component_background(const RgbColor &color,
                                            double lightness_delta) {
  static constexpr double channel_maximum = 255.0;
  const double red =
      static_cast<double>(color.red) / channel_maximum;
  const double green =
      static_cast<double>(color.green) / channel_maximum;
  const double blue =
      static_cast<double>(color.blue) / channel_maximum;
  const double maximum = std::max(red, std::max(green, blue));
  const double minimum = std::min(red, std::min(green, blue));
  const double chroma = maximum - minimum;
  const double lightness = (maximum + minimum) / 2.0;

  double hue = 0.0;
  double saturation = 0.0;
  if (chroma > 0.0) {
    saturation =
        chroma / (1.0 - std::abs(2.0 * lightness - 1.0));
    if (maximum == red) {
      hue = 60.0 * std::fmod((green - blue) / chroma, 6.0);
    } else if (maximum == green) {
      hue = 60.0 * ((blue - red) / chroma + 2.0);
    } else {
      hue = 60.0 * ((red - green) / chroma + 4.0);
    }
    if (hue < 0.0) {
      hue += 360.0;
    }
  }

  const double component_lightness =
      lightness <= 0.5
          ? lightness +
                (1.0 - lightness) * lightness_delta
          : lightness * (1.0 - lightness_delta);
  const double component_chroma =
      (1.0 - std::abs(2.0 * component_lightness - 1.0)) *
      saturation;
  const double hue_sector = hue / 60.0;
  const double secondary =
      component_chroma *
      (1.0 - std::abs(std::fmod(hue_sector, 2.0) - 1.0));
  const double match = component_lightness - component_chroma / 2.0;

  double component_red = 0.0;
  double component_green = 0.0;
  double component_blue = 0.0;
  if (hue_sector < 1.0) {
    component_red = component_chroma;
    component_green = secondary;
  } else if (hue_sector < 2.0) {
    component_red = secondary;
    component_green = component_chroma;
  } else if (hue_sector < 3.0) {
    component_green = component_chroma;
    component_blue = secondary;
  } else if (hue_sector < 4.0) {
    component_green = secondary;
    component_blue = component_chroma;
  } else if (hue_sector < 5.0) {
    component_red = secondary;
    component_blue = component_chroma;
  } else {
    component_red = component_chroma;
    component_blue = secondary;
  }

  return {
      .red = normalized_color_channel(component_red + match),
      .green = normalized_color_channel(component_green + match),
      .blue = normalized_color_channel(component_blue + match),
  };
}

GtkCssProvider *create_widget_background_provider(
    const RgbColor &color, const char *target_name) {
  return create_background_provider(color, "*", target_name);
}

GtkCssProvider *create_widget_component_background_provider(
    const RgbColor &color, const char *target_name) {
  return create_background_provider(
      derive_component_background(color, component_lightness_delta),
      component_background_selectors, target_name);
}

GtkCssProvider *create_scoped_widget_component_background_provider(
    const RgbColor &color, const char *style_class,
    const char *target_name) {
  return create_background_provider(
      derive_component_background(color, component_lightness_delta),
      scoped_descendant_selectors(
          style_class, component_background_selectors),
      target_name);
}

GtkCssProvider *create_widget_popup_component_background_provider(
    const RgbColor &color, const char *target_name) {
  const RgbColor component_background =
      derive_component_background(color, component_lightness_delta);
  const RgbColor highlight_background =
      derive_component_background(
          color, component_highlight_lightness_delta);
  const std::string css =
      rgb_color_css(component_background,
                    popup_component_background_selectors) +
      "\n" +
      rgb_color_css(highlight_background,
                    popup_component_highlight_selectors);
  return create_css_provider(css, target_name);
}

GtkCssProvider *create_scoped_widget_background_provider(
    const RgbColor &color, const char *style_class,
    const char *transparent_descendants_style_class,
    const char *target_name) {
  const std::string class_selector = "." + std::string(style_class);
  std::string css = rgb_color_css(
      color, class_selector + ", " + class_selector + " *");
  if (transparent_descendants_style_class != nullptr) {
    css += class_selector + " ." +
           std::string(transparent_descendants_style_class) +
           " * { background-color: transparent; }";
  }
  return create_css_provider(css, target_name);
}

GtkCssProvider *create_scoped_widget_surface_background_provider(
    const RgbColor &color, const char *style_class,
    const char *target_name) {
  return create_background_provider(
      color, "." + std::string(style_class), target_name);
}

void add_widget_tree_background_provider(
    GtkWidget *widget, GtkCssProvider *provider) {
  add_widget_tree_background_provider_at_priority(
      widget, provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

struct WidgetTreeBackgroundProvider {
  GtkCssProvider *provider = nullptr;
  guint priority = GTK_STYLE_PROVIDER_PRIORITY_APPLICATION;
};

static void add_widget_tree_background_provider_callback(
    GtkWidget *widget, gpointer data) {
  auto *registration =
      static_cast<WidgetTreeBackgroundProvider *>(data);
  add_widget_tree_background_provider_at_priority(
      widget, registration->provider, registration->priority);
}

void add_widget_tree_background_provider_at_priority(
    GtkWidget *widget, GtkCssProvider *provider, guint priority) {
  if (widget == nullptr || provider == nullptr) {
    return;
  }

  gtk_style_context_add_provider(
      gtk_widget_get_style_context(widget),
      GTK_STYLE_PROVIDER(provider),
      priority);
  if (GTK_IS_CONTAINER(widget)) {
    WidgetTreeBackgroundProvider registration{
        .provider = provider,
        .priority = priority,
    };
    gtk_container_forall(
        GTK_CONTAINER(widget),
        add_widget_tree_background_provider_callback, &registration);
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

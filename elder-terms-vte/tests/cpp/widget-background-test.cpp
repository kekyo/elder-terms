#include "../../src/widget-background.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>

static std::uint32_t rendered_background(GtkWidget *widget) {
  auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
  auto *cr = cairo_create(surface);
  gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0, 16, 16);
  cairo_surface_flush(surface);
  const auto pixel = reinterpret_cast<const std::uint32_t *>(cairo_image_surface_get_data(surface))[8 * 16 + 8];
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  return pixel & 0xffffff;
}

int main(int argc, char **argv) {
  using namespace elder_terms;
  gtk_init(&argc, &argv);
  try {
    // A theme can draw a titlebar glyph with background-image, as PiXtrix does.
    auto *theme = gtk_css_provider_new();
    gtk_css_provider_load_from_data(theme,
        "* { transition: none; }"
        "button { background-image: linear-gradient(#ff0000, #ff0000); }"
        "button image { background-image: linear-gradient(#00ffff, #00ffff); }",
        -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(theme),
        GTK_STYLE_PROVIDER_PRIORITY_THEME + 1);
    for (int variant = 0; variant < 5; ++variant) {
      auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
      gtk_style_context_add_class(gtk_widget_get_style_context(window), "test-colors");
      if (variant == 4) gtk_style_context_add_class(gtk_widget_get_style_context(window), "popup");
      auto *button = gtk_button_new();
      auto *image = gtk_image_new();
      gtk_container_add(GTK_CONTAINER(button), image);
      auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      auto *notebook = gtk_notebook_new();
      auto *tab = gtk_label_new("General");
      gtk_notebook_append_page(GTK_NOTEBOOK(notebook), gtk_label_new("Page"), tab);
      gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(box), notebook, TRUE, TRUE, 0);
      gtk_container_add(GTK_CONTAINER(window), box);
      gtk_widget_show_all(window);
      const RgbColor color{0x61, 0x35, 0x83};
      auto *provider = variant == 0 ? create_widget_background_provider(color, "test")
          : variant == 1 ? create_widget_component_background_provider(color, "test")
          : variant == 2 ? create_scoped_widget_background_provider(color, "test-colors", nullptr, "test")
          : variant == 3 ? create_scoped_widget_component_background_provider(color, "test-colors", "test")
          : create_widget_popup_component_background_provider(color, "test");
      if (variant < 2) add_widget_tree_background_provider(window, provider);
      else gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      while (g_main_context_iteration(nullptr, FALSE)) {}
      const auto actual = rendered_background(image);
      if (actual != 0x00ffff) {
        throw std::runtime_error("Custom backgrounds must preserve theme glyph images: variant=" +
            std::to_string(variant) + " actual=" + std::to_string(actual));
      }
      if (rendered_background(button) == 0xff0000) {
        throw std::runtime_error("Custom surfaces must replace theme background gradients");
      }
      if ((variant == 1 || variant == 3) &&
          rendered_background(tab) != 0x68398c) {
        throw std::runtime_error("Notebook tab labels must use the configured component surface: variant=" +
            std::to_string(variant) + " actual=" + std::to_string(rendered_background(tab)));
      }
      if (variant >= 2) gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider));
      gtk_widget_destroy(window);
      g_object_unref(provider);
    }
    gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(theme));
    g_object_unref(theme);
    std::cout << "widget-background-test: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "widget-background-test: FAIL: " << error.what() << '\n';
    return 1;
  }
}

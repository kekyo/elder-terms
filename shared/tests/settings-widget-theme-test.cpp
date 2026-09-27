#include <elder-terms/settings-widget.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <utility>

static GtkWidget *find_notebook(GtkWidget *widget) {
  if (GTK_IS_NOTEBOOK(widget)) return widget;
  if (!GTK_IS_CONTAINER(widget)) return nullptr;
  auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
  GtkWidget *found = nullptr;
  for (auto *child = children; child != nullptr && found == nullptr; child = child->next) {
    found = find_notebook(GTK_WIDGET(child->data));
  }
  g_list_free(children);
  return found;
}

static GtkWidget *find_label(GtkWidget *widget) {
  if (GTK_IS_LABEL(widget)) return widget;
  if (GTK_IS_BIN(widget)) return find_label(gtk_bin_get_child(GTK_BIN(widget)));
  return nullptr;
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  try {
    // Reproduce the flat tab-button styling used by both light and dark themes.
    auto *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,
        "* { transition: none; }"
        "notebook > header tab { color: #707070; }"
        "notebook > header tab:checked { color: #2e3436; }"
        "notebook > header tab button.flat { color: alpha(currentColor, 0.3); }",
        -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_THEME + 1);
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    elder_terms::SettingsWidgetOptions options;
    options.store = elder_terms::create_default_settings({}, "Theme test");
    auto *state = elder_terms::create_settings_widget(std::move(options));
    g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
      elder_terms::destroy_settings_widget(static_cast<elder_terms::SettingsWidgetState *>(data));
    }), state);
    gtk_container_add(GTK_CONTAINER(window), elder_terms::settings_widget_root(state));
    gtk_widget_show_all(window);
    auto *notebook = GTK_NOTEBOOK(find_notebook(elder_terms::settings_widget_root(state)));
    if (notebook == nullptr) throw std::runtime_error("Settings must expose a notebook");
    for (int selected : {0, 1, 0}) {
      gtk_notebook_set_current_page(notebook, selected);
      while (g_main_context_iteration(nullptr, FALSE)) {}
      for (int page = 0; page < gtk_notebook_get_n_pages(notebook); ++page) {
        auto *tab = gtk_notebook_get_tab_label(notebook, gtk_notebook_get_nth_page(notebook, page));
        if (!gtk_widget_get_visible(tab)) continue;
        auto *label = find_label(tab);
        if (label == nullptr) throw std::runtime_error("Tab text must remain accessible");
        GdkRGBA color{};
        auto *context = gtk_widget_get_style_context(label);
        gtk_style_context_get_color(context, gtk_style_context_get_state(context), &color);
        if (std::abs(color.alpha - 1.0) > 0.001) {
          throw std::runtime_error("Enabled tab text must retain the theme's full opacity");
        }
      }
    }
    gtk_widget_destroy(window);
    gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider));
    g_object_unref(provider);
    std::cout << "settings-widget-theme-test: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "settings-widget-theme-test: FAIL: " << error.what() << '\n';
    return 1;
  }
}

#include <filesystem>
#include <iostream>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gestament/gtk.h>

#include "main-window.h"

namespace elder_terms {

static constexpr int indicator_icon_pixel_size = 18;
static constexpr const char *indicator_off_icon_file_name = "green-off.png";

static std::filesystem::path executable_directory() {
  const std::filesystem::path executable_path =
      std::filesystem::read_symlink("/proc/self/exe");
  return executable_path.parent_path();
}

static std::filesystem::path ui_path() {
  return executable_directory() / "main-window.ui";
}

static std::filesystem::path indicator_icon_path(const char *file_name) {
  return executable_directory() / file_name;
}

static GtkWidget *required_widget(GtkBuilder *builder, const char *id) {
  GObject *object = gtk_builder_get_object(builder, id);
  if (object == nullptr || !GTK_IS_WIDGET(object)) {
    std::cerr << "Missing GTK widget: " << id << '\n';
    return nullptr;
  }

  return GTK_WIDGET(object);
}

static bool main_window_has_required_widgets(const MainWindow &main_window) {
  return main_window.window != nullptr && main_window.header_bar != nullptr &&
         main_window.settings_button != nullptr &&
         main_window.root_box != nullptr &&
         main_window.terminal_scroller != nullptr &&
         main_window.terminal != nullptr &&
         main_window.terminal_scrollbar != nullptr &&
         main_window.status_bar != nullptr &&
         main_window.status_label != nullptr &&
         main_window.activity_indicator_bar != nullptr &&
         main_window.sd_indicator_box != nullptr &&
         main_window.sd_indicator_image != nullptr &&
         main_window.sd_indicator_label != nullptr &&
         main_window.rd_indicator_box != nullptr &&
         main_window.rd_indicator_image != nullptr &&
         main_window.rd_indicator_label != nullptr;
}

static bool load_indicator_image(GtkWidget *image,
                                 const std::filesystem::path &path) {
  GError *error = nullptr;
  GdkPixbuf *pixbuf =
      gdk_pixbuf_new_from_file_at_scale(path.c_str(),
                                        indicator_icon_pixel_size,
                                        indicator_icon_pixel_size, TRUE,
                                        &error);
  if (pixbuf == nullptr) {
    std::cerr << "Failed to load indicator image: " << path << '\n';
    if (error != nullptr) {
      std::cerr << error->message << '\n';
      g_clear_error(&error);
    }
    return false;
  }

  gtk_image_set_from_pixbuf(GTK_IMAGE(image), pixbuf);
  g_object_unref(pixbuf);
  return true;
}

static bool load_indicator_images(const MainWindow &main_window) {
  const std::filesystem::path off_icon_path =
      indicator_icon_path(indicator_off_icon_file_name);
  return load_indicator_image(main_window.sd_indicator_image, off_icon_path) &&
         load_indicator_image(main_window.rd_indicator_image, off_icon_path);
}

std::optional<MainWindow> load_main_window() {
  MainWindow main_window;
  GError *error = nullptr;
  main_window.builder = gtk_builder_new();
  const std::filesystem::path builder_file = ui_path();
  if (!gtk_builder_add_from_file(main_window.builder, builder_file.c_str(),
                                 &error)) {
    std::cerr << "Failed to load UI file: " << builder_file << '\n';
    if (error != nullptr) {
      std::cerr << error->message << '\n';
      g_clear_error(&error);
    }
    release_main_window(&main_window);
    return std::nullopt;
  }

  gestament_gtk_assign_accessible_ids_from_builder(main_window.builder);

  main_window.window = required_widget(main_window.builder, "main_window");
  main_window.header_bar = required_widget(main_window.builder, "header_bar");
  main_window.settings_button =
      required_widget(main_window.builder, "settings_button");
  main_window.root_box = required_widget(main_window.builder, "root_box");
  main_window.terminal_scroller =
      required_widget(main_window.builder, "terminal_scroller");
  main_window.terminal =
      required_widget(main_window.builder, "terminal_view");
  main_window.terminal_scrollbar =
      required_widget(main_window.builder, "terminal_scrollbar");
  main_window.status_bar = required_widget(main_window.builder, "status_bar");
  main_window.status_label =
      required_widget(main_window.builder, "status_label");
  main_window.activity_indicator_bar =
      required_widget(main_window.builder, "activity_indicator_bar");
  main_window.sd_indicator_box =
      required_widget(main_window.builder, "sd_indicator_box");
  main_window.sd_indicator_image =
      required_widget(main_window.builder, "sd_indicator_image");
  main_window.sd_indicator_label =
      required_widget(main_window.builder, "sd_indicator_label");
  main_window.rd_indicator_box =
      required_widget(main_window.builder, "rd_indicator_box");
  main_window.rd_indicator_image =
      required_widget(main_window.builder, "rd_indicator_image");
  main_window.rd_indicator_label =
      required_widget(main_window.builder, "rd_indicator_label");
  if (!main_window_has_required_widgets(main_window)) {
    release_main_window(&main_window);
    return std::nullopt;
  }
  if (!load_indicator_images(main_window)) {
    release_main_window(&main_window);
    return std::nullopt;
  }

  return main_window;
}

void release_main_window(MainWindow *main_window) {
  if (main_window == nullptr) {
    return;
  }

  if (main_window->builder != nullptr) {
    g_object_unref(main_window->builder);
  }
  *main_window = {};
}

} // namespace elder_terms

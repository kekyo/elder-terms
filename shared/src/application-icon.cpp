#include <array>
#include <iostream>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>

#include <elder-terms/application-icon.h>

namespace elder_terms {

static constexpr std::array application_icon_resource_paths{
    "/net/kekyo/elder-terms/icons/16x16.png",
    "/net/kekyo/elder-terms/icons/22x22.png",
    "/net/kekyo/elder-terms/icons/24x24.png",
    "/net/kekyo/elder-terms/icons/32x32.png",
    "/net/kekyo/elder-terms/icons/48x48.png",
    "/net/kekyo/elder-terms/icons/64x64.png",
    "/net/kekyo/elder-terms/icons/128x128.png",
    "/net/kekyo/elder-terms/icons/256x256.png",
};

bool initialize_application_window_icon() {
  GList *icons = nullptr;
  for (const char *resource_path : application_icon_resource_paths) {
    GError *error = nullptr;
    GdkPixbuf *icon =
        gdk_pixbuf_new_from_resource(resource_path, &error);
    if (icon == nullptr) {
      std::cerr << "Warning: failed to load bundled application icon "
                << resource_path << ": "
                << (error == nullptr ? "unknown error" : error->message)
                << '\n';
      g_clear_error(&error);
      g_list_free_full(icons, g_object_unref);
      return false;
    }
    icons = g_list_append(icons, icon);
  }

  gtk_window_set_default_icon_list(icons);
  g_list_free_full(icons, g_object_unref);
  return true;
}

} // namespace elder_terms

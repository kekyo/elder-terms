// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-background.h"
#include "terminal-runtime.h"
#include <boxrig/gtk3/background_effect.h>
#include <vector>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

struct TerminalBackground {
  MainWindow *window;
  GtkCssProvider *provider;
  GdkScreen *screen;
  std::vector<GtkWidget *> paths;
  TerminalBackgroundSettings settings{TerminalBackgroundMode::opaque, 80, {0, 0, 0}, 0,
      TerminalCellBackgroundMode::transparent};
  RgbColor backing{0, 0, 0};
  bool desktop_transparent = false;
  TerminalBackgroundImagePtr image;
  TerminalBackgroundImageSettings image_settings{"", 100, TerminalBackgroundImageFit::cover};
  GCancellable *image_cancel = nullptr;
  std::size_t image_generation = 0;
  std::shared_ptr<TerminalBackground *> owner;
  std::string image_error;
  TerminalBackgroundImageLoader loader;
  boxrig_gtk3_background_effect *effect = nullptr;
  boxrig_ui_background_effect_request effect_request = BOXRIG_UI_BACKGROUND_EFFECT_NONE;
  boxrig_ui_background_effect_rectangle effect_rectangle{};
};

static void select_visual(GtkWidget *window, GdkScreen *, gpointer) {
  if (gtk_widget_get_realized(window)) return;
  if (auto *visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(window)))
    gtk_widget_set_visual(window, visual);
}

void prepare_terminal_background_window(GtkWidget *window) {
  select_visual(window, nullptr, nullptr);
  g_signal_connect(window, "screen-changed", G_CALLBACK(select_visual), nullptr);
  gtk_widget_set_app_paintable(window, TRUE);
  g_signal_connect(window, "draw", G_CALLBACK(+[](GtkWidget *, cairo_t *cr, gpointer) {
    // Replace the previous frame's alpha too when toggling back to transparency.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
  }), nullptr);
}

static void update_opaque_region(GtkWidget *window, gpointer) {
  if (auto *surface = gtk_widget_get_window(window)) gdk_window_set_opaque_region(surface, nullptr);
}

static gboolean draw_backing(GtkWidget *root, cairo_t *cr, gpointer data) {
  const auto *background = static_cast<TerminalBackground *>(data);
  auto *terminal = GTK_WIDGET(terminal_runtime_widget(background->window->runtime));
  gint x = 0, y = 0;
  if (!terminal || !gtk_widget_translate_coordinates(terminal, root, 0, 0, &x, &y)) return FALSE;
  GdkRGBA chrome{0.94, 0.94, 0.94, 1};
  gtk_style_context_lookup_color(gtk_widget_get_style_context(root), "theme_bg_color", &chrome);
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgb(cr, chrome.red, chrome.green, chrome.blue);
  cairo_rectangle(cr, 0, 0, gtk_widget_get_allocated_width(root), gtk_widget_get_allocated_height(root));
  cairo_fill(cr);
  const auto &color = background->backing;
  cairo_set_source_rgba(cr, color.red / 255.0, color.green / 255.0, color.blue / 255.0,
      background->desktop_transparent ? 0 : 1);
  cairo_rectangle(cr, x, y, gtk_widget_get_allocated_width(terminal), gtk_widget_get_allocated_height(terminal));
  cairo_fill(cr);
  cairo_restore(cr);
  return FALSE;
}

static bool update_effect(TerminalBackground *background, TerminalBackgroundMode mode) {
  const auto request = mode == TerminalBackgroundMode::blur ? BOXRIG_UI_BACKGROUND_EFFECT_PREFER :
      BOXRIG_UI_BACKGROUND_EFFECT_NONE;
  if (!background->effect) return request == BOXRIG_UI_BACKGROUND_EFFECT_NONE;
  boxrig_ui_background_effect_rectangle rectangle{};
  auto *terminal = GTK_WIDGET(terminal_runtime_widget(background->window->runtime));
  if (request != BOXRIG_UI_BACKGROUND_EFFECT_NONE && terminal &&
      gtk_widget_translate_coordinates(terminal, background->window->window, 0, 0, &rectangle.x, &rectangle.y)) {
    rectangle.width = gtk_widget_get_allocated_width(terminal);
    rectangle.height = gtk_widget_get_allocated_height(terminal);
  }
  const auto &previous = background->effect_rectangle;
  if (request == background->effect_request && rectangle.x == previous.x && rectangle.y == previous.y &&
      rectangle.width == previous.width && rectangle.height == previous.height) return true;
  const bool area = rectangle.width > 0 && rectangle.height > 0;
  if (boxrig_gtk3_background_effect_set(background->effect, request, area ? &rectangle : nullptr, area ? 1 : 0) != BOXRIG_UI_OK)
    return false;
  background->effect_request = request;
  background->effect_rectangle = rectangle;
  return true;
}

static bool apply_native(TerminalBackground *background, const TerminalBackgroundSettings &settings,
    RgbColor backing, TerminalBackgroundImagePtr image, const TerminalBackgroundImageSettings &image_settings, bool reattach) {
  auto *widget = terminal_runtime_widget(background->window->runtime);
  auto *view = boxrig_gtk3_terminal_widget_view(widget);
  const bool requested = settings.mode != TerminalBackgroundMode::opaque;
  if (!update_effect(background, settings.mode)) return false;
  auto config = boxrig_ui_transparency_config_default();
  const bool surface_alpha = requested || image;
  config.request = surface_alpha ? BOXRIG_UI_TRANSPARENCY_PREFERRED : BOXRIG_UI_TRANSPARENCY_OPAQUE;
  config.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
  config.background_opacity = surface_alpha ? static_cast<uint8_t>((settings.opacity * 255 + 50) / 100) : 255;
  boxrig_ui_transparency_state previous{};
  if (boxrig_gtk3_terminal_widget_get_transparency(widget, &previous) != BOXRIG_UI_OK ||
      boxrig_gtk3_terminal_widget_set_transparency(widget, &config) != BOXRIG_UI_OK) {
    update_effect(background, background->settings.mode);
    return false;
  }
  const boxrig_ui_rgba tint{settings.tint.red, settings.tint.green, settings.tint.blue,
      static_cast<uint8_t>((settings.tint_opacity * 255 + 50) / 100)};
  const auto previous_cell_mode = boxrig_ui_view_get_cell_background_mode(view);
  const auto cell_mode = settings.cell_mode == TerminalCellBackgroundMode::opaque
      ? BOXRIG_UI_CELL_BACKGROUND_OPAQUE_EXPLICIT : BOXRIG_UI_CELL_BACKGROUND_TRANSPARENT;
  if (boxrig_ui_view_set_cell_background_mode(view, cell_mode) != BOXRIG_UI_OK) {
    boxrig_gtk3_terminal_widget_set_transparency(widget, &previous.config);
    update_effect(background, background->settings.mode);
    return false;
  }
  const auto previous_tint = boxrig_ui_view_get_background_color(view);
  if (boxrig_ui_view_set_background_color(view, tint) != BOXRIG_UI_OK) {
    boxrig_ui_view_set_cell_background_mode(view, previous_cell_mode);
    boxrig_gtk3_terminal_widget_set_transparency(widget, &previous.config);
    update_effect(background, background->settings.mode);
    return false;
  }
  if (reattach || image != background->image || image_settings.opacity != background->image_settings.opacity ||
      image_settings.fit != background->image_settings.fit) {
    boxrig_ui_background_image native{};
    if (image) {
      native = {image->width, image->height, image->width * 4, image->rgba.size(), image->rgba.data(),
          static_cast<uint8_t>((image_settings.opacity * 255 + 50) / 100),
          image_settings.fit == TerminalBackgroundImageFit::contain ? BOXRIG_UI_BACKGROUND_CONTAIN :
          image_settings.fit == TerminalBackgroundImageFit::stretch ? BOXRIG_UI_BACKGROUND_STRETCH : BOXRIG_UI_BACKGROUND_COVER};
    }
    if (boxrig_ui_view_set_background_image(view, image ? &native : nullptr) != BOXRIG_UI_OK) {
      boxrig_ui_view_set_cell_background_mode(view, previous_cell_mode);
      boxrig_ui_view_set_background_color(view, previous_tint);
      boxrig_gtk3_terminal_widget_set_transparency(widget, &previous.config);
      update_effect(background, background->settings.mode);
      return false;
    }
  }
  background->settings = settings;
  background->backing = backing;
  background->image = std::move(image);
  background->image_settings = image_settings;
  boxrig_ui_transparency_state actual{};
  boxrig_gtk3_terminal_widget_get_transparency(widget, &actual);
  auto *window = background->window->window;
  background->desktop_transparent = requested && actual.effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA &&
      gdk_screen_is_composited(background->screen) &&
      gtk_widget_get_visual(window) == gdk_screen_get_rgba_visual(background->screen);
  update_opaque_region(window, nullptr);
  gtk_widget_queue_draw(window);
  terminal_runtime_refresh(background->window->runtime);
  return true;
}

static void clear_paths(TerminalBackground *background) {
  for (auto *widget : background->paths) {
    auto *context = gtk_widget_get_style_context(widget);
    gtk_style_context_remove_provider(context, GTK_STYLE_PROVIDER(background->provider));
    gtk_style_context_remove_class(context, "terminal-background-path");
    g_object_unref(widget);
  }
  background->paths.clear();
}

void attach_terminal_background(TerminalBackground *background) {
  clear_paths(background);
  auto *widget = GTK_WIDGET(terminal_runtime_widget(background->window->runtime));
  for (auto *parent = gtk_widget_get_parent(widget); parent; parent = gtk_widget_get_parent(parent)) {
    background->paths.push_back(GTK_WIDGET(g_object_ref(parent)));
    auto *context = gtk_widget_get_style_context(parent);
    gtk_style_context_add_class(context, "terminal-background-path");
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(background->provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 4);
    if (parent == background->window->window) break;
  }
  if (!apply_native(background, background->settings, background->backing, background->image, background->image_settings, true))
    g_printerr("Failed to restore terminal background\n");
}

TerminalBackground *create_terminal_background(MainWindow *window, TerminalBackgroundImageLoader loader) {
  auto *background = new TerminalBackground{};
  background->window = window;
  background->provider = gtk_css_provider_new();
  background->screen = GDK_SCREEN(g_object_ref(gtk_widget_get_screen(window->window)));
  background->loader = std::move(loader);
  background->owner = std::make_shared<TerminalBackground *>(background);
  boxrig_gtk3_background_effect_create(GTK_WINDOW(window->window), +[](void *data) {
    // The adapter retains requests across map/unmap and capability changes.
    // Refresh status through the normal frame notification, never destroy here.
    auto *background = static_cast<TerminalBackground *>(data);
    terminal_runtime_refresh(background->window->runtime);
  }, background, &background->effect);
  gtk_css_provider_load_from_data(background->provider,
      ".terminal-background-path { background-color: transparent; background-image: none; }", -1, nullptr);
  g_signal_connect(window->root_box, "draw", G_CALLBACK(draw_backing), background);
  g_signal_connect_after(window->root_box, "size-allocate", G_CALLBACK(+[](GtkWidget *, GtkAllocation *, gpointer data) {
    auto *background = static_cast<TerminalBackground *>(data);
    if (!update_effect(background, background->settings.mode)) g_printerr("Failed to resize terminal background effect\n");
  }), background);
  g_signal_connect(window->window, "style-updated", G_CALLBACK(update_opaque_region), background);
  g_signal_connect(window->window, "realize", G_CALLBACK(update_opaque_region), background);
  g_signal_connect(background->screen, "composited-changed", G_CALLBACK(+[](GdkScreen *, gpointer data) {
    auto *background = static_cast<TerminalBackground *>(data);
    if (!apply_native(background, background->settings, background->backing, background->image, background->image_settings, false))
      g_printerr("Failed to update terminal compositor state\n");
  }), background);
  attach_terminal_background(background);
  return background;
}

bool apply_terminal_background(TerminalBackground *background, const SettingsStore &settings, TerminalBackgroundImagePtr image) {
  return apply_native(background, terminal_background_settings(settings),
      general_color_settings(settings).background.value_or(RgbColor{0, 0, 0}), std::move(image), terminal_background_image_settings(settings), false);
}

TerminalBackgroundImagePtr terminal_background_image(const TerminalBackground *background) {
  return background->image;
}

void cancel_terminal_background_image(TerminalBackground *background) {
  if (!background) return;
  ++background->image_generation;
  if (background->image_cancel) g_cancellable_cancel(background->image_cancel);
  g_clear_object(&background->image_cancel);
}

void prepare_terminal_background_image(TerminalBackground *background, const SettingsStore &settings,
    std::function<bool()> current, std::function<void(TerminalBackgroundImagePtr, std::string)> complete) {
  cancel_terminal_background_image(background);
  const auto path = terminal_background_image_settings(settings).path;
  if ((background->image && background->image->path == path) || path.empty()) {
    if (current()) {
      background->image_error.clear();
      complete(path.empty() ? nullptr : background->image, {});
    }
    return;
  }
  background->image_cancel = g_cancellable_new();
  const auto generation = background->image_generation;
  background->loader(path, background->image_cancel,
      [owner = background->owner, generation, current = std::move(current), complete = std::move(complete)]
      (TerminalBackgroundImagePtr image, std::string error) {
        auto *background = *owner;
        if (!background || background->image_generation != generation) return;
        g_clear_object(&background->image_cancel);
        if (!current()) return;
        background->image_error = error;
        complete(std::move(image), std::move(error));
      });
}

std::string format_terminal_background_status(TerminalBackgroundMode mode, bool transparent,
    const boxrig_ui_background_effect_state &effect) {
  if (mode == TerminalBackgroundMode::opaque) return _("Opaque background");
  if (!transparent) return _("Opaque background: desktop transparency is unavailable");
  if (mode != TerminalBackgroundMode::blur) return _("Transparent background");
  if (effect.accepted && effect.reason == BOXRIG_UI_BACKGROUND_EFFECT_ACCEPTED) return _("Frosted glass requested");
  if (effect.reason == BOXRIG_UI_BACKGROUND_EFFECT_PENDING) return _("Transparent background: checking frosted glass support");
  if (effect.reason == BOXRIG_UI_BACKGROUND_EFFECT_FAILED) return _("Transparent background: frosted glass request failed");
  return _("Transparent background: frosted glass is unavailable");
}

std::string terminal_background_status(const TerminalBackground *background) {
  if (!background) return {};
  std::string status = format_terminal_background_status(background->settings.mode, background->desktop_transparent,
      boxrig_gtk3_background_effect_get_state(background->effect));
  if (!background->image_error.empty()) status += "\n" + std::string(_("Background image")) + ": " + background->image_error;
  return status;
}

void destroy_terminal_background(TerminalBackground *background) {
  if (!background) return;
  *background->owner = nullptr;
  cancel_terminal_background_image(background);
  boxrig_gtk3_background_effect_destroy(background->effect);
  g_signal_handlers_disconnect_by_data(background->window->root_box, background);
  g_signal_handlers_disconnect_by_data(background->window->window, background);
  g_signal_handlers_disconnect_by_data(background->screen, background);
  clear_paths(background);
  g_object_unref(background->screen);
  g_object_unref(background->provider);
  delete background;
}
}

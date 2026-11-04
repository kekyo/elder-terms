// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "titlebar-effects.h"
#include <algorithm>

namespace elder_terms {

struct TitlebarEffects {
  GtkWidget *header;
  TitlebarEffectState state;
  guint tick = 0;
  TitlebarEffectStrength bell_strength = TitlebarEffectStrength::medium;
  TitlebarEffectStrength title_strength = TitlebarEffectStrength::medium;
  bool destroyed = false;
};

static constexpr const char *effects_key = "elder-terms-titlebar-effects";

static TitlebarEffects *effects_for(GtkWidget *header) {
  return header ? static_cast<TitlebarEffects *>(g_object_get_data(G_OBJECT(header), effects_key)) : nullptr;
}

static bool can_animate(const TitlebarEffects &effects) {
  if (effects.destroyed || !gtk_widget_get_mapped(effects.header)) return false;
  gboolean animations = TRUE;
  g_object_get(gtk_widget_get_settings(effects.header), "gtk-enable-animations", &animations, nullptr);
  auto *window = gtk_widget_get_window(gtk_widget_get_toplevel(effects.header));
  return animations && window && !(gdk_window_get_state(window) & GDK_WINDOW_STATE_ICONIFIED);
}

static void stop(TitlebarEffects &effects) {
  if (effects.tick) gtk_widget_remove_tick_callback(effects.header, effects.tick);
  effects.tick = 0;
  reset_titlebar_effect(effects.state);
  gtk_widget_queue_draw(effects.header);
}

static gboolean tick(GtkWidget *header, GdkFrameClock *clock, gpointer context) {
  auto &effects = *static_cast<TitlebarEffects *>(context);
  const bool active = can_animate(effects) &&
      sample_titlebar_effect(effects.state, gdk_frame_clock_get_frame_time(clock)).active;
  gtk_widget_queue_draw(header);
  if (active) return G_SOURCE_CONTINUE;
  effects.tick = 0;
  if (!can_animate(effects)) reset_titlebar_effect(effects.state);
  return G_SOURCE_REMOVE;
}

static void start(TitlebarEffects &effects) {
  if (!effects.tick && sample_titlebar_effect(effects.state, g_get_monotonic_time()).active)
    effects.tick = gtk_widget_add_tick_callback(effects.header, tick, &effects, nullptr);
  gtk_widget_queue_draw(effects.header);
}

static std::optional<double> background_extreme(GtkStyleContext *style, int width, int height) {
  // Sample the rendered background instead of the deprecated GTK background
  // color getter, which also omits theme background images/gradients.
  auto *pixel = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  auto *probe = cairo_create(pixel);
  std::optional<double> extreme;
  if (cairo_status(probe) == CAIRO_STATUS_SUCCESS) {
    gtk_render_background(style, probe, -width / 2.0, -height / 2.0, width, height);
    cairo_surface_flush(pixel);
    const auto *data = cairo_image_surface_get_data(pixel);
    if (cairo_status(probe) == CAIRO_STATUS_SUCCESS && data) {
      const auto color = *reinterpret_cast<const guint32 *>(data);
      const auto lightness = .2126 * ((color >> 16) & 255) + .7152 * ((color >> 8) & 255) + .0722 * (color & 255);
      extreme = lightness < 128 ? 1 : 0;
    }
  }
  cairo_destroy(probe);
  cairo_surface_destroy(pixel);
  return extreme;
}

void draw_titlebar_effect_frame(GtkWidget *header, cairo_t *cr, const TitlebarEffectFrame &frame) {
  const auto width = gtk_widget_get_allocated_width(header);
  const auto height = gtk_widget_get_allocated_height(header);
  auto *style = gtk_widget_get_style_context(header);
  // Render the current theme, including backdrop gradients and rounded corners.
  // The same background alpha masks the effect, so no rectangular corner leaks.
  cairo_push_group(cr);
  gtk_render_background(style, cr, 0, 0, width, height);
  auto *background = cairo_pop_group(cr);
  cairo_set_source(cr, background);
  cairo_paint(cr);
  if (frame.title > 0 || frame.bell > 0) {
    // A failed allocation skips only the effect. Never pass an error pattern
    // into GTK's context, which would prevent the title and buttons from drawing.
    if (const auto extreme = background_extreme(style, width, height)) {
      auto *band = cairo_pattern_create_linear(0, 0, std::max(1, width), 0);
      for (int index = 0; index <= 96; ++index) {
        const double x = index / 96.0;
        cairo_pattern_add_color_stop_rgba(band, x, *extreme, *extreme, *extreme,
            titlebar_effect_opacity(frame, x));
      }
      if (cairo_pattern_status(band) == CAIRO_STATUS_SUCCESS) {
        cairo_set_source(cr, band);
        cairo_mask(cr, background);
      }
      cairo_pattern_destroy(band);
    }
  }
  cairo_pattern_destroy(background);
  gtk_render_frame(style, cr, 0, 0, width, height);
  struct Children { GtkWidget *header; cairo_t *cr; } children{header, cr};
  gtk_container_forall(GTK_CONTAINER(header), +[](GtkWidget *child, gpointer data) {
    const auto &children = *static_cast<Children *>(data);
    gtk_container_propagate_draw(GTK_CONTAINER(children.header), child, children.cr);
  }, &children);
}

void install_titlebar_effects(GtkWidget *header) {
  if (!header || effects_for(header)) return;
  auto *effects = new TitlebarEffects{header, {}};
  g_object_set_data_full(G_OBJECT(header), effects_key, effects,
      +[](gpointer data) { delete static_cast<TitlebarEffects *>(data); });
  g_signal_connect(header, "draw", G_CALLBACK(+[](GtkWidget *header, cairo_t *cr, gpointer data) -> gboolean {
    auto &effects = *static_cast<TitlebarEffects *>(data);
    if (!can_animate(effects)) return FALSE;
    auto *clock = gtk_widget_get_frame_clock(header);
    const auto frame = sample_titlebar_effect(effects.state, gdk_frame_clock_get_frame_time(clock));
    if (!frame.active) return FALSE;
    draw_titlebar_effect_frame(header, cr, scale_titlebar_effect(frame, effects.bell_strength, effects.title_strength));
    return TRUE;
  }), effects);
  g_signal_connect(header, "unmap", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    stop(*static_cast<TitlebarEffects *>(data));
  }), effects);
  g_signal_connect(header, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    auto &effects = *static_cast<TitlebarEffects *>(data);
    stop(effects);
    effects.destroyed = true;
  }), effects);
  g_signal_connect_object(gtk_widget_get_settings(header), "notify::gtk-enable-animations",
      G_CALLBACK(+[](GObject *, GParamSpec *, gpointer header) {
        if (auto *effects = effects_for(GTK_WIDGET(header)); effects && !can_animate(*effects)) stop(*effects);
      }), header, G_CONNECT_DEFAULT);
  g_signal_connect_object(gtk_widget_get_toplevel(header), "window-state-event",
      G_CALLBACK(+[](GtkWidget *, GdkEventWindowState *event, gpointer header) -> gboolean {
        if (event->new_window_state & GDK_WINDOW_STATE_ICONIFIED)
          if (auto *effects = effects_for(GTK_WIDGET(header))) stop(*effects);
        return FALSE;
      }), header, G_CONNECT_DEFAULT);
}

void set_titlebar_effect_strengths(GtkWidget *header, TitlebarEffectStrength bell, TitlebarEffectStrength title) {
  auto *effects = effects_for(header);
  if (!effects || effects->destroyed) return;
  effects->bell_strength = bell;
  effects->title_strength = title;
  if (bell == TitlebarEffectStrength::disabled) effects->state.bell_start.reset();
  if (title == TitlebarEffectStrength::disabled) {
    effects->state.cycle_start.reset();
    effects->state.fade_start.reset();
    effects->state.changed = false;
  }
  if (!sample_titlebar_effect(effects->state, g_get_monotonic_time()).active) stop(*effects);
  gtk_widget_queue_draw(header);
}

void notify_titlebar_title(GtkWidget *header, const std::string &title) {
  auto *effects = effects_for(header);
  if (!effects || effects->destroyed) return;
  notify_titlebar_title(effects->state, title, g_get_monotonic_time(),
      effects->title_strength != TitlebarEffectStrength::disabled && can_animate(*effects));
  start(*effects);
}

void notify_titlebar_bell(GtkWidget *header) {
  auto *effects = effects_for(header);
  if (!effects || effects->bell_strength == TitlebarEffectStrength::disabled || !can_animate(*effects)) return;
  notify_titlebar_bell(effects->state, g_get_monotonic_time());
  start(*effects);
}

bool titlebar_effects_running(GtkWidget *header) {
  const auto *effects = effects_for(header);
  return effects && effects->tick;
}

} // namespace elder_terms

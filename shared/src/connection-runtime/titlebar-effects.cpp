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
  GtkWidget *tab = nullptr;
  GtkWidget *ticker = nullptr;
  guint tick = 0;
  TitlebarEffectStrength bell_strength = TitlebarEffectStrength::medium;
  TitlebarEffectStrength receive_strength = TitlebarEffectStrength::medium;
  bool destroyed = false;
};

static constexpr const char *effects_key = "elder-terms-titlebar-effects";

static TitlebarEffects *effects_for(GtkWidget *header) {
  return header ? static_cast<TitlebarEffects *>(g_object_get_data(G_OBJECT(header), effects_key)) : nullptr;
}

static GtkWidget *animation_surface(const TitlebarEffects &effects) {
  if (effects.destroyed) return nullptr;
  // The row remains mapped while the selected GtkStack header changes.
  auto *surface = effects.tab && gtk_widget_get_mapped(effects.tab) ? effects.tab : effects.header;
  if (!gtk_widget_get_mapped(surface)) return nullptr;
  gboolean animations = TRUE;
  g_object_get(gtk_widget_get_settings(surface), "gtk-enable-animations", &animations, nullptr);
  auto *window = gtk_widget_get_window(gtk_widget_get_toplevel(surface));
  return animations && window && !(gdk_window_get_state(window) & GDK_WINDOW_STATE_ICONIFIED) ? surface : nullptr;
}

static void redraw(const TitlebarEffects &effects) {
  if (!effects.destroyed && gtk_widget_get_mapped(effects.header)) gtk_widget_queue_draw(effects.header);
  if (effects.tab && gtk_widget_get_mapped(effects.tab)) gtk_widget_queue_draw(effects.tab);
}

static void remove_tick(TitlebarEffects &effects) {
  if (effects.tick) gtk_widget_remove_tick_callback(effects.ticker, effects.tick);
  effects.tick = 0;
  effects.ticker = nullptr;
}

static gboolean tick(GtkWidget *, GdkFrameClock *clock, gpointer data) {
  auto &effects = *static_cast<TitlebarEffects *>(data);
  if (!animation_surface(effects)) reset_titlebar_effect(effects.state);
  redraw(effects);
  if (sample_titlebar_effect(effects.state, gdk_frame_clock_get_frame_time(clock)).active) return G_SOURCE_CONTINUE;
  effects.tick = 0;
  effects.ticker = nullptr;
  return G_SOURCE_REMOVE;
}

static void update_clock(TitlebarEffects &effects) {
  auto *surface = animation_surface(effects);
  if (!surface) reset_titlebar_effect(effects.state);
  const bool active = sample_titlebar_effect(effects.state, g_get_monotonic_time()).active;
  if (!active || surface != effects.ticker) remove_tick(effects);
  if (active && !effects.tick) {
    effects.ticker = surface;
    effects.tick = gtk_widget_add_tick_callback(surface, tick, &effects, nullptr);
  }
  redraw(effects);
}

std::optional<RgbColor> sample_titlebar_background(GtkWidget *widget) {
  auto *style = gtk_widget_get_style_context(widget);
  // A stacked, unselected header can still have its initial 1x1 allocation.
  const int width = std::max(32, gtk_widget_get_allocated_width(widget));
  const int height = std::max(24, gtk_widget_get_allocated_height(widget));
  // Sample the rendered background instead of the deprecated GTK background
  // color getter, which also omits theme background images/gradients.
  auto *pixel = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  auto *probe = cairo_create(pixel);
  std::optional<RgbColor> result;
  if (cairo_status(probe) == CAIRO_STATUS_SUCCESS) {
    GdkRGBA underlay{};
    if (gtk_style_context_lookup_color(style, "theme_bg_color", &underlay)) {
      gdk_cairo_set_source_rgba(probe, &underlay);
      cairo_paint(probe);
    }
    gtk_render_background(style, probe, -width / 2.0, -height / 2.0, width, height);
    cairo_surface_flush(pixel);
    const auto *data = cairo_image_surface_get_data(pixel);
    if (cairo_status(probe) == CAIRO_STATUS_SUCCESS && data) {
      const auto color = *reinterpret_cast<const guint32 *>(data);
      result = RgbColor{static_cast<guint8>(color >> 16), static_cast<guint8>(color >> 8), static_cast<guint8>(color)};
    }
  }
  cairo_destroy(probe);
  cairo_surface_destroy(pixel);
  return result;
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
  if (frame.receive > 0 || frame.bell > 0) {
    // A failed allocation skips only the effect. Never pass an error pattern
    // into GTK's context, which would prevent the title and buttons from drawing.
    if (const auto color = sample_titlebar_background(header)) {
      const double extreme = .2126 * color->red + .7152 * color->green + .0722 * color->blue < 128 ? 1 : 0;
      auto *band = cairo_pattern_create_linear(0, 0, std::max(1, width), 0);
      for (int index = 0; index <= 96; ++index) {
        const double x = index / 96.0;
        cairo_pattern_add_color_stop_rgba(band, x, extreme, extreme, extreme,
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

static void bind_surface(GtkWidget *surface, GtkWidget *header) {
  g_signal_connect_object(surface, "draw", G_CALLBACK(+[](GtkWidget *surface, cairo_t *cr, GtkWidget *header) -> gboolean {
    auto &effects = *effects_for(header);
    if (!effects.tick) return FALSE;
    auto *clock = gtk_widget_get_frame_clock(surface);
    if (!clock) return FALSE;
    const auto frame = sample_titlebar_effect(effects.state, gdk_frame_clock_get_frame_time(clock));
    if (!frame.active) return FALSE;
    draw_titlebar_effect_frame(surface, cr, scale_titlebar_effect(frame, effects.bell_strength, effects.receive_strength));
    return TRUE;
  }), header, G_CONNECT_DEFAULT);
  for (const auto *signal : {"map", "unmap"})
    g_signal_connect_object(surface, signal, G_CALLBACK(+[](GtkWidget *, GtkWidget *header) {
      update_clock(*effects_for(header));
    }), header, G_CONNECT_AFTER);
}

static gboolean window_state_changed(GtkWidget *, GdkEventWindowState *event, GtkWidget *header) {
  if (event->changed_mask & GDK_WINDOW_STATE_ICONIFIED) update_clock(*effects_for(header));
  return FALSE;
}

static void bind_window(GtkWidget *header, GtkWidget *previous, gpointer) {
  if (GTK_IS_WINDOW(previous))
    g_signal_handlers_disconnect_by_func(previous, reinterpret_cast<gpointer>(window_state_changed), header);
  auto *window = gtk_widget_get_toplevel(header);
  if (GTK_IS_WINDOW(window))
    g_signal_connect_object(window, "window-state-event", G_CALLBACK(window_state_changed), header, G_CONNECT_DEFAULT);
}

void install_titlebar_effects(GtkWidget *header) {
  if (!header || effects_for(header)) return;
  auto *effects = new TitlebarEffects{header, {}};
  g_object_set_data_full(G_OBJECT(header), effects_key, effects,
      +[](gpointer data) { delete static_cast<TitlebarEffects *>(data); });
  bind_surface(header, header);
  g_signal_connect(header, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    auto &effects = *static_cast<TitlebarEffects *>(data);
    effects.destroyed = true;
    update_clock(effects);
  }), effects);
  g_signal_connect_object(gtk_widget_get_settings(header), "notify::gtk-enable-animations",
      G_CALLBACK(+[](GObject *, GParamSpec *, GtkWidget *header) { update_clock(*effects_for(header)); }),
      header, G_CONNECT_DEFAULT);
  g_signal_connect(header, "hierarchy-changed", G_CALLBACK(bind_window), nullptr);
  bind_window(header, nullptr, nullptr);
}

void share_titlebar_effects_with_tab(GtkWidget *header, GtkWidget *row) {
  auto *effects = effects_for(header);
  if (!effects || effects->tab || !row) return;
  effects->tab = row;
  bind_surface(row, header);
  g_signal_connect_object(row, "destroy", G_CALLBACK(+[](GtkWidget *, GtkWidget *header) {
    auto &effects = *effects_for(header);
    effects.tab = nullptr;
    update_clock(effects);
  }), header, G_CONNECT_DEFAULT);
  update_clock(*effects);
}

void set_titlebar_effect_strengths(GtkWidget *header, TitlebarEffectStrength bell, TitlebarEffectStrength receive) {
  auto *effects = effects_for(header);
  if (!effects || effects->destroyed) return;
  effects->bell_strength = bell;
  effects->receive_strength = receive;
  if (bell == TitlebarEffectStrength::disabled) effects->state.bell_start.reset();
  if (receive == TitlebarEffectStrength::disabled) {
    effects->state.cycle_start.reset();
    effects->state.fade_start.reset();
    effects->state.received = false;
  }
  update_clock(*effects);
}

void notify_titlebar_receive(GtkWidget *header) {
  auto *effects = effects_for(header);
  if (!effects || effects->receive_strength == TitlebarEffectStrength::disabled || !animation_surface(*effects)) return;
  notify_titlebar_receive(effects->state, g_get_monotonic_time());
  update_clock(*effects);
}

void notify_titlebar_bell(GtkWidget *header) {
  auto *effects = effects_for(header);
  if (!effects || effects->bell_strength == TitlebarEffectStrength::disabled || !animation_surface(*effects)) return;
  notify_titlebar_bell(effects->state, g_get_monotonic_time());
  update_clock(*effects);
}

bool titlebar_effects_running(GtkWidget *header) {
  const auto *effects = effects_for(header);
  return effects && effects->tick;
}

} // namespace elder_terms

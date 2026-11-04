// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../../shared/src/connection-runtime/titlebar-effects.h"
#include "../../../shared/src/connection-runtime/compact-header.h"
#include "../../../shared/src/connection-runtime/widget-background.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace elder_terms;

struct Row {
  GtkWidget *header = nullptr;
  GtkBuilder *builder = nullptr;
  GtkCssProvider *compact = nullptr;
  GtkCssProvider *components = nullptr;
  TitlebarEffectState state;
  TitlebarEffectStrength strength = TitlebarEffectStrength::weak;
  bool light = false;
  std::int64_t now = 0;
};

struct Demo {
  GtkWidget *window;
  std::array<Row, 9> rows;
  std::string directory;
  std::int64_t start = 0;
  std::int64_t elapsed = 0;
  int event = 0;
  int frame = 0;
  std::FILE *trace = nullptr;
};

static void title(Demo &demo, const char *text) {
  for (auto &row : demo.rows) {
    const auto label = std::string(row.strength == TitlebarEffectStrength::weak ? "Weak" :
        row.strength == TitlebarEffectStrength::medium ? "Medium" : "Strong") + " | " + text;
    gtk_header_bar_set_title(GTK_HEADER_BAR(row.header), label.c_str());
  }
}

static void receive(Demo &demo, const char *text) {
  title(demo, text);
  for (auto &row : demo.rows) notify_titlebar_receive(row.state, row.now);
}

static gboolean animate(GtkWidget *, GdkFrameClock *clock, gpointer data) {
  auto &demo = *static_cast<Demo *>(data);
  const auto time = gdk_frame_clock_get_frame_time(clock);
  if (!demo.start) demo.start = time;
  demo.elapsed = time - demo.start;
  for (auto &row : demo.rows) row.now = demo.elapsed;
  if (demo.event == 0 && demo.elapsed >= 300000) { receive(demo, "Received data"); demo.event++; }
  if (demo.event == 1 && demo.elapsed >= 500000) { receive(demo, "Received more data"); demo.event++; }
  if (demo.event == 2 && demo.elapsed >= 600000) { receive(demo, "Received data"); demo.event++; }
  if (demo.event == 3 && demo.elapsed >= 900000) {
    for (auto &row : demo.rows) notify_titlebar_bell(row.state, row.now);
    demo.event++;
  }
  // A repeat merges into the existing flash instead of restarting it.
  if (demo.event == 4 && demo.elapsed >= 1000000) {
    for (auto &row : demo.rows) notify_titlebar_bell(row.state, row.now);
    demo.event++;
  }
  gtk_widget_queue_draw(demo.window);
  return G_SOURCE_CONTINUE;
}

static void after_paint(GdkFrameClock *, gpointer data) {
  auto &demo = *static_cast<Demo *>(data);
  if (demo.directory.empty()) return;
  auto *pixels = gdk_pixbuf_get_from_window(gtk_widget_get_window(demo.window), 0, 0,
      gtk_widget_get_allocated_width(demo.window), gtk_widget_get_allocated_height(demo.window));
  if (!pixels) g_error("Cannot capture comparison frame");
  char name[64];
  std::snprintf(name, sizeof(name), "/frame-%04d.png", demo.frame++);
  GError *error = nullptr;
  if (!gdk_pixbuf_save(pixels, (demo.directory + name).c_str(), "png", &error, nullptr))
    g_error("Cannot save comparison frame: %s", error->message);
  g_object_unref(pixels);
  std::fprintf(demo.trace, "%d,%lld,%lld,%d\n", demo.frame - 1,
      static_cast<long long>(demo.elapsed), static_cast<long long>(g_get_monotonic_time()), demo.event);
  if (demo.elapsed >= 4200000) gtk_main_quit();
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  Demo demo{};
  if (argc == 2) {
    demo.directory = argv[1];
    std::filesystem::create_directories(demo.directory);
    demo.trace = std::fopen((demo.directory + "/frames.csv").c_str(), "w");
    if (!demo.trace) return 1;
    std::fputs("frame,elapsed_us,captured_us,event\n", demo.trace);
  }
  demo.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(demo.window), "Titlebar effects: Weak / Medium / Strong");
  gtk_window_set_default_size(GTK_WINDOW(demo.window), 760, 500);
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_add(GTK_CONTAINER(demo.window), box);
  auto *description = gtk_label_new("Weak: original intensity   Medium: 1.5x (default)   Strong: 2x\n"
      "Dark, light and connection colors; normal and compact headers. Final fade: 120 ms.");
  gtk_box_pack_start(GTK_BOX(box), description, FALSE, FALSE, 8);
  for (int index = 0; index < 9; ++index) {
    auto &row = demo.rows[index];
    row.strength = std::array{TitlebarEffectStrength::weak, TitlebarEffectStrength::medium, TitlebarEffectStrength::strong}[index % 3];
    row.light = index / 3 == 1;
    row.builder = gtk_builder_new_from_file("elder-terms-terminal/src/main-window.ui");
    row.header = GTK_WIDGET(gtk_builder_get_object(row.builder, "header_bar"));
    g_object_ref(row.header);
    gtk_window_set_titlebar(GTK_WINDOW(gtk_builder_get_object(row.builder, "main_window")), nullptr);
    gtk_box_pack_start(GTK_BOX(box), row.header, FALSE, FALSE, 0);
    g_object_unref(row.header);
    gtk_image_set_from_icon_name(GTK_IMAGE(gtk_builder_get_object(row.builder, "header_connection_icon")),
        "utilities-terminal-symbolic", GTK_ICON_SIZE_BUTTON);
    auto *provider = gtk_css_provider_new();
    const std::string color = row.light ? "#eeeeee" : index / 3 == 2 ? "#245b76" : "#202020";
    const auto css = "headerbar { background-image: none; background-color: " + color +
        "; color: " + (row.light ? "black" : "white") + "; }";
    gtk_css_provider_load_from_data(provider, css.c_str(), -1, nullptr);
    gtk_style_context_add_provider(gtk_widget_get_style_context(row.header), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
    g_object_unref(provider);
    const auto scope = "comparison-row-" + std::to_string(index);
    gtk_style_context_add_class(gtk_widget_get_style_context(row.header), scope.c_str());
    row.components = create_scoped_widget_component_background_provider(
        *parse_rgb_color(color), scope.c_str(), "comparison controls");
    gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(row.header),
        GTK_STYLE_PROVIDER(row.components), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 3);
    if (index % 2) {
      const auto scope = "comparison-compact-" + std::to_string(index);
      row.compact = create_compact_header_provider(row.header, scope.c_str());
      set_compact_header_enabled(row.header, row.compact, scope.c_str(), true);
    }
    g_signal_connect(row.header, "draw", G_CALLBACK(+[](GtkWidget *header, cairo_t *cr, gpointer data) -> gboolean {
      auto &row = *static_cast<Row *>(data);
      const auto frame = sample_titlebar_effect(row.state, row.now);
      draw_titlebar_effect_frame(header, cr, scale_titlebar_effect(frame, row.strength, row.strength));
      return TRUE;
    }), &row);
  }
  title(demo, "Initial title");
  auto *controls = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
  for (const char *label : {"BEL", "Receive data", "Restart comparison"}) {
    auto *button = gtk_button_new_with_label(label);
    gtk_container_add(GTK_CONTAINER(controls), button);
    g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton *button, gpointer data) {
      auto &demo = *static_cast<Demo *>(data);
      const std::string label = gtk_button_get_label(button);
      if (label == "BEL") for (auto &row : demo.rows) notify_titlebar_bell(row.state, row.now);
      else if (label == "Receive data") receive(demo, std::to_string(demo.elapsed).c_str());
      else {
        demo.start = 0; demo.event = 0;
        for (auto &row : demo.rows) { row.state = {}; row.now = 0; }
        title(demo, "Initial title");
      }
    }), &demo);
  }
  gtk_box_pack_start(GTK_BOX(box), controls, FALSE, FALSE, 8);
  g_signal_connect(demo.window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer) {
    if (gtk_main_level()) gtk_main_quit();
  }), nullptr);
  gtk_widget_show_all(demo.window);
  if (!demo.directory.empty()) {
    auto *geometry = std::fopen((demo.directory + "/rows.csv").c_str(), "w");
    if (!geometry) return 1;
    for (const auto &row : demo.rows) {
      gint x = 0, y = 0;
      gtk_widget_translate_coordinates(row.header, demo.window, 0, 0, &x, &y);
      std::fprintf(geometry, "%d,%d,%d,%d\n", x, y,
          gtk_widget_get_allocated_width(row.header), gtk_widget_get_allocated_height(row.header));
    }
    std::fclose(geometry);
  }
  const auto tick = gtk_widget_add_tick_callback(demo.window, animate, &demo, nullptr);
  auto *clock = gtk_widget_get_frame_clock(demo.window);
  const auto painted = g_signal_connect_after(clock, "after-paint", G_CALLBACK(after_paint), &demo);
  gtk_main();
  g_signal_handler_disconnect(clock, painted);
  gtk_widget_remove_tick_callback(demo.window, tick);
  if (demo.trace) std::fclose(demo.trace);
  for (auto &row : demo.rows) {
    gtk_style_context_remove_provider_for_screen(gtk_widget_get_screen(row.header), GTK_STYLE_PROVIDER(row.components));
    g_object_unref(row.components);
    if (row.compact) {
      gtk_style_context_remove_provider_for_screen(gtk_widget_get_screen(row.header), GTK_STYLE_PROVIDER(row.compact));
      g_object_unref(row.compact);
    }
    g_object_unref(row.builder);
  }
  gtk_widget_destroy(demo.window);
}

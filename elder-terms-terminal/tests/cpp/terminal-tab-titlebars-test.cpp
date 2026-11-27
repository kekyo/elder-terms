// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

#include "../../src/terminal-tabs.h"
#include "../../src/terminal-runtime.h"
#include "../../../shared/src/connection-runtime/titlebar-effects.h"
#include <elder-terms/connection-icon-image.h>
#include <gdk/gdkx.h>
#include <cassert>
#include <cstdio>

using namespace elder_terms;

static GtkWidget *descendant(GtkWidget *widget, GType type) {
  if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type)) return widget;
  if (!GTK_IS_CONTAINER(widget)) return nullptr;
  auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
  GtkWidget *found = nullptr;
  for (auto *child = children; child && !found; child = child->next)
    found = descendant(GTK_WIDGET(child->data), type);
  g_list_free(children);
  return found;
}

static void paint(GtkWidget *widget) {
  auto *clock = gtk_widget_get_frame_clock(widget);
  bool done = false;
  auto signal = g_signal_connect_after(clock, "after-paint", G_CALLBACK(+[](GdkFrameClock *, gpointer data) {
    *static_cast<bool *>(data) = true;
  }), &done);
  gtk_widget_queue_draw(widget);
  while (!done) g_main_context_iteration(nullptr, TRUE);
  g_signal_handler_disconnect(clock, signal);
}

static GdkPoint window_point(GtkWidget *widget, int x, int y) {
  GdkPoint point{};
  assert(gtk_widget_translate_coordinates(widget, gtk_widget_get_toplevel(widget), x, y, &point.x, &point.y));
  return point;
}

static void point_at(GtkWidget *widget, int x, int y) {
  const auto point = window_point(widget, x, y);
  auto *native = gtk_widget_get_window(gtk_widget_get_toplevel(widget));
  auto *display = GDK_WINDOW_XDISPLAY(native);
  XWarpPointer(display, None, GDK_WINDOW_XID(native), 0, 0, 0, 0, point.x, point.y);
  XSync(display, False);
  paint(widget);
}

static unsigned pixel(GtkWidget *widget, int x, int y) {
  const auto point = window_point(widget, x, y);
  auto *native = gtk_widget_get_window(gtk_widget_get_toplevel(widget));
  auto *image = gdk_pixbuf_get_from_window(native, point.x, point.y, 1, 1);
  assert(image);
  const auto *rgb = gdk_pixbuf_read_pixels(image);
  const unsigned color = (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
  g_object_unref(image);
  return color;
}

// Encode exactly the native window pixels used for the frame-by-frame checks.
struct TabVideo {
  GtkWidget *window;
  int width;
  GdkPixbuf *frame = nullptr;
  GSubprocess *encoder = nullptr;

  TabVideo(GtkWidget *window, const char *path) : window(window), width(gtk_widget_get_allocated_width(window) & ~3) {
    next();
    if (path) {
      const auto size = std::to_string(width) + "x160";
      encoder = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE, nullptr, "ffmpeg", "-nostdin", "-y", "-loglevel", "error",
          "-f", "rawvideo", "-framerate", "60", "-pixel_format", gdk_pixbuf_get_has_alpha(frame) ? "rgba" : "rgb24",
          "-video_size", size.c_str(), "-i", "pipe:0", "-c:v", "ffv1", "-threads", "1", path, nullptr);
      assert(encoder);
    }
  }
  void next() {
    paint(window);
    g_clear_object(&frame);
    frame = gdk_pixbuf_get_from_window(gtk_widget_get_window(window), 0, 0, width, 160);
    assert(frame);
    if (encoder) assert(g_output_stream_write_all(g_subprocess_get_stdin_pipe(encoder),
        gdk_pixbuf_read_pixels(frame), gdk_pixbuf_get_byte_length(frame), nullptr, nullptr, nullptr));
  }
  int red(GdkPoint point) const {
    assert(point.x >= 0 && point.x < width && point.y >= 0 && point.y < 160);
    return gdk_pixbuf_read_pixels(frame)[point.y * gdk_pixbuf_get_rowstride(frame) +
        point.x * gdk_pixbuf_get_n_channels(frame)];
  }
  ~TabVideo() {
    g_clear_object(&frame);
    if (encoder) {
      assert(g_output_stream_close(g_subprocess_get_stdin_pipe(encoder), nullptr, nullptr));
      assert(g_subprocess_wait_check(encoder, nullptr, nullptr));
      g_object_unref(encoder);
    }
  }
};

static GdkPoint band_point(GtkWidget *surface, int percent) {
  return window_point(surface, gtk_widget_get_allocated_width(surface) * percent / 100, 5);
}

// Compare glyph shapes in the recording against short labels rendered by GTK
// with the same font. This also distinguishes the last word from an ellipsis.
static std::string glyphs(const TabVideo &video, GtkWidget *caption, int x, int width) {
  const auto point = window_point(caption, x, 0);
  std::string mask;
  for (int y = 0; y < gtk_widget_get_allocated_height(caption); ++y)
    for (int dx = 0; dx < width; ++dx)
      mask.push_back(video.red({point.x + dx, point.y + y}) > 190);
  return mask;
}

static bool same_glyphs(const std::string &sample, const std::string &reference) {
  if (sample.size() != reference.size()) return false;
  // The hover background can put an antialiased edge across the ink threshold.
  int differences = 0;
  for (size_t i = 0; i < sample.size(); ++i)
    if (sample[i] != reference[i] && ++differences > 3) return false;
  return true;
}

static void check_second_row_scroll(MainWindow &page, TabVideo &video) {
  auto *caption = descendant(page.tab_row, GTK_TYPE_LABEL);
  constexpr int sample_width = 32;
  set_main_window_title(&page, "Building");
  video.next();
  const auto head = glyphs(video, caption, 0, sample_width);
  assert(head.find(char{1}) != std::string::npos);
  set_main_window_title(&page, "finished");
  video.next();
  int tail_width;
  pango_layout_get_pixel_size(gtk_label_get_layout(GTK_LABEL(caption)), &tail_width, nullptr);
  const auto tail = glyphs(video, caption, tail_width - sample_width, sample_width);

  set_main_window_title(&page, "Building the terminal application / finished");
  video.next();
  assert(same_glyphs(glyphs(video, caption, 0, sample_width), head));
  const unsigned char output[] = {'x'};
  note_main_window_output(&page, output);
  const int width = gtk_widget_get_allocated_width(caption);
  const auto deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  do {
    video.next();
    assert(glyphs(video, caption, 0, width).find(char{1}) != std::string::npos);
    if (same_glyphs(glyphs(video, caption, width - sample_width, sample_width), tail)) return;
  } while (g_get_monotonic_time() < deadline);
  assert(false && "the end of the title must be visible on the second row");
}

static void check_title_scroll(MainWindow &page, TerminalTab *tab, TabVideo &video, GtkWidget *caption) {
  constexpr int sample_width = 32;
  auto *settings = gtk_widget_get_settings(caption);
  auto *paned = GTK_PANED(gtk_widget_get_ancestor(caption, GTK_TYPE_PANED));
  const int sidebar = gtk_paned_get_position(paned);
  auto width = [caption] { return gtk_widget_get_allocated_width(caption); };
  const std::string first = "Compile application sources and libraries / BUILD-READY";
  const std::string skipped = "Review intermediate changes and artifacts / UNUSED";
  const std::string latest = "Deploy current application to staging / DEPLOY-DONE";
  set_titlebar_effect_strengths(page.header_bar, TitlebarEffectStrength::disabled, TitlebarEffectStrength::disabled);
  g_object_set(settings, "gtk-enable-animations", FALSE, nullptr);
  set_main_window_title(&page, latest);
  video.next();
  const auto idle = glyphs(video, caption, 0, width());
  auto reference = [&](const char *text, bool at_end) {
    set_main_window_title(&page, text);
    video.next();
    int span;
    pango_layout_get_pixel_size(gtk_label_get_layout(GTK_LABEL(caption)), &span, nullptr);
    assert(span >= sample_width);
    return glyphs(video, caption, at_end ? span - sample_width : 0, sample_width);
  };
  const auto first_head = reference("Compile", false);
  const auto first_tail = reference("BUILD-READY", true);
  const auto next_head = reference("Deploy", false);
  const auto next_tail = reference("DEPLOY-DONE", true);
  const auto skipped_head = reference("Review", false);
  g_object_set(settings, "gtk-enable-animations", TRUE, nullptr);
  set_main_window_title(&page, first);
  video.next();
  assert(glyphs(video, caption, 0, sample_width) == first_head);
  bool changed = false, finished_first = false, started_latest = false, finished_latest = false;
  const auto deadline = g_get_monotonic_time() + 12 * G_TIME_SPAN_SECOND;
  do {
    video.next();
    const auto head = glyphs(video, caption, 0, sample_width);
    const auto tail = glyphs(video, caption, width() - sample_width, sample_width);
    assert(!same_glyphs(head, skipped_head));
    if (!changed && !same_glyphs(head, first_head)) {
      changed = true;
      set_main_window_title(&page, skipped);
      set_main_window_title(&page, latest);
      auto *pick = gtk_widget_get_ancestor(caption, GTK_TYPE_BUTTON);
      auto *tooltip = gtk_widget_get_tooltip_text(pick);
      assert(tooltip && latest == tooltip);
      g_free(tooltip);
      assert(latest == gtk_label_get_text(GTK_LABEL(page.header_title_label)));
      select_terminal_tab(tab);
      // Change both allocations in mid-flight. The current pass must finish.
      point_at(page.tab_row, 60, 15);
      gtk_paned_set_position(paned, sidebar - 16);
    } else if (changed && !finished_first && same_glyphs(tail, first_tail)) {
      finished_first = true;
      point_at(page.root_box, 250, 150);
      gtk_paned_set_position(paned, sidebar);
    } else if (changed && same_glyphs(head, next_head)) {
      assert(finished_first);
      started_latest = true;
    }
    if (started_latest && same_glyphs(tail, next_tail)) finished_latest = true;
    if (finished_latest && glyphs(video, caption, 0, width()) == idle) break;
  } while (g_get_monotonic_time() < deadline);
  assert(changed && finished_first && started_latest && finished_latest);
  assert(glyphs(video, caption, 0, width()) == idle);

  // Repeated notifications and allocation changes leave an idle label still.
  g_object_notify(G_OBJECT(page.header_title_label), "label");
  point_at(page.tab_row, 60, 15);
  gtk_paned_set_position(paned, sidebar - 20);
  video.next();
  const auto narrow = glyphs(video, caption, 0, width());
  const auto until = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
  while (g_get_monotonic_time() < until) {
    video.next();
    assert(glyphs(video, caption, 0, width()) == narrow);
  }
  point_at(page.root_box, 250, 150);
  gtk_paned_set_position(paned, sidebar);
  set_main_window_title(&page, first);
  set_main_window_title(&page, latest);
  g_object_set(settings, "gtk-enable-animations", FALSE, nullptr);
  video.next();
  assert(glyphs(video, caption, 0, width()) == idle);
  g_object_set(settings, "gtk-enable-animations", TRUE, nullptr);
  set_main_window_title(&page, "Ready");
  video.next();
  const auto ready = glyphs(video, caption, 0, width());
  set_main_window_title(&page, first);
  video.next();
  set_main_window_title(&page, "Ready");
  bool saw_tail = false;
  const auto short_deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
  do {
    video.next();
    if (same_glyphs(glyphs(video, caption, width() - sample_width, sample_width), first_tail)) saw_tail = true;
    if (glyphs(video, caption, 0, width()) == ready) break;
  } while (g_get_monotonic_time() < short_deadline);
  assert(saw_tail && glyphs(video, caption, 0, width()) == ready);
}

static TerminalTab *tab_for(MainWindow &page, const char *name) {
  set_connection_window_icon(GTK_WINDOW(page.window), GTK_IMAGE(page.header_connection_icon), "default", 20);
  set_main_window_connection_phase(&page, TerminalSessionConnectionPhase::connected);
  set_main_window_title(&page, name);
  TerminalTabActions actions{};
  actions.movable = [] { return true; };
  actions.transferable = actions.movable;
  actions.host = [&page](GtkWidget *window, GtkWidget *body, bool selected) {
    page.window = window;
    page.selected = selected;
    terminal_runtime_set_window(page.runtime, GTK_WINDOW(window), GTK_BOX(body), selected);
  };
  return create_terminal_tab(&page, true, std::move(actions));
}

extern "C" int __wrap_main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto first = load_main_window(TerminalAmbiguousWidth::narrow);
  auto second = load_main_window(TerminalAmbiguousWidth::narrow);
  assert(first && second);
  set_main_window_colors(&*first, {RgbColor{0x7a, 0x22, 0x22}, {}});
  set_main_window_colors(&*second, {RgbColor{0x22, 0x44, 0x8a}, {}});
  auto *a = tab_for(*first, "First");
  auto *b = tab_for(*second, "Second");
  assert(dock_terminal_tabs({b}, b, a));
  gtk_window_set_default_size(GTK_WINDOW(first->window), 720, 460);
  gtk_widget_show_all(first->window);
  auto *list = GTK_LIST_BOX(descendant(first->window, GTK_TYPE_LIST_BOX));
  assert(list);
  auto *row = GTK_WIDGET(gtk_list_box_get_row_at_index(list, 0));
  auto *caption = GTK_LABEL(descendant(row, GTK_TYPE_LABEL));
  assert(caption);
  point_at(second->root_box, 250, 150);
  set_main_window_title(&*first, "Current session title");
  paint(row);
  assert(std::string(gtk_label_get_text(caption)) == "Current session title");
  assert(pixel(row, 15, 3) == 0x6c2422);
  const auto full_width = gtk_widget_get_allocated_width(GTK_WIDGET(caption));
  const auto height = gtk_widget_get_allocated_height(row);
  auto *selected = GTK_WIDGET(gtk_list_box_get_row_at_index(list, 1));
  const auto blue = pixel(selected, 15, 5);
  int gap = 0;
  for (int y = height - 4; y < height + 8; ++y) {
    const auto color = pixel(row, 15, y);
    if (color != 0x6c2422 && color != blue) ++gap;
  }
  assert(gap == 2);
  assert(pixel(selected, 0, height / 2) != blue);
  assert(pixel(row, 0, height / 2) == pixel(row, 15, 3));
  point_at(row, 65, height / 2);
  assert(pixel(row, 15, 3) == 0x732a28);
  assert(gtk_widget_get_allocated_width(GTK_WIDGET(caption)) < full_width);
  assert(gtk_widget_get_allocated_height(row) == height);
  const auto hover_width = gtk_widget_get_allocated_width(GTK_WIDGET(caption));
  // Pick a title at the actual font's width boundary.
  std::string boundary;
  int text_width = 0;
  auto *layout = gtk_widget_create_pango_layout(row, "");
  while (text_width <= hover_width) {
    boundary += 'M';
    pango_layout_set_text(layout, boundary.c_str(), -1);
    pango_layout_get_pixel_size(layout, &text_width, nullptr);
  }
  g_object_unref(layout);
  assert(text_width <= full_width);
  set_main_window_title(&*first, boundary);
  paint(row);
  assert(pango_layout_is_ellipsized(gtk_label_get_layout(caption)));
  point_at(row, gtk_widget_get_allocated_width(row) - 10, height / 2);
  assert(gtk_widget_get_allocated_width(GTK_WIDGET(caption)) == hover_width);
  point_at(second->root_box, 250, 150);
  assert(!pango_layout_is_ellipsized(gtk_label_get_layout(caption)));
  // Switch the inactive page from a custom exterior to its GTK header color.
  auto *theme = gtk_css_provider_new();
  gtk_css_provider_load_from_data(theme, "* {background: white;}", -1, nullptr);
  gtk_style_context_add_provider(gtk_widget_get_style_context(first->header_bar),
      GTK_STYLE_PROVIDER(theme), GTK_STYLE_PROVIDER_PRIORITY_USER);
  set_main_window_colors(&*first, {});
  paint(row);
  assert(pixel(row, 15, 3) == 0xebebeb);
  gtk_style_context_remove_provider(gtk_widget_get_style_context(first->header_bar), GTK_STYLE_PROVIDER(theme));
  g_object_unref(theme);
  set_main_window_colors(&*first, {RgbColor{0x7a, 0x22, 0x22}, {}});
  set_main_window_title(&*first, "First");
  paint(row);
  const auto button = pixel(second->application_menu_button_image, 2, 2);
  assert((button & 255) > (button >> 16));
  TabVideo video(first->window, argc > 1 ? argv[1] : nullptr);
  const auto a_left = band_point(row, 25);
  const auto a_center = band_point(row, 50);
  const auto a_right = band_point(row, 75);
  const auto b_center = band_point(selected, 50);
  const auto b_header = band_point(second->header_bar, 50);
  const auto b_base = video.red(b_center);
  const auto header_base = video.red(b_header);
  const unsigned char output[] = {'x'};
  note_main_window_output(&*first, output);
  bool saw_background = false, saw_shared_center = false;
  const auto start = g_get_monotonic_time();
  do {
    video.next();
    if (!saw_background) {
      assert(video.red(b_center) == b_base && video.red(b_header) == header_base);
      if (video.red(a_left) > 0x6c + 8 && video.red(a_right) == 0x6c) {
        saw_background = true;
        select_terminal_tab(a);
      }
    } else if (video.red(a_center) > 0x6c + 10 && video.red(a_left) < 0x6c + 4 &&
        video.red(a_right) < 0x6c + 4 && video.red(band_point(first->header_bar, 50)) > 0x7a + 10) {
      saw_shared_center = true;
    }
  } while (g_get_monotonic_time() - start < 1600000);
  assert(saw_background && saw_shared_center);
  select_terminal_tab(b);
  video.next();
  notify_titlebar_bell(first->header_bar);
  bool saw_flash = false;
  do {
    video.next();
    const int flash = video.red(a_center);
    if (flash > 0x6c + 5 && flash == video.red(a_right) && flash == video.red(a_left)) saw_flash = true;
    assert(video.red(b_center) == b_base && video.red(b_header) == header_base);
  } while (titlebar_effects_running(first->header_bar));
  assert(saw_flash && video.red(a_center) == 0x6c);
  check_second_row_scroll(*second, video);
  check_title_scroll(*first, a, video, GTK_WIDGET(caption));
  destroy_terminal_runtime(second->runtime);
  second->runtime = nullptr;
  destroy_terminal_tab(b);
  destroy_terminal_runtime(first->runtime);
  first->runtime = nullptr;
  destroy_terminal_tab(a);
  release_main_window(&*second);
  release_main_window(&*first);
  std::puts("PASS docked tab titles, hover layout, shared effects and scrolling");
  return 0;
}

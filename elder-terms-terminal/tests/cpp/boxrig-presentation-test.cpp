// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-runtime.h"

#include <epoxy/gl.h>
#include <cstdio>
#include <string_view>

using namespace elder_terms;

struct Fixture {
  TerminalRuntime *runtime = nullptr;
  guint observation = 0;
  bool fed = false;
  bool notified_ready = false;
  bool failed = false;
  bool gl_reported = false;
  gulong render_hook = 0;
  std::string_view required_renderer;
  boxrig_gtk_render_backend backend = BOXRIG_GTK_RENDER_BACKEND_OPENGL;
  std::uint64_t previous_generation = 0;
};

static gboolean report_gl_connection(GSignalInvocationHint *, guint, const GValue *values, gpointer context) {
  auto *fixture = static_cast<Fixture *>(context);
  auto *area = GTK_WIDGET(g_value_get_object(values));
  if (!gtk_widget_is_ancestor(area, GTK_WIDGET(terminal_runtime_widget(fixture->runtime)))) return TRUE;
  fixture->render_hook = 0;
  if (!gdk_gl_context_get_current()) return FALSE;
  const auto *vendor = reinterpret_cast<const char *>(glGetString(GL_VENDOR));
  const auto *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
  const auto *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
  fixture->gl_reported = vendor && renderer && version;
  if (fixture->gl_reported) {
    std::printf("GL vendor=%s\nGL renderer=%s\nGL version=%s\n", vendor, renderer, version);
    if (std::string_view(renderer).find(fixture->required_renderer) == std::string_view::npos) {
      std::fputs("The application did not use the required GL renderer\n", stderr);
      fixture->failed = true;
    }
  }
  return FALSE;
}

static gboolean observe(gpointer context) {
  auto *fixture = static_cast<Fixture *>(context);
  fixture->observation = 0;
  auto *widget = terminal_runtime_widget(fixture->runtime);
  auto *view = boxrig_gtk3_terminal_widget_view(widget);
  boxrig_ui_size_command_state size{};
  if (!boxrig_gtk3_terminal_widget_has_presentation(widget) ||
      boxrig_ui_view_frame_requested(view) ||
      boxrig_gtk3_terminal_widget_get_size_command_state(widget, &size) != BOXRIG_UI_OK ||
      size != BOXRIG_UI_SIZE_COMMAND_IDLE) return G_SOURCE_REMOVE;
  const auto generation = boxrig_gtk3_terminal_widget_presentation_generation(widget);
  if (!fixture->fed) {
    fixture->fed = true;
    fixture->previous_generation = generation;
    fixture->notified_ready = false;
    terminal_runtime_feed(fixture->runtime, "Updated native presentation");
    return G_SOURCE_REMOVE;
  }
  if (generation > fixture->previous_generation) {
    boxrig_gtk_render_state render{};
    if (boxrig_gtk3_terminal_widget_get_render_state(widget, &render) != BOXRIG_UI_OK || render.active_backend != fixture->backend) {
      std::fputs("Requested presentation backend is unavailable\n", stderr);
      fixture->failed = true;
    }
    if (fixture->backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL && !fixture->gl_reported) {
      std::fputs("The application did not report its GL connection\n", stderr);
      fixture->failed = true;
    }
    // Native drawing has finished and higher-priority layout dispatches have
    // drained. Readiness must agree with the actual current presentation.
    if (!terminal_runtime_is_settled(fixture->runtime) || !fixture->notified_ready) {
      std::fputs("Native presentation completed without a settled runtime notification\n", stderr);
      fixture->failed = true;
    }
    gtk_main_quit();
  }
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(window), content);
  Fixture fixture;
  if (argc > 1 && std::string_view(argv[1]) == "cairo") fixture.backend = BOXRIG_GTK_RENDER_BACKEND_CAIRO;
  if (argc > 2) fixture.required_renderer = argv[2];
  fixture.runtime = create_terminal_runtime(GTK_WINDOW(window), GTK_BOX(content), GTK_BOX(content), elder_terms::TerminalAmbiguousWidth::narrow);
  if (!fixture.runtime) return 1;
  fixture.render_hook = g_signal_add_emission_hook(g_signal_lookup("render", GTK_TYPE_GL_AREA), 0,
      report_gl_connection, &fixture, nullptr);
  terminal_runtime_set_changed_callback(fixture.runtime, [](void *context) {
    auto *fixture = static_cast<Fixture *>(context);
    fixture->notified_ready = terminal_runtime_is_settled(fixture->runtime);
  }, &fixture);
  g_signal_connect_after(terminal_runtime_widget(fixture.runtime), "draw",
      G_CALLBACK(+[](GtkWidget *, cairo_t *, gpointer context) -> gboolean {
        auto *fixture = static_cast<Fixture *>(context);
        if (!fixture->observation) fixture->observation = g_idle_add(observe, fixture);
        return FALSE;
      }), &fixture);
  gtk_widget_show_all(window);
  gtk_main();
  if (fixture.observation) g_source_remove(fixture.observation);
  if (fixture.render_hook)
    g_signal_remove_emission_hook(g_signal_lookup("render", GTK_TYPE_GL_AREA), fixture.render_hook);
  destroy_terminal_runtime(fixture.runtime);
  gtk_widget_destroy(window);
  return fixture.failed ? 1 : 0;
}

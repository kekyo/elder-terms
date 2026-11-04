// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/main-window.h"
#include "../../src/titlebar-effects.h"
#include "../../src/terminal-runtime.h"
#include "../../src/terminal-presentation.h"
#include <cassert>
#include <cstdio>

using namespace elder_terms;

static void check_title(const MainWindow &window, const char *text) {
  assert(std::string(gtk_window_get_title(GTK_WINDOW(window.window))) == text);
  assert(std::string(gtk_label_get_text(GTK_LABEL(window.header_title_label))) == text);
  assert(std::string(gtk_header_bar_get_title(GTK_HEADER_BAR(window.header_bar))) == text);
}

extern "C" int __wrap_main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto window = load_main_window(TerminalAmbiguousWidth::narrow);
  assert(window);
  auto *presentation = create_terminal_presentation(&*window, {}, {}, {});
  auto settings = create_default_settings(default_terminal_display_settings(16), "Effects");
  set_main_window_connection_phase(&*window, TerminalSessionConnectionPhase::connected);
  set_main_window_title(&*window, "Initial");
  gtk_widget_show(window->window);
  assert(!titlebar_effects_running(window->header_bar));
  set_main_window_title(&*window, "Initial");
  assert(!titlebar_effects_running(window->header_bar));
  set_main_window_title(&*window, "A");
  assert(titlebar_effects_running(window->header_bar));
  // No dispatcher iteration or frame occurs between these updates: the effect
  // cannot delay or coalesce any visible title, including A -> B -> A.
  check_title(*window, "A");
  set_main_window_title(&*window, "B");
  check_title(*window, "B");
  set_main_window_title(&*window, "A");
  check_title(*window, "A");
  assert(titlebar_effects_running(window->header_bar));
  gtk_widget_hide(window->window);
  set_main_window_title(&*window, "Hidden");
  check_title(*window, "Hidden");
  gtk_widget_show(window->window);
  assert(!titlebar_effects_running(window->header_bar));
  set_main_window_title(&*window, "Hidden");
  assert(!titlebar_effects_running(window->header_bar));
  set_main_window_connection_phase(&*window, TerminalSessionConnectionPhase::disconnected);
  check_title(*window, "Hidden (Disconnected)");
  assert(titlebar_effects_running(window->header_bar));
  set_setting_value(&settings, terminal_visual_bell_setting_key(), false);
  set_setting_value(&settings, terminal_title_change_effect_setting_key(), false);
  assert(apply_terminal_presentation(presentation, settings, nullptr));
  set_main_window_title(&*window, "Disabled");
  check_title(*window, "Disabled (Disconnected)");
  assert(!titlebar_effects_running(window->header_bar));
  set_setting_value(&settings, terminal_visual_bell_setting_key(), true);
  set_setting_value(&settings, terminal_title_change_effect_setting_key(), true);
  assert(apply_terminal_presentation(presentation, settings, nullptr));
  set_main_window_title(&*window, "Disabled");
  assert(!titlebar_effects_running(window->header_bar));
  set_main_window_connection_phase(&*window, TerminalSessionConnectionPhase::connected);
  check_title(*window, "Disabled");
  assert(titlebar_effects_running(window->header_bar));
  destroy_terminal_presentation(presentation);
  destroy_terminal_runtime(window->runtime);
  window->runtime = nullptr;
  gtk_widget_destroy(window->window);
  release_main_window(&*window);
  std::puts("PASS synchronous final titles, duplicates, A-B-A, hidden changes, disconnect and disabled effects");
  return 0;
}

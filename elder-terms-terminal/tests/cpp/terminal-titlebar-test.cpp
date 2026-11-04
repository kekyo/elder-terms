// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/main-window.h"
#include "../../../shared/src/connection-runtime/titlebar-effects.h"
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
  // Temporary removal check: title changes alone no longer start the gradient.
  assert(!titlebar_effects_running(window->header_bar));
  note_main_window_activity(&*window, ActivityIndicatorId::rd);
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
  note_main_window_activity(&*window, ActivityIndicatorId::rd);
  assert(titlebar_effects_running(window->header_bar));
  set_setting_value(&settings, terminal_visual_bell_setting_key(), std::string("disabled"));
  set_setting_value(&settings, general_receive_effect_setting_key(), std::string("disabled"));
  assert(apply_terminal_presentation(presentation, settings, nullptr));
  set_main_window_title(&*window, "Disabled");
  check_title(*window, "Disabled (Disconnected)");
  note_main_window_activity(&*window, ActivityIndicatorId::rd);
  assert(!titlebar_effects_running(window->header_bar));
  set_setting_value(&settings, terminal_visual_bell_setting_key(), std::string("medium"));
  set_setting_value(&settings, general_receive_effect_setting_key(), std::string("medium"));
  assert(apply_terminal_presentation(presentation, settings, nullptr));
  set_main_window_title(&*window, "Disabled");
  assert(!titlebar_effects_running(window->header_bar));
  set_main_window_connection_phase(&*window, TerminalSessionConnectionPhase::connected);
  check_title(*window, "Disabled");
  note_main_window_activity(&*window, ActivityIndicatorId::sd);
  assert(!titlebar_effects_running(window->header_bar));
  // Reception remains visible even if its small status indicator is hidden.
  window->indicator_visible.fill(false);
  note_main_window_activity(&*window, ActivityIndicatorId::rd);
  assert(titlebar_effects_running(window->header_bar));
  destroy_terminal_presentation(presentation);
  destroy_terminal_runtime(window->runtime);
  window->runtime = nullptr;
  gtk_widget_destroy(window->window);
  release_main_window(&*window);
  std::puts("PASS synchronous titles, receive activity, hidden changes, disconnect and disabled effects");
  return 0;
}

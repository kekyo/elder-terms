// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/tracked-window.h>

#include <glib.h>
#include <gtk/gtk.h>

#include <string>
#include <unistd.h>

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "Tracked window fixture");
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
  gtk_widget_show_all(window);
  if (const char *pid_path = g_getenv("ELDER_TERMS_TRACKED_WINDOW_PID")) {
    const std::string pid = std::to_string(getpid());
    g_file_set_contents(pid_path, pid.c_str(), -1, nullptr);
  }
  elder_terms::attach_tracked_window(window,
      [](GtkWidget *widget, std::uint32_t time, const std::string &token) {
        const char *capture = g_getenv("ELDER_TERMS_TRACKED_WINDOW_CAPTURE");
        if (capture != nullptr) {
          gchar *previous = nullptr;
          g_file_get_contents(capture, &previous, nullptr, nullptr);
          const std::string next = std::string(previous == nullptr ? "" : previous) + "present\n";
          g_file_set_contents(capture, next.c_str(), -1, nullptr);
          g_free(previous);
        }
        elder_terms::present_tracked_window(widget, time, token);
      });
  gtk_main();
  return 0;
}

#include <iostream>

#include <gtk/gtk.h>

struct ClipboardReadState {
  GMainLoop *loop = nullptr;
  bool completed = false;
  int exit_code = 1;
};

static void on_clipboard_text_received(GtkClipboard *, const gchar *text,
                                       gpointer data) {
  auto *state = static_cast<ClipboardReadState *>(data);
  state->completed = true;
  if (text != nullptr) {
    std::cout << text;
    state->exit_code = 0;
  }
  if (g_main_loop_is_running(state->loop)) {
    g_main_loop_quit(state->loop);
  }
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);

  ClipboardReadState state{
      .loop = g_main_loop_new(nullptr, FALSE),
  };
  GtkClipboard *clipboard = gtk_clipboard_get_default(gdk_display_get_default());
  gtk_clipboard_request_text(clipboard, on_clipboard_text_received, &state);
  if (!state.completed) {
    g_main_loop_run(state.loop);
  }
  g_main_loop_unref(state.loop);
  return state.exit_code;
}

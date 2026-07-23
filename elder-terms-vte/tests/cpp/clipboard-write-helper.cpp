#include <unistd.h>

#include <iostream>

#include <glib-unix.h>
#include <gtk/gtk.h>

static gboolean on_stdin_ready(gint fd, GIOCondition condition,
                               gpointer data) {
  if ((condition & G_IO_IN) != 0) {
    char value = '\0';
    (void)::read(fd, &value, 1);
  }
  g_main_loop_quit(static_cast<GMainLoop *>(data));
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  if (argc != 2) {
    std::cerr << "Usage: clipboard-write-helper TEXT\n";
    return 2;
  }

  GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
  GtkClipboard *clipboard =
      gtk_clipboard_get_default(gdk_display_get_default());
  gtk_clipboard_set_text(clipboard, argv[1], -1);
  g_unix_fd_add(STDIN_FILENO,
                static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
                on_stdin_ready, loop);
  std::cout << "READY\n" << std::flush;
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  return 0;
}

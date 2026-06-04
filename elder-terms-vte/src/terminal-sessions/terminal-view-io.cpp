#include "terminal-view-io.h"

#include <vte/vte.h>

#include <cstddef>
#include <utility>

namespace elder_terms {

TerminalViewIo::TerminalViewIo(GtkWidget *terminal)
    : terminal(terminal) {
}

TerminalViewIo::~TerminalViewIo() {
  disconnect_user_input();
}

void TerminalViewIo::on_terminal_commit(VteTerminal *, const gchar *text,
                                        guint size, gpointer user_data) {
  auto *self = static_cast<TerminalViewIo *>(user_data);
  if (self->input_callback && text != nullptr && size != 0) {
    const auto *data = reinterpret_cast<const unsigned char *>(text);
    self->input_callback(
        std::span<const unsigned char>(data, static_cast<std::size_t>(size)));
  }
}

void TerminalViewIo::feed(std::span<const unsigned char> bytes) {
  if (bytes.empty()) {
    return;
  }

  vte_terminal_feed(VTE_TERMINAL(terminal),
                    reinterpret_cast<const char *>(bytes.data()),
                    static_cast<gssize>(bytes.size()));
}

TerminalViewGridSize TerminalViewIo::grid_size() const {
  return {
      .columns = vte_terminal_get_column_count(VTE_TERMINAL(terminal)),
      .rows = vte_terminal_get_row_count(VTE_TERMINAL(terminal)),
  };
}

void TerminalViewIo::connect_user_input(TerminalViewInputCallback callback) {
  disconnect_user_input();
  input_callback = std::move(callback);
  commit_handler_id =
      g_signal_connect(terminal, "commit",
                       G_CALLBACK(TerminalViewIo::on_terminal_commit), this);
}

void TerminalViewIo::disconnect_user_input() {
  if (commit_handler_id != 0) {
    g_signal_handler_disconnect(terminal, commit_handler_id);
    commit_handler_id = 0;
  }
  input_callback = {};
}

} // namespace elder_terms

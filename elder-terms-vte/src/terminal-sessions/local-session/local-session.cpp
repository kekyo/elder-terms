#include <vte/vte.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "local-session.h"

namespace elder_terms {

static std::string shell_path() {
  const char *shell = std::getenv("SHELL");
  if (shell == nullptr || shell[0] == '\0') {
    return "/bin/sh";
  }

  return shell;
}

static void on_terminal_spawned(VteTerminal *, GPid, GError *error, gpointer) {
  if (error != nullptr) {
    std::cerr << "Failed to spawn shell: " << error->message << '\n';
  }
}

static void notify_session_ended(const TerminalSessionCallbacks &callbacks) {
  if (callbacks.ended) {
    callbacks.ended();
  }
}

class TerminalLocalShellSession final : public TerminalSession {
private:
  GtkWidget *terminal = nullptr;
  LocalShellConnectionSettings settings;
  TerminalSessionCallbacks callbacks;
  gulong child_exited_handler_id = 0;
  bool started = false;
  bool stopping = false;

  void disconnect_terminal_signal() {
    if (child_exited_handler_id != 0) {
      g_signal_handler_disconnect(terminal, child_exited_handler_id);
      child_exited_handler_id = 0;
    }
  }

  void handle_child_exited(int status) {
    (void)status;
    if (stopping) {
      return;
    }

    disconnect_terminal_signal();
    notify_session_ended(callbacks);
  }

  static void on_terminal_child_exited(VteTerminal *, gint status,
                                       gpointer user_data) {
    auto *self = static_cast<TerminalLocalShellSession *>(user_data);
    self->handle_child_exited(status);
  }

public:
  TerminalLocalShellSession(GtkWidget *terminal,
                            LocalShellConnectionSettings settings,
                            TerminalSessionCallbacks callbacks)
      : terminal(terminal),
        settings(std::move(settings)),
        callbacks(callbacks) {
  }

  ~TerminalLocalShellSession() override {
    stop();
  }

  bool start() override {
    if (started) {
      return true;
    }

    std::string shell = shell_path();
    char *argv[] = {
        shell.data(),
        nullptr,
    };

    child_exited_handler_id =
        g_signal_connect(terminal, "child-exited",
                         G_CALLBACK(
                             TerminalLocalShellSession::on_terminal_child_exited),
                         this);
    vte_terminal_spawn_async(VTE_TERMINAL(terminal), VTE_PTY_DEFAULT, nullptr,
                             argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr,
                             nullptr, -1, nullptr, on_terminal_spawned,
                             nullptr);
    started = true;
    return true;
  }

  void stop() override {
    if (stopping) {
      return;
    }

    stopping = true;
    disconnect_terminal_signal();
  }

  void resize(glong columns, glong rows) override {
    (void)columns;
    (void)rows;
  }
};

std::unique_ptr<TerminalSession>
create_terminal_local_shell_session(GtkWidget *terminal,
                                    LocalShellConnectionSettings settings,
                                    TerminalSessionCallbacks callbacks) {
  return std::make_unique<TerminalLocalShellSession>(terminal,
                                                     std::move(settings),
                                                     callbacks);
}

} // namespace elder_terms

#include <iostream>
#include <memory>
#include <utility>

#include "terminal-session.h"

#include "terminal-sessions/terminal-session.h"
#include "terminal-sessions/local-session/local-session.h"
#include "terminal-sessions/serial-session/serial-session.h"
#include "terminal-sessions/telnet-session/telnet-session.h"

namespace elder_terms {

static std::unique_ptr<TerminalSession>
create_backend(GtkWidget *terminal, const TerminalConnectionProfile &profile,
               TerminalSessionCallbacks callbacks) {
  if (profile.kind == TerminalConnectionKind::local_shell) {
    const auto *settings =
        std::get_if<LocalShellConnectionSettings>(&profile.settings);
    if (settings != nullptr) {
      return create_terminal_local_shell_session(terminal, *settings,
                                                 callbacks);
    }
  }

  if (profile.kind == TerminalConnectionKind::telnet) {
    const auto *settings =
        std::get_if<TelnetConnectionSettings>(&profile.settings);
    if (settings != nullptr) {
      if (settings->address.empty()) {
        return nullptr;
      }
      return create_terminal_telnet_session(terminal, *settings, callbacks);
    }
  }

  if (profile.kind == TerminalConnectionKind::serial) {
    const auto *settings =
        std::get_if<SerialConnectionSettings>(&profile.settings);
    if (settings != nullptr) {
      if (settings->device.empty()) {
        return nullptr;
      }
      return create_terminal_serial_session(terminal, *settings, callbacks);
    }
  }

  std::cerr << "Warning: unsupported terminal connection profile" << '\n';
  return nullptr;
}

struct TerminalSessionState {
  GtkWidget *terminal = nullptr;
  TerminalConnectionProfile profile;
  TerminalSessionCallbacks callbacks;
  std::unique_ptr<TerminalSession> session;
};

TerminalSessionState *
create_terminal_session(GtkWidget *terminal, TerminalConnectionProfile profile,
                        TerminalSessionCallbacks callbacks) {
  auto *state = new TerminalSessionState();
  state->terminal = terminal;
  state->profile = std::move(profile);
  state->callbacks = callbacks;
  state->session = create_backend(terminal, state->profile, state->callbacks);
  return state;
}

bool start_terminal_session(TerminalSessionState *state) {
  if (state == nullptr || state->session == nullptr) {
    return false;
  }

  return state->session->start();
}

void stop_terminal_session(TerminalSessionState *state) {
  if (state == nullptr || state->session == nullptr) {
    return;
  }

  state->session->stop();
}

void resize_terminal_session(TerminalSessionState *state, glong columns,
                             glong rows) {
  if (state == nullptr || state->session == nullptr) {
    return;
  }

  state->session->resize(columns, rows);
}

void apply_terminal_session_connection_profile(
    TerminalSessionState *state, const TerminalConnectionProfile &profile) {
  if (state == nullptr || state->session == nullptr) {
    return;
  }

  state->profile = profile;
  state->session->apply_connection_profile(state->profile);
}

void destroy_terminal_session(TerminalSessionState *state) {
  stop_terminal_session(state);
  delete state;
}

} // namespace elder_terms

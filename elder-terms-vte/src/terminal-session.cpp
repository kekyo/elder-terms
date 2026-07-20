#include <memory>
#include <utility>
#include <variant>

#include "terminal-session.h"

#include "terminal-sessions/terminal-session.h"
#include "terminal-sessions/local-session/local-session.h"
#include "terminal-sessions/serial-session/serial-session.h"
#include "terminal-sessions/telnet-session/telnet-session.h"

namespace elder_terms {

static constexpr const char *application_title = "elder-terms";

static std::string application_window_title(const std::string &session_title) {
  return std::string(application_title) + ": " + session_title;
}

struct TerminalSessionState {
  GtkWidget *terminal = nullptr;
  TerminalConnectionProfile profile;
  TerminalSessionCallbacks callbacks;
  std::unique_ptr<TerminalSession> session;
};

struct TerminalSessionBackendCreator {
  GtkWidget *terminal = nullptr;
  TerminalTextSettings text_settings;
  TerminalSessionCallbacks callbacks;

  std::unique_ptr<TerminalSession>
  operator()(const LocalShellConnectionSettings &settings) const {
    return create_terminal_local_shell_session(terminal, settings,
                                               text_settings, callbacks);
  }

  std::unique_ptr<TerminalSession>
  operator()(const TelnetConnectionSettings &settings) const {
    return create_terminal_telnet_session(terminal, settings, text_settings,
                                          callbacks);
  }

  std::unique_ptr<TerminalSession>
  operator()(const SerialConnectionSettings &settings) const {
    return create_terminal_serial_session(terminal, settings, text_settings,
                                          callbacks);
  }
};

static std::unique_ptr<TerminalSession>
create_backend(GtkWidget *terminal, const TerminalConnectionProfile &profile,
               TerminalSessionCallbacks callbacks) {
  return std::visit(
      TerminalSessionBackendCreator{
          .terminal = terminal,
          .text_settings = profile.text_settings,
          .callbacks = callbacks,
      },
      profile.settings);
}

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

bool terminal_session_supports_transfer(const TerminalSessionState *state) {
  if (state == nullptr || state->session == nullptr) {
    return false;
  }

  return state->session->supports_transfer();
}

bool terminal_session_transfer_in_progress(const TerminalSessionState *state) {
  if (state == nullptr || state->session == nullptr) {
    return false;
  }

  return state->session->transfer_in_progress();
}

bool start_terminal_session_transfer(TerminalSessionState *state,
                                     TerminalTransferRequest request) {
  if (state == nullptr || state->session == nullptr) {
    return false;
  }

  return state->session->start_transfer(std::move(request));
}

void set_terminal_session_zmodem_autostart(TerminalSessionState *state,
                                           bool enabled) {
  if (state == nullptr || state->session == nullptr) {
    return;
  }

  state->session->set_zmodem_autostart(enabled);
}

std::string terminal_session_title(const TerminalSessionState *state) {
  if (state == nullptr || state->session == nullptr) {
    return application_title;
  }

  return application_window_title(state->session->title());
}

void apply_terminal_session_connection_profile(
    TerminalSessionState *state, const TerminalConnectionProfile &profile) {
  if (state == nullptr) {
    return;
  }

  state->profile = profile;
  if (state->session != nullptr) {
    state->session->apply_connection_profile(state->profile);
  }
}

void destroy_terminal_session(TerminalSessionState *state) {
  stop_terminal_session(state);
  delete state;
}

} // namespace elder_terms

#include <memory>
#include <optional>
#include <utility>
#include <variant>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

#include "terminal-session.h"

#include "terminal-sessions/terminal-session.h"
#include "terminal-sessions/local-session/local-session.h"
#include "terminal-sessions/serial-session/serial-session.h"
#include "terminal-sessions/ssh-session/ssh-session.h"
#include "terminal-sessions/telnet-session/telnet-session.h"

namespace elder_terms {

static constexpr const char *application_title = "elder-terms";

static std::string application_window_title(
    const std::string &connection_name) {
  return std::string(application_title) + ": " + connection_name;
}

struct TerminalSessionState {
  GtkWidget *terminal = nullptr;
  TerminalConnectionProfile profile;
  TerminalSessionCallbacks callbacks;
  TerminalSessionOptions options;
  std::unique_ptr<TerminalSession> session;
  std::optional<cardio::promise<void>> retirement_task;
  std::optional<cardio::promise<void>> restart_task;
  std::size_t generation = 1;
  glong columns = 0;
  glong rows = 0;
  bool stopping = false;
  bool retiring = false;
  bool quiescent = false;
  bool reconnect_ready = false;
  bool restarting = false;
  bool ended = false;
  bool zmodem_autostart = false;
};

static bool is_network_session(const TerminalSessionState *state) {
  return state->profile.kind == TerminalConnectionKind::ssh ||
         state->profile.kind == TerminalConnectionKind::telnet;
}

static bool is_current_session(const TerminalSessionState *state,
                               std::size_t generation) {
  return !state->stopping && state->generation == generation;
}

// Session and operation callbacks share the same owner and generation guard.
// Joining their tasks, not this guard, keeps their captured resources alive.
template <typename Callback>
static Callback guard_session_callback(TerminalSessionState *state,
                                        std::size_t generation, Callback callback) {
  if (!callback) return {};
  return [state, generation, callback = std::move(callback)](auto &&...arguments) {
    if (is_current_session(state, generation) && callback) {
      callback(std::forward<decltype(arguments)>(arguments)...);
    }
  };
}

static void notify_reconnect_state(TerminalSessionState *state) {
  if (!state->stopping && state->callbacks.reconnect_state_changed) {
    state->callbacks.reconnect_state_changed();
  }
}

static cardio::promise<void> retire_backend_async(TerminalSessionState *state) {
  // Never destroy or join a backend on its own notification stack.
  co_await cardio::resolved();
  state->session->stop();
  co_await state->session->wait_stopped_async();
  state->quiescent = true;
  state->retiring = false;
  state->reconnect_ready = !state->stopping;
  notify_reconnect_state(state);
}

static void retire_backend(TerminalSessionState *state) {
  if (!is_network_session(state) || state->stopping || state->retiring ||
      state->quiescent) {
    return;
  }
  state->retiring = true;
  state->reconnect_ready = false;
  state->retirement_task.emplace(retire_backend_async(state));
}

static cardio::promise<SshUserPromptResponse> prompt_session_async(
    TerminalSessionState *state, std::size_t generation,
    const SshUserPrompt &prompt, cardio::cancellation cancellation) {
  if (!is_current_session(state, generation) || !state->callbacks.ssh_prompt) {
    co_return SshUserPromptResponse{};
  }
  auto response = co_await state->callbacks.ssh_prompt(prompt, std::move(cancellation));
  co_return is_current_session(state, generation) ? std::move(response)
                                                 : SshUserPromptResponse{};
}

static TerminalSessionCallbacks backend_callbacks(TerminalSessionState *state) {
  const auto generation = state->generation;
  TerminalSessionCallbacks result{
      .ended = [state, generation]() {
        if (!is_current_session(state, generation) || state->ended) return;
        state->ended = true;
        retire_backend(state);
        if (state->callbacks.ended) state->callbacks.ended();
      },
      .activity = guard_session_callback(state, generation, state->callbacks.activity),
      .indicator_state = guard_session_callback(state, generation, state->callbacks.indicator_state),
      .connection_phase = [state, generation](TerminalSessionConnectionPhase phase) {
        if (!is_current_session(state, generation)) return;
        if (phase == TerminalSessionConnectionPhase::disconnected) retire_backend(state);
        if (state->callbacks.connection_phase) state->callbacks.connection_phase(phase);
      },
      .failure = guard_session_callback(state, generation, state->callbacks.failure),
      .output = guard_session_callback(state, generation, state->callbacks.output),
      .zmodem_auto_start = guard_session_callback(state, generation, state->callbacks.zmodem_auto_start),
      .ssh_prompt = {},
  };
  if (state->callbacks.ssh_prompt) {
    result.ssh_prompt = [state, generation](const SshUserPrompt &prompt,
                                           cardio::cancellation cancellation) {
        return prompt_session_async(state, generation, prompt, std::move(cancellation));
    };
  }
  return result;
}

struct TerminalSessionBackendCreator {
  GtkWidget *terminal = nullptr;
  TerminalTextSettings text_settings;
  TerminalSessionCallbacks callbacks;
  TerminalSessionOptions options;

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

  std::unique_ptr<TerminalSession>
  operator()(const SshConnectionSettings &settings) const {
    return create_terminal_ssh_session(terminal, settings, text_settings,
                                       callbacks,
                                       SshChannelConnectionOptions{
                                           .known_hosts_file =
                                               options.ssh_known_hosts_file,
                                           .config_file = {},
                                       });
  }
};

static std::unique_ptr<TerminalSession>
create_backend(GtkWidget *terminal, const TerminalConnectionProfile &profile,
               TerminalSessionCallbacks callbacks,
               TerminalSessionOptions options) {
  return std::visit(
      TerminalSessionBackendCreator{
          .terminal = terminal,
          .text_settings = profile.text_settings,
          .callbacks = callbacks,
          .options = std::move(options),
      },
      profile.settings);
}

TerminalSessionState *
create_terminal_session(GtkWidget *terminal, TerminalConnectionProfile profile,
                        TerminalSessionCallbacks callbacks,
                        TerminalSessionOptions options) {
  auto *state = new TerminalSessionState();
  state->terminal = terminal;
  state->profile = std::move(profile);
  state->callbacks = callbacks;
  state->options = std::move(options);
  state->session = create_backend(terminal, state->profile, backend_callbacks(state),
                                  state->options);
  return state;
}

bool start_terminal_session(TerminalSessionState *state) {
  if (state == nullptr || state->session == nullptr || state->stopping) {
    return false;
  }

  const bool started = state->session->start();
  if (!started) {
    backend_callbacks(state).connection_phase(TerminalSessionConnectionPhase::disconnected);
  }
  return started;
}

void stop_terminal_session(TerminalSessionState *state) {
  if (state == nullptr || state->session == nullptr) {
    return;
  }

  state->stopping = true;
  state->reconnect_ready = false;
  state->session->stop();
}

cardio::promise<void> stop_terminal_session_async(TerminalSessionState *state) {
  if (state == nullptr) co_return;
  stop_terminal_session(state);
  if (state->restart_task.has_value()) co_await *state->restart_task;
  if (state->retirement_task.has_value()) co_await *state->retirement_task;
  if (state->session != nullptr && !state->quiescent) {
    co_await state->session->wait_stopped_async();
    state->quiescent = true;
  }
}

bool terminal_session_can_reconnect(const TerminalSessionState *state) {
  return state != nullptr && is_network_session(state) && !state->stopping &&
         !state->restarting && state->reconnect_ready;
}

static cardio::promise<void> restart_backend_async(TerminalSessionState *state) {
  co_await cardio::resolved();
  if (state->stopping) {
    state->restarting = false;
    co_return;
  }
  // Readiness guarantees that all operations borrowing the old backend have
  // finished. Independent SFTP windows may still own its shared transport.
  state->retirement_task.reset();
  ++state->generation;
  state->session.reset();
  state->quiescent = false;
  state->ended = false;
  state->session = create_backend(state->terminal, state->profile,
                                  backend_callbacks(state), state->options);
  state->session->set_zmodem_autostart(state->zmodem_autostart);
  if (state->columns > 0 && state->rows > 0) {
    state->session->resize(state->columns, state->rows);
  }
  state->restarting = false;
  (void)start_terminal_session(state);
}

bool reconnect_terminal_session(TerminalSessionState *state) {
  if (!terminal_session_can_reconnect(state)) return false;
  state->reconnect_ready = false;
  state->restarting = true;
  notify_reconnect_state(state);
  state->restart_task.emplace(restart_backend_async(state));
  return true;
}

void resize_terminal_session(TerminalSessionState *state, glong columns,
                             glong rows) {
  if (state == nullptr || state->session == nullptr) {
    return;
  }

  state->columns = columns;
  state->rows = rows;
  state->session->resize(columns, rows);
}

bool send_terminal_session_text(TerminalSessionState *state,
                                const std::string &utf8_text) {
  if (state == nullptr || state->session == nullptr) {
    return false;
  }

  return state->session->send_text(utf8_text);
}

bool terminal_session_supports_transfer(const TerminalSessionState *state) {
  if (state == nullptr || state->session == nullptr) {
    return false;
  }

  return state->session->supports_transfer();
}

bool terminal_session_supports_text_send(const TerminalSessionState *state) {
  if (state == nullptr || state->session == nullptr) {
    return false;
  }

  return state->session->supports_text_send();
}

bool terminal_session_supports_break(const TerminalSessionState *state) {
  if (state == nullptr || state->session == nullptr) {
    return false;
  }

  return state->session->supports_break();
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

  const auto generation = state->generation;
  request.active = guard_session_callback(state, generation, std::move(request.active));
  request.status = guard_session_callback(state, generation, std::move(request.status));
  request.progress = guard_session_callback(state, generation, std::move(request.progress));
  request.finished = guard_session_callback(state, generation, std::move(request.finished));
  return state->session->start_transfer(std::move(request));
}

bool start_terminal_session_text_send(TerminalSessionState *state,
                                      TerminalTextSendRequest request) {
  if (state == nullptr || state->session == nullptr) {
    return false;
  }

  const auto generation = state->generation;
  request.active = guard_session_callback(state, generation, std::move(request.active));
  request.status = guard_session_callback(state, generation, std::move(request.status));
  request.progress = guard_session_callback(state, generation, std::move(request.progress));
  request.finished = guard_session_callback(state, generation, std::move(request.finished));
  return state->session->start_text_send(std::move(request));
}

bool send_terminal_session_break(TerminalSessionState *state,
                                 TerminalBreakRequest request) {
  if (state == nullptr || state->session == nullptr) {
    return false;
  }

  const auto generation = state->generation;
  request.status = guard_session_callback(state, generation, std::move(request.status));
  request.finished = guard_session_callback(state, generation, std::move(request.finished));
  return state->session->send_break(std::move(request));
}

bool cancel_terminal_session_transfer(TerminalSessionState *state) {
  if (state == nullptr || state->session == nullptr) {
    return false;
  }

  return state->session->cancel_transfer();
}

void set_terminal_session_zmodem_autostart(TerminalSessionState *state,
                                           bool enabled) {
  if (state == nullptr || state->session == nullptr) {
    return;
  }

  state->zmodem_autostart = enabled;
  state->session->set_zmodem_autostart(enabled);
}

std::shared_ptr<AuthenticatedSshTransport>
terminal_session_authenticated_ssh_transport(
    const TerminalSessionState *state) {
  if (state == nullptr || state->session == nullptr) {
    return nullptr;
  }

  return state->session->authenticated_ssh_transport();
}

std::string terminal_session_window_title(const TerminalSessionState *state) {
  if (state == nullptr || state->session == nullptr) {
    return application_title;
  }

  return application_window_title(state->profile.name);
}

std::string
terminal_session_connection_detail(const TerminalSessionState *state) {
  if (state == nullptr || state->session == nullptr) {
    return _("Terminal");
  }

  return state->session->connection_detail();
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

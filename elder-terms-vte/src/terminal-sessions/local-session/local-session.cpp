#include <vte/vte.h>

#include <signal.h>

#include <cardio.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "local-session.h"

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

#include "../../terminal-text-send-runner.h"
#include "../terminal-view-io.h"

namespace elder_terms {

class TerminalLocalShellSession;

struct LocalShellSpawnState {
  TerminalLocalShellSession *session = nullptr;
  bool active = true;
};

static std::string shell_path() {
  const char *shell = std::getenv("SHELL");
  if (shell == nullptr || shell[0] == '\0') {
    return "/bin/sh";
  }

  return shell;
}

static int clamp_pty_dimension(glong value) {
  if (value <= 0) {
    return 0;
  }
  if (value > G_MAXINT) {
    return G_MAXINT;
  }
  return static_cast<int>(value);
}

static void notify_session_ended(const TerminalSessionCallbacks &callbacks) {
  if (callbacks.ended) {
    callbacks.ended();
  }
}

static void clear_gerror(GError **error) {
  if (error != nullptr && *error != nullptr) {
    g_clear_error(error);
  }
}

static std::string gerror_message(GError *error, const char *fallback) {
  if (error != nullptr && error->message != nullptr) {
    return error->message;
  }
  return fallback;
}

static void terminate_child_noexcept(GPid pid) {
  if (pid > 0) {
    (void)::kill(pid, SIGHUP);
  }
}

class TerminalLocalShellSession final : public TerminalSession {
private:
  TerminalViewIo terminal_io;
  LocalShellConnectionSettings settings;
  TerminalSessionCallbacks callbacks;
  std::shared_ptr<LocalShellSpawnState> spawn_state;
  std::optional<cardio::io_uring> io;
  cardio::cancellation_source stop_source;
  cardio::primitives::mutex backend_write_mutex;
  std::deque<std::vector<unsigned char>> outgoing;
  std::optional<cardio::cancellation_source> text_send_cancel_source;
  std::optional<cardio::promise<void>> read_task;
  std::optional<cardio::promise<void>> write_task;
  std::optional<cardio::promise<void>> text_send_task;
  VtePty *pty = nullptr;
  GPid child_pid = 0;
  int pty_fd = -1;
  guint child_watch_id = 0;
  bool started = false;
  bool stopping = false;
  bool cleaned_up = false;
  bool writing = false;
  bool text_send_active = false;
  bool ended_notified = false;

  void deactivate_spawn_callback() {
    if (spawn_state != nullptr) {
      spawn_state->active = false;
      spawn_state->session = nullptr;
      spawn_state.reset();
    }
  }

  void remove_child_watch() {
    if (child_watch_id != 0) {
      g_source_remove(child_watch_id);
      child_watch_id = 0;
    }
  }

  void release_pty() {
    pty_fd = -1;
    if (pty != nullptr) {
      g_object_unref(pty);
      pty = nullptr;
    }
  }

  void terminate_child() {
    if (child_pid != 0) {
      terminate_child_noexcept(child_pid);
      g_spawn_close_pid(child_pid);
      child_pid = 0;
    }
  }

  void notify_ended_once() {
    if (ended_notified) {
      return;
    }

    ended_notified = true;
    notify_connection_phase(TerminalSessionConnectionPhase::disconnected);
    notify_session_ended(callbacks);
  }

  void notify_activity(ActivityIndicatorId indicator) {
    if (callbacks.activity) {
      callbacks.activity(indicator);
    }
  }

  void notify_connection_phase(TerminalSessionConnectionPhase phase) {
    if (callbacks.connection_phase) {
      callbacks.connection_phase(phase);
    }
  }

  void cleanup_session_resources() {
    if (cleaned_up) {
      return;
    }

    cleaned_up = true;
    deactivate_spawn_callback();
    terminal_io.disconnect_user_input();
    outgoing.clear();
    if (text_send_cancel_source.has_value()) {
      (void)text_send_cancel_source->cancel();
    }
    (void)stop_source.cancel();
    remove_child_watch();
    terminate_child();
    release_pty();
  }

  void finish_naturally() {
    if (stopping) {
      return;
    }

    stopping = true;
    cleanup_session_resources();
    notify_ended_once();
  }

  void set_current_pty_size() {
    if (pty == nullptr) {
      return;
    }

    const TerminalViewGridSize size = terminal_io.grid_size();
    set_pty_size(size.columns, size.rows);
  }

  void set_pty_size(glong columns, glong rows) {
    if (pty == nullptr || columns <= 0 || rows <= 0) {
      return;
    }

    GError *error = nullptr;
    if (!vte_pty_set_size(pty, clamp_pty_dimension(rows),
                          clamp_pty_dimension(columns), &error)) {
      if (!stopping) {
        std::cerr << "Warning: failed to resize local PTY: "
                  << gerror_message(error, "unknown error") << '\n';
      }
      clear_gerror(&error);
    }
  }

  void create_pty() {
    GError *error = nullptr;
    pty = vte_pty_new_sync(VTE_PTY_DEFAULT, nullptr, &error);
    if (pty == nullptr) {
      const std::string message =
          gerror_message(error, "failed to create local PTY");
      clear_gerror(&error);
      throw std::runtime_error(message);
    }
    pty_fd = vte_pty_get_fd(pty);
    if (pty_fd < 0) {
      throw std::runtime_error("local PTY returned an invalid fd");
    }
  }

  void start_writer() {
    if (writing || pty_fd < 0 || stopping || outgoing.empty()) {
      return;
    }

    writing = true;
    write_task.reset();
    write_task.emplace(write_loop_async());
  }

  void enqueue_bytes(std::span<const unsigned char> bytes) {
    if (bytes.empty() || stopping || pty_fd < 0) {
      return;
    }

    outgoing.emplace_back(bytes.begin(), bytes.end());
    start_writer();
  }

  void send_user_input(std::span<const unsigned char> bytes) {
    enqueue_bytes(bytes);
  }

  void handle_child_exited(int status) {
    (void)status;
    finish_naturally();
  }

  void handle_spawn_finished(GObject *source_object, GAsyncResult *result) {
    GError *error = nullptr;
    GPid spawned_pid = 0;
    if (!vte_pty_spawn_finish(VTE_PTY(source_object), result, &spawned_pid,
                              &error)) {
      if (!stopping) {
        std::cerr << "Failed to spawn shell: "
                  << gerror_message(error, "unknown error") << '\n';
      }
      clear_gerror(&error);
      if (!stopping) {
        finish_naturally();
      }
      return;
    }

    if (stopping) {
      terminate_child_noexcept(spawned_pid);
      g_spawn_close_pid(spawned_pid);
      return;
    }

    child_pid = spawned_pid;
    child_watch_id =
        g_child_watch_add(child_pid, TerminalLocalShellSession::on_child_exited,
                          this);
    notify_connection_phase(TerminalSessionConnectionPhase::connected);
  }

  static void finish_inactive_spawn(GObject *source_object,
                                    GAsyncResult *result) {
    GError *error = nullptr;
    GPid spawned_pid = 0;
    if (vte_pty_spawn_finish(VTE_PTY(source_object), result, &spawned_pid,
                             &error)) {
      terminate_child_noexcept(spawned_pid);
      g_spawn_close_pid(spawned_pid);
    }
    clear_gerror(&error);
  }

  static void on_pty_spawned(GObject *source_object, GAsyncResult *result,
                             gpointer user_data) {
    std::unique_ptr<std::shared_ptr<LocalShellSpawnState>> state_owner(
        static_cast<std::shared_ptr<LocalShellSpawnState> *>(user_data));
    const std::shared_ptr<LocalShellSpawnState> state = *state_owner;
    if (state == nullptr || !state->active || state->session == nullptr) {
      finish_inactive_spawn(source_object, result);
      return;
    }

    state->session->handle_spawn_finished(source_object, result);
  }

  static void on_child_exited(GPid pid, gint status, gpointer user_data) {
    auto *self = static_cast<TerminalLocalShellSession *>(user_data);
    self->child_watch_id = 0;
    if (self->child_pid == pid) {
      self->child_pid = 0;
    }
    g_spawn_close_pid(pid);
    self->handle_child_exited(status);
  }

  cardio::promise<void> read_loop_async() {
    bool natural_end = false;
    try {
      std::array<unsigned char, 4096> buffer{};
      while (!stopping && pty_fd >= 0) {
        std::span<unsigned char> writable(buffer.data(), buffer.size());
        const std::size_t read_size = co_await cardio::io_urings::read(
            *io, pty_fd, std::as_writable_bytes(writable));
        if (read_size == 0) {
          natural_end = true;
          break;
        }
        notify_activity(ActivityIndicatorId::rd);
        terminal_io.feed(std::span<const unsigned char>(buffer.data(),
                                                        read_size));
      }
    } catch (const cardio::canceled_exception &) {
    } catch (const std::system_error &error) {
      if (!stopping && error.code().value() == EIO) {
        natural_end = true;
      } else if (!stopping) {
        std::cerr << "Warning: local PTY read failed: " << error.what()
                  << '\n';
      }
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: local PTY read failed: " << error.what()
                  << '\n';
      }
    }

    if (natural_end) {
      finish_naturally();
    }
  }

  cardio::promise<void> write_loop_async() {
    try {
      while (!stopping && pty_fd >= 0 && !outgoing.empty()) {
        std::vector<unsigned char> chunk = std::move(outgoing.front());
        outgoing.pop_front();
        auto write_lock_promise =
            backend_write_mutex.lock(stop_source.get_cancellation());
        auto write_lock = std::move(co_await write_lock_promise);
        std::size_t offset = 0;
        while (!stopping && offset < chunk.size()) {
          std::span<const unsigned char> remaining(chunk.data() + offset,
                                                   chunk.size() - offset);
          const std::size_t written = co_await cardio::io_urings::write(
              *io, pty_fd, std::as_bytes(remaining),
              stop_source.get_cancellation());
          if (written == 0) {
            throw std::runtime_error("local PTY write made no progress");
          }
          notify_activity(ActivityIndicatorId::sd);
          offset += written;
        }
      }
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: local PTY write failed: " << error.what()
                  << '\n';
      }
    }

    writing = false;
    if (!stopping && pty_fd >= 0 && !outgoing.empty()) {
      start_writer();
    }
  }

  cardio::promise<void>
  send_text_bytes(std::span<const unsigned char> bytes,
                  cardio::cancellation cancellation) {
    if (!text_send_active || stopping || pty_fd < 0 || !io.has_value()) {
      throw std::runtime_error("local text send is not connected");
    }
    auto write_lock_promise = backend_write_mutex.lock(cancellation);
    auto write_lock = std::move(co_await write_lock_promise);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      cancellation.throw_if_cancellation_requested();
      if (!text_send_active || stopping || pty_fd < 0 || !io.has_value()) {
        throw std::runtime_error("local text send is not connected");
      }
      const std::span<const unsigned char> remaining(bytes.data() + offset,
                                                      bytes.size() - offset);
      const std::size_t written = co_await cardio::io_urings::write(
          *io, pty_fd, std::as_bytes(remaining), cancellation);
      if (written == 0) {
        throw std::runtime_error("local text send write made no progress");
      }
      notify_activity(ActivityIndicatorId::sd);
      offset += written;
    }
  }

  static cardio::promise<void>
  delay_text_send_async(std::uint64_t delay_us,
                        cardio::cancellation cancellation) {
    const std::uint64_t delay_ms =
        delay_us / 1000 + (delay_us % 1000 == 0 ? 0 : 1);
    co_await cardio::promises::delay(delay_ms, std::move(cancellation));
  }

  void finish_text_send(const TerminalTextSendRequest &request,
                        bool succeeded) {
    text_send_active = false;
    text_send_cancel_source.reset();
    if (request.status) {
      request.status(succeeded ? connection_detail() : _("Text send failed"));
    }
    if (!stopping && pty_fd >= 0) {
      terminal_io.connect_user_input(
          [this](std::span<const unsigned char> bytes) {
            send_user_input(bytes);
          });
    }
    if (request.active) {
      request.active(false);
    }
    if (request.finished) {
      request.finished(succeeded);
    }
  }

  cardio::promise<void>
  text_send_loop_async(TerminalTextSendRequest request,
                       TerminalTextSendTransport transport) {
    bool succeeded = false;
    try {
      TerminalTextSendRequest run_request = request;
      co_await run_terminal_text_send_async(
          std::move(run_request), std::move(transport),
          text_send_cancel_source->get_cancellation());
      succeeded = true;
    } catch (const cardio::canceled_exception &) {
      if (!stopping) {
        std::cerr << "Warning: local text send cancelled" << '\n';
      }
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: local text send failed: " << error.what()
                  << '\n';
      }
    }
    finish_text_send(request, succeeded);
  }

public:
  TerminalLocalShellSession(GtkWidget *terminal,
                            LocalShellConnectionSettings settings,
                            TerminalTextSettings text_settings,
                            TerminalSessionCallbacks callbacks)
      : terminal_io(terminal, text_settings, callbacks.output),
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

    notify_connection_phase(TerminalSessionConnectionPhase::connecting);
    try {
      io.emplace(64);
      create_pty();
      set_current_pty_size();
      terminal_io.connect_user_input(
          [this](std::span<const unsigned char> bytes) {
            send_user_input(bytes);
          });
      read_task.emplace(read_loop_async());

      std::string shell = shell_path();
      char *argv[] = {
          shell.data(),
          nullptr,
      };
      spawn_state = std::make_shared<LocalShellSpawnState>();
      spawn_state->session = this;
      vte_pty_spawn_async(
          pty, nullptr, argv, nullptr, static_cast<GSpawnFlags>(0), nullptr,
          nullptr, nullptr, -1, nullptr,
          TerminalLocalShellSession::on_pty_spawned,
          new std::shared_ptr<LocalShellSpawnState>(spawn_state));

      started = true;
      return true;
    } catch (const std::exception &error) {
      std::cerr << "Warning: failed to initialize local shell session: "
                << error.what() << '\n';
      stop();
      notify_connection_phase(TerminalSessionConnectionPhase::disconnected);
      return false;
    }
  }

  void stop() override {
    if (stopping && cleaned_up) {
      return;
    }

    stopping = true;
    cleanup_session_resources();
  }

  void resize(glong columns, glong rows) override {
    set_pty_size(columns, rows);
  }

  std::string connection_detail() const override {
    return _("local terminal");
  }

  void apply_connection_profile(
      const TerminalConnectionProfile &profile) override {
    (void)terminal_io.apply_text_settings(profile.text_settings);
  }

  bool supports_text_send() const override {
    return true;
  }

  bool transfer_in_progress() const override {
    return text_send_active;
  }

  bool cancel_transfer() override {
    if (!text_send_active || !text_send_cancel_source.has_value()) {
      return false;
    }
    (void)text_send_cancel_source->cancel();
    return true;
  }

  bool start_text_send(TerminalTextSendRequest request) override {
    if (!started || stopping || text_send_active || pty_fd < 0 ||
        !terminal_text_send_source_is_valid(request.source)) {
      return false;
    }

    text_send_cancel_source.emplace();
    text_send_active = true;
    terminal_io.disconnect_user_input();
    if (request.active) {
      request.active(true);
    }
    TerminalTextSendTransport transport{
        .send =
            [this](std::span<const unsigned char> bytes,
                   cardio::cancellation cancellation) {
              return send_text_bytes(bytes, std::move(cancellation));
            },
        .now_us = []() {
          return static_cast<std::uint64_t>(g_get_monotonic_time());
        },
        .delay = delay_text_send_async,
    };
    text_send_task.reset();
    text_send_task.emplace(
        text_send_loop_async(std::move(request), std::move(transport)));
    return true;
  }
};

std::unique_ptr<TerminalSession>
create_terminal_local_shell_session(GtkWidget *terminal,
                                    LocalShellConnectionSettings settings,
                                    TerminalTextSettings text_settings,
                                    TerminalSessionCallbacks callbacks) {
  return std::make_unique<TerminalLocalShellSession>(terminal,
                                                     std::move(settings),
                                                     std::move(text_settings),
                                                     callbacks);
}

} // namespace elder_terms

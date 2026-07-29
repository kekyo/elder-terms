#include "ssh-session.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <cardio.h>

#include "../../terminal-text-send-runner.h"
#include "../../terminal-transfer-runner.h"
#include "../../terminal-zmodem-auto-start.h"
#include "../terminal-view-io.h"
#include "ssh-channel-connection.h"

namespace elder_terms {

static int open_ssh_event_fd() {
  const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "eventfd failed");
  }
  return fd;
}

static void close_ssh_event_fd(int *fd) {
  if (*fd >= 0) {
    (void)::close(*fd);
    *fd = -1;
  }
}

static void notify_ssh_event_fd(int fd) {
  if (fd < 0) {
    return;
  }
  while (::eventfd_write(fd, 1) < 0 && errno == EINTR) {
  }
}

static void drain_ssh_event_fd(int fd) {
  if (fd < 0) {
    return;
  }
  eventfd_t value = 0;
  while (::eventfd_read(fd, &value) < 0 && errno == EINTR) {
  }
  while (::eventfd_read(fd, &value) == 0) {
  }
}

static std::uint64_t ssh_monotonic_milliseconds() {
  return static_cast<std::uint64_t>(g_get_monotonic_time() / 1000);
}

class TerminalSshSession final : public TerminalSession {
private:
  TerminalViewIo terminal_io;
  SshConnectionSettings settings;
  TerminalSessionCallbacks callbacks;
  SshChannelConnectionOptions connection_options;
  cardio::cancellation_source stop_source;
  cardio::primitives::mutex backend_write_mutex;
  std::unique_ptr<SshChannelConnection> connection;
  std::deque<std::vector<unsigned char>> outgoing;
  std::deque<unsigned char> transfer_incoming;
  std::optional<cardio::cancellation_source> transfer_cancel_source;
  std::optional<cardio::cancellation_source> text_send_cancel_source;
  std::optional<cardio::promise<void>> session_task;
  std::optional<cardio::promise<void>> write_task;
  std::optional<cardio::promise<void>> transfer_task;
  std::optional<cardio::promise<void>> text_send_task;
  std::optional<cardio::promise<void>> resize_task;
  TerminalZmodemAutoStartDetectorState zmodem_auto_start_detector;
  int transfer_input_event_fd = -1;
  glong columns = 80;
  glong rows = 24;
  bool started = false;
  bool stopping = false;
  bool connected = false;
  bool writing = false;
  bool resize_running = false;
  bool resize_pending = false;
  bool transfer_active = false;
  bool text_send_active = false;
  bool zmodem_autostart_enabled = false;

  void notify_connection_phase(TerminalSessionConnectionPhase phase) {
    if (callbacks.connection_phase) {
      callbacks.connection_phase(phase);
    }
  }

  void notify_failure(const std::string &message) {
    if (callbacks.failure) {
      callbacks.failure(message);
    }
  }

  void notify_activity(ActivityIndicatorId indicator) {
    if (callbacks.activity) {
      callbacks.activity(indicator);
    }
  }

  void notify_ended() {
    if (callbacks.ended) {
      callbacks.ended();
    }
  }

  void cancel_modem_transfer_noexcept() {
    if (transfer_cancel_source.has_value()) {
      (void)transfer_cancel_source->cancel();
    }
    notify_ssh_event_fd(transfer_input_event_fd);
  }

  void cancel_text_send() {
    if (text_send_cancel_source.has_value()) {
      (void)text_send_cancel_source->cancel();
    }
  }

  void enqueue_bytes(std::span<const unsigned char> bytes) {
    if (bytes.empty() || stopping || !connected) {
      return;
    }
    outgoing.emplace_back(bytes.begin(), bytes.end());
    start_writer();
  }

  void start_writer() {
    if (writing || stopping || !connected || connection == nullptr ||
        outgoing.empty()) {
      return;
    }
    writing = true;
    write_task.reset();
    write_task.emplace(write_loop_async());
  }

  cardio::promise<void> write_loop_async() {
    try {
      while (!stopping && connected && connection != nullptr &&
             !outgoing.empty()) {
        std::vector<unsigned char> bytes = std::move(outgoing.front());
        outgoing.pop_front();
        auto lock_promise =
            backend_write_mutex.lock(stop_source.get_cancellation());
        auto lock = std::move(co_await lock_promise);
        co_await connection->write_all_async(
            std::span<const unsigned char>(bytes.data(), bytes.size()),
            stop_source.get_cancellation());
        notify_activity(ActivityIndicatorId::sd);
      }
    } catch (const cardio::canceled_exception &) {
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: SSH write failed: " << error.what() << '\n';
        if (connection != nullptr) {
          connection->close();
        }
      }
    }
    writing = false;
    if (!stopping && connected && connection != nullptr &&
        !outgoing.empty()) {
      start_writer();
    }
  }

  void append_transfer_input(std::span<const unsigned char> bytes) {
    if (bytes.empty()) {
      return;
    }
    transfer_incoming.insert(transfer_incoming.end(), bytes.begin(),
                             bytes.end());
    notify_ssh_event_fd(transfer_input_event_fd);
  }

  bool handle_zmodem_auto_start(std::span<const unsigned char> bytes) {
    if (text_send_active || !zmodem_autostart_enabled ||
        !callbacks.zmodem_auto_start || bytes.empty()) {
      return false;
    }
    const std::string_view payload(
        reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const std::optional<TerminalTransferDirection> direction =
        feed_terminal_zmodem_auto_start_detector(
            &zmodem_auto_start_detector, payload);
    if (!direction.has_value()) {
      return false;
    }
    callbacks.zmodem_auto_start(*direction);
    return true;
  }

  void handle_received(std::span<const unsigned char> bytes) {
    if (transfer_active) {
      append_transfer_input(bytes);
      return;
    }
    if (!handle_zmodem_auto_start(bytes)) {
      terminal_io.feed(bytes);
    }
  }

  cardio::promise<void> session_loop_async() {
    bool natural_end = false;
    try {
      auto connect_promise = SshChannelConnection::connect_async(
          settings, columns, rows, callbacks, connection_options,
          stop_source.get_cancellation());
      connection = std::move(co_await connect_promise);
      if (stopping) {
        connection->close();
        connection.reset();
        co_return;
      }
      connected = true;
      notify_connection_phase(TerminalSessionConnectionPhase::connected);
      terminal_io.connect_user_input(
          [this](std::span<const unsigned char> bytes) {
            send_user_input(bytes);
          });

      std::array<unsigned char, 4096> buffer{};
      while (!stopping && connection != nullptr) {
        const std::size_t read_size = co_await connection->read_async(
            std::span<unsigned char>(buffer.data(), buffer.size()),
            stop_source.get_cancellation());
        if (read_size == 0) {
          natural_end = true;
          break;
        }
        notify_activity(ActivityIndicatorId::rd);
        handle_received(std::span<const unsigned char>(buffer.data(),
                                                       read_size));
      }
    } catch (const cardio::canceled_exception &) {
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: SSH session failed: " << error.what() << '\n';
        notify_failure(error.what());
        natural_end = true;
      }
    }

    const bool should_notify_ended = natural_end && !stopping;
    stopping = true;
    connected = false;
    cancel_modem_transfer_noexcept();
    cancel_text_send();
    terminal_io.disconnect_user_input();
    outgoing.clear();
    if (connection != nullptr) {
      connection->close();
      connection.reset();
    }
    if (should_notify_ended) {
      notify_connection_phase(TerminalSessionConnectionPhase::disconnected);
      notify_ended();
    }
  }

  cardio::promise<void> resize_loop_async() {
    try {
      while (!stopping && connected && connection != nullptr &&
             resize_pending) {
        resize_pending = false;
        const glong next_columns = columns;
        const glong next_rows = rows;
        co_await connection->resize_async(
            next_columns, next_rows, stop_source.get_cancellation());
      }
    } catch (const cardio::canceled_exception &) {
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: SSH resize failed: " << error.what() << '\n';
      }
    }
    resize_running = false;
    if (!stopping && connected && connection != nullptr &&
        resize_pending) {
      start_resize();
    }
  }

  void start_resize() {
    if (resize_running || stopping || !connected || connection == nullptr) {
      return;
    }
    resize_running = true;
    resize_task.reset();
    resize_task.emplace(resize_loop_async());
  }

  cardio::promise<void>
  send_transfer_bytes(std::span<const std::uint8_t> bytes,
                      std::uint32_t timeout_ms, std::size_t &written_len,
                      cardio::cancellation cancellation) {
    if (!transfer_active || stopping || !connected || connection == nullptr) {
      throw xyzm_async_io_error("SSH transfer is not connected");
    }
    if (bytes.empty()) {
      written_len = 0;
      co_return;
    }
    if (timeout_ms == 0) {
      throw xyzm_async_timeout_error("SSH transfer send timeout");
    }

    cardio::cancellation_source timeout_source =
        cardio::cancellations::timeout(timeout_ms);
    cardio::cancellation_source combined =
        transfer_cancel_source.has_value()
            ? cardio::cancellations::any(
                  cancellation, transfer_cancel_source->get_cancellation(),
                  timeout_source.get_cancellation())
            : cardio::cancellations::any(
                  cancellation, timeout_source.get_cancellation());
    try {
      auto lock_promise =
          backend_write_mutex.lock(combined.get_cancellation());
      auto lock = std::move(co_await lock_promise);
      co_await connection->write_all_async(
          std::span<const unsigned char>(bytes.data(), bytes.size()),
          combined.get_cancellation());
    } catch (const cardio::canceled_exception &) {
      if (cancellation.is_cancellation_requested() ||
          (transfer_cancel_source.has_value() &&
           transfer_cancel_source->get_cancellation()
               .is_cancellation_requested())) {
        throw xyzm_async_cancelled_error("SSH transfer cancelled");
      }
      throw xyzm_async_timeout_error("SSH transfer send timeout");
    }
    notify_activity(ActivityIndicatorId::sd);
    written_len = bytes.size();
  }

  cardio::promise<void>
  wait_transfer_input_async(std::uint32_t timeout_ms,
                            std::uint64_t start_ms,
                            cardio::cancellation cancellation) {
    if (timeout_ms == 0 ||
        ssh_monotonic_milliseconds() - start_ms >= timeout_ms) {
      throw xyzm_async_timeout_error("SSH transfer receive timeout");
    }
    const std::uint64_t elapsed =
        ssh_monotonic_milliseconds() - start_ms;
    const std::uint64_t remaining =
        elapsed >= timeout_ms ? 1 : timeout_ms - elapsed;
    cardio::cancellation_source timeout_source =
        cardio::cancellations::timeout(
            static_cast<std::uint32_t>(remaining));
    cardio::cancellation_source combined =
        transfer_cancel_source.has_value()
            ? cardio::cancellations::any(
                  cancellation, transfer_cancel_source->get_cancellation(),
                  timeout_source.get_cancellation())
            : cardio::cancellations::any(
                  cancellation, timeout_source.get_cancellation());

    drain_ssh_event_fd(transfer_input_event_fd);
    try {
      co_await cardio::from_fd(transfer_input_event_fd,
                               cardio::fd_event::read,
                               combined.get_cancellation());
      drain_ssh_event_fd(transfer_input_event_fd);
    } catch (const cardio::canceled_exception &) {
      if (cancellation.is_cancellation_requested() ||
          (transfer_cancel_source.has_value() &&
           transfer_cancel_source->get_cancellation()
               .is_cancellation_requested())) {
        throw xyzm_async_cancelled_error("SSH transfer cancelled");
      }
      throw xyzm_async_timeout_error("SSH transfer receive timeout");
    }
  }

  cardio::promise<void>
  receive_transfer_bytes(std::span<std::uint8_t> bytes,
                         std::uint32_t timeout_ms, std::size_t &read_len,
                         cardio::cancellation cancellation) {
    if (bytes.empty()) {
      read_len = 0;
      co_return;
    }
    const std::uint64_t start_ms = ssh_monotonic_milliseconds();
    while (transfer_incoming.empty()) {
      if (!transfer_active || stopping || !connected ||
          connection == nullptr) {
        throw xyzm_async_io_error("SSH transfer is not connected");
      }
      cancellation.throw_if_cancellation_requested();
      co_await wait_transfer_input_async(timeout_ms, start_ms,
                                         cancellation);
    }
    read_len = std::min(bytes.size(), transfer_incoming.size());
    for (std::size_t index = 0; index < read_len; ++index) {
      bytes[index] = transfer_incoming.front();
      transfer_incoming.pop_front();
    }
  }

  void reconnect_user_input() {
    if (!stopping && connected && connection != nullptr &&
        !transfer_active && !text_send_active) {
      terminal_io.connect_user_input(
          [this](std::span<const unsigned char> bytes) {
            send_user_input(bytes);
          });
    }
  }

  void finish_transfer(const TerminalTransferRequest &request,
                       bool succeeded) {
    transfer_active = false;
    transfer_incoming.clear();
    transfer_cancel_source.reset();
    if (request.status) {
      request.status(succeeded ? connection_detail() : "Transfer failed");
    }
    reconnect_user_input();
    if (request.active) {
      request.active(false);
    }
    if (request.finished) {
      request.finished(succeeded);
    }
  }

  cardio::promise<void>
  transfer_loop_async(TerminalTransferRequest request,
                      TerminalTransferTransport transport) {
    bool succeeded = false;
    try {
      TerminalTransferRequest run_request = request;
      co_await run_terminal_transfer_async(
          std::move(run_request), std::move(transport),
          transfer_cancel_source->get_cancellation());
      succeeded = true;
    } catch (const xyzm_async_cancelled_error &) {
      if (!stopping) {
        std::cerr << "Warning: SSH transfer cancelled" << '\n';
      }
    } catch (const cardio::canceled_exception &) {
      if (!stopping) {
        std::cerr << "Warning: SSH transfer cancelled" << '\n';
      }
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: SSH transfer failed: " << error.what()
                  << '\n';
      }
    }
    finish_transfer(request, succeeded);
  }

  cardio::promise<void>
  send_text_bytes(std::span<const unsigned char> bytes,
                  cardio::cancellation cancellation) {
    if (!text_send_active || stopping || !connected ||
        connection == nullptr) {
      throw std::runtime_error("SSH text send is not connected");
    }
    auto lock_promise = backend_write_mutex.lock(cancellation);
    auto lock = std::move(co_await lock_promise);
    co_await connection->write_all_async(bytes, cancellation);
    notify_activity(ActivityIndicatorId::sd);
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
      request.status(succeeded ? connection_detail() : "Text send failed");
    }
    reconnect_user_input();
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
        std::cerr << "Warning: SSH text send cancelled" << '\n';
      }
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: SSH text send failed: " << error.what()
                  << '\n';
      }
    }
    finish_text_send(request, succeeded);
  }

  void send_user_input(std::span<const unsigned char> bytes) {
    if (bytes.empty() || transfer_active || text_send_active) {
      return;
    }
    enqueue_bytes(bytes);
  }

public:
  TerminalSshSession(GtkWidget *terminal, SshConnectionSettings settings,
                     TerminalTextSettings text_settings,
                     TerminalSessionCallbacks callbacks,
                     SshChannelConnectionOptions connection_options)
      : terminal_io(terminal, text_settings, callbacks.output),
        settings(std::move(settings)), callbacks(std::move(callbacks)),
        connection_options(std::move(connection_options)) {
    const TerminalViewGridSize size = terminal_io.grid_size();
    if (size.columns > 0) {
      columns = size.columns;
    }
    if (size.rows > 0) {
      rows = size.rows;
    }
  }

  ~TerminalSshSession() override {
    stop();
    close_ssh_event_fd(&transfer_input_event_fd);
  }

  bool start() override {
    if (started) {
      return true;
    }
    if (settings.endpoint.address.empty()) {
      return false;
    }
    notify_connection_phase(TerminalSessionConnectionPhase::connecting);
    try {
      transfer_input_event_fd = open_ssh_event_fd();
      session_task.emplace(session_loop_async());
      started = true;
      return true;
    } catch (const std::exception &error) {
      std::cerr << "Warning: failed to initialize SSH session: "
                << error.what() << '\n';
      notify_failure(error.what());
      stop();
      notify_connection_phase(TerminalSessionConnectionPhase::disconnected);
      return false;
    }
  }

  void stop() override {
    if (stopping) {
      return;
    }
    stopping = true;
    connected = false;
    cancel_modem_transfer_noexcept();
    cancel_text_send();
    terminal_io.disconnect_user_input();
    outgoing.clear();
    (void)stop_source.cancel();
  }

  void resize(glong next_columns, glong next_rows) override {
    if (next_columns > 0) {
      columns = next_columns;
    }
    if (next_rows > 0) {
      rows = next_rows;
    }
    if (connected && connection != nullptr) {
      resize_pending = true;
      start_resize();
    }
  }

  std::string connection_detail() const override {
    if (settings.endpoint.address.empty()) {
      return "ssh: (unknown)";
    }
    std::string endpoint = settings.endpoint.address + ":" +
                           std::to_string(settings.endpoint.port);
    if (!settings.endpoint.username.empty()) {
      endpoint = settings.endpoint.username + "@" + endpoint;
    }
    return "ssh: " + endpoint;
  }

  bool supports_transfer() const override {
    return true;
  }

  bool supports_text_send() const override {
    return true;
  }

  bool transfer_in_progress() const override {
    return transfer_active || text_send_active;
  }

  bool cancel_transfer() override {
    if (!transfer_active && !text_send_active) {
      return false;
    }
    cancel_modem_transfer_noexcept();
    cancel_text_send();
    return true;
  }

  void set_zmodem_autostart(bool enabled) override {
    if (zmodem_autostart_enabled == enabled) {
      return;
    }
    zmodem_autostart_enabled = enabled;
    zmodem_auto_start_detector = {};
  }

  std::shared_ptr<AuthenticatedSshTransport>
  authenticated_ssh_transport() const override {
    if (!started || stopping || !connected || connection == nullptr) {
      return nullptr;
    }
    return connection->authenticated_transport();
  }

  bool start_transfer(TerminalTransferRequest request) override {
    if (!started || stopping || !connected || connection == nullptr ||
        transfer_active || text_send_active) {
      return false;
    }
    if (request.direction == TerminalTransferDirection::send &&
        request.source_file_uris.empty()) {
      return false;
    }

    transfer_cancel_source.emplace();
    transfer_incoming.clear();
    transfer_active = true;
    terminal_io.disconnect_user_input();
    outgoing.clear();
    if (request.active) {
      request.active(true);
    }
    TerminalTransferTransport transport{
        .send =
            [this](std::span<const std::uint8_t> bytes,
                   std::uint32_t timeout_ms, std::size_t &written_len,
                   cardio::cancellation cancellation) {
              return send_transfer_bytes(bytes, timeout_ms, written_len,
                                         std::move(cancellation));
            },
        .recv =
            [this](std::span<std::uint8_t> bytes,
                   std::uint32_t timeout_ms, std::size_t &read_len,
                   cardio::cancellation cancellation) {
              return receive_transfer_bytes(bytes, timeout_ms, read_len,
                                            std::move(cancellation));
            },
        .now_ms = []() { return ssh_monotonic_milliseconds(); },
    };
    transfer_task.reset();
    transfer_task.emplace(
        transfer_loop_async(std::move(request), std::move(transport)));
    return true;
  }

  bool start_text_send(TerminalTextSendRequest request) override {
    if (!started || stopping || !connected || connection == nullptr ||
        transfer_active || text_send_active ||
        !terminal_text_send_source_is_valid(request.source)) {
      return false;
    }

    text_send_cancel_source.emplace();
    text_send_active = true;
    zmodem_auto_start_detector = {};
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

  void apply_connection_profile(
      const TerminalConnectionProfile &profile) override {
    (void)terminal_io.apply_text_settings(profile.text_settings);
  }
};

std::unique_ptr<TerminalSession>
create_terminal_ssh_session(GtkWidget *terminal,
                            SshConnectionSettings settings,
                            TerminalTextSettings text_settings,
                            TerminalSessionCallbacks callbacks,
                            SshChannelConnectionOptions connection_options) {
  return std::make_unique<TerminalSshSession>(
      terminal, std::move(settings), std::move(text_settings),
      std::move(callbacks), std::move(connection_options));
}

} // namespace elder_terms

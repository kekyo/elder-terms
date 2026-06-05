#include "serial-session.h"

#include <fcntl.h>
#include <unistd.h>

#include <glib-unix.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "../terminal-view-io.h"
#include "../../terminal-transfer-runner.h"
#include "serial-device-resolver.h"
#include "serial-device-event-monitor.h"
#include "serial-line-monitor.h"
#include "serial-termios.h"

namespace elder_terms {

static void close_fd_noexcept(int *fd) {
  if (*fd >= 0) {
    (void)::close(*fd);
    *fd = -1;
  }
}

static void remove_source(guint *source_id) {
  if (*source_id != 0) {
    g_source_remove(*source_id);
    *source_id = 0;
  }
}

static void notify_session_ended(const TerminalSessionCallbacks &callbacks) {
  if (callbacks.ended) {
    callbacks.ended();
  }
}

static std::uint64_t monotonic_milliseconds() {
  return static_cast<std::uint64_t>(g_get_monotonic_time() / 1000);
}

static bool serial_line_error_indicates_connection_lost(
    const std::system_error &error) {
  const int code = error.code().value();
  return code == EIO || code == ENODEV || code == EBADF;
}

static bool selected_carrier_signal_is_high(SerialLineSignals signals,
                                            SerialCarrierDetect carrier_detect) {
  if (carrier_detect == SerialCarrierDetect::cts) {
    return signals.cts;
  }
  if (carrier_detect == SerialCarrierDetect::dsr) {
    return signals.dsr;
  }
  return signals.cd;
}

class TerminalSerialSession final : public TerminalSession {
private:
  TerminalViewIo terminal_io;
  SerialConnectionSettings settings;
  TerminalSessionCallbacks callbacks;
  SerialCarrierTracker carrier_tracker;
  std::deque<std::vector<unsigned char>> outgoing;
  std::unique_ptr<SerialDeviceEventMonitor> device_event_monitor;
  std::optional<cardio::cancellation_source> transfer_cancel_source;
  std::optional<cardio::promise<void>> transfer_task;
  int serial_fd = -1;
  guint read_watch_id = 0;
  guint write_watch_id = 0;
  guint carrier_poll_id = 0;
  guint reconnect_poll_id = 0;
  guint ended_idle_id = 0;
  bool started = false;
  bool stopping = false;
  bool line_warning_reported = false;
  bool connection_warning_reported = false;
  bool disconnected_reported = false;
  bool carrier_disconnected = false;
  bool transfer_active = false;
  std::deque<unsigned char> transfer_incoming;

  void remove_connection_sources() {
    remove_source(&read_watch_id);
    remove_source(&write_watch_id);
    remove_source(&carrier_poll_id);
  }

  void remove_sources() {
    remove_connection_sources();
    remove_source(&reconnect_poll_id);
    remove_source(&ended_idle_id);
  }

  void feed_terminal(const unsigned char *data, std::size_t size) {
    terminal_io.feed(std::span<const unsigned char>(data, size));
  }

  void append_transfer_input(const unsigned char *data, std::size_t size) {
    transfer_incoming.insert(transfer_incoming.end(), data, data + size);
  }

  void notify_activity(ActivityIndicatorId indicator) {
    if (callbacks.activity) {
      callbacks.activity(indicator);
    }
  }

  void notify_indicator_state(ActivityIndicatorId indicator, bool active) {
    if (callbacks.indicator_state) {
      callbacks.indicator_state(indicator, active);
    }
  }

  void notify_serial_line_state(SerialLineSignals signals) {
    for (ActivityIndicatorState state : serial_line_indicator_states(signals)) {
      notify_indicator_state(state.indicator, state.active);
    }
  }

  void notify_connected_state(bool active) {
    notify_indicator_state(ActivityIndicatorId::conn, active);
  }

  void schedule_disconnected_notification() {
    if (disconnected_reported || stopping) {
      return;
    }

    disconnected_reported = true;
    if (ended_idle_id == 0) {
      ended_idle_id = g_idle_add(TerminalSerialSession::on_session_ended_idle,
                                 this);
    }
  }

  void start_writer() {
    if (stopping || serial_fd < 0 || write_watch_id != 0 || outgoing.empty()) {
      return;
    }

    write_watch_id = g_unix_fd_add(
        serial_fd,
        static_cast<GIOCondition>(G_IO_OUT | G_IO_ERR | G_IO_HUP | G_IO_NVAL),
        TerminalSerialSession::on_write_ready, this);
  }

  void stop_device_monitor() {
    device_event_monitor.reset();
  }

  void stop_reconnect_poll() {
    remove_source(&reconnect_poll_id);
  }

  void start_reconnect_poll() {
    if (stopping || serial_fd >= 0 || reconnect_poll_id != 0) {
      return;
    }

    reconnect_poll_id =
        g_timeout_add(250, TerminalSerialSession::on_reconnect_poll, this);
  }

  bool connect_if_available() {
    const bool connected = attempt_connect();
    if (connected) {
      stop_device_monitor();
      stop_reconnect_poll();
    }
    return connected;
  }

  void handle_device_event() {
    if (stopping || serial_fd >= 0) {
      return;
    }

    if (!connect_if_available()) {
      start_reconnect_poll();
    }
  }

  void start_device_monitor() {
    if (stopping || serial_fd >= 0 || device_event_monitor != nullptr) {
      return;
    }

    device_event_monitor = std::make_unique<SerialDeviceEventMonitor>(
        settings.device, [this]() { handle_device_event(); });
    device_event_monitor->start();
  }

  void handle_device_connection_lost() {
    if (stopping) {
      return;
    }

    cancel_transfer_noexcept();
    remove_connection_sources();
    outgoing.clear();
    close_fd_noexcept(&serial_fd);
    carrier_tracker = SerialCarrierTracker(settings.carrier_detect);
    carrier_disconnected = false;
    notify_serial_line_state({});
    notify_connected_state(false);
    schedule_disconnected_notification();
    start_device_monitor();
    if (!connect_if_available()) {
      start_reconnect_poll();
    }
  }

  void handle_carrier_disconnected() {
    if (stopping || carrier_disconnected) {
      return;
    }

    cancel_transfer_noexcept();
    outgoing.clear();
    carrier_disconnected = true;
    notify_connected_state(false);
    schedule_disconnected_notification();
  }

  void handle_carrier_connected() {
    if (!carrier_disconnected) {
      return;
    }

    carrier_disconnected = false;
    disconnected_reported = false;
    notify_connected_state(true);
  }

  bool attempt_connect() {
    if (stopping || serial_fd >= 0) {
      return serial_fd >= 0;
    }

    const SerialDeviceResolveResult resolve_result =
        resolve_serial_device(settings.device);
    if (!resolve_result.resolved) {
      if (!connection_warning_reported) {
        for (const std::string &warning : resolve_result.warnings) {
          std::cerr << warning << '\n';
        }
        connection_warning_reported = true;
      }
      return false;
    }

    try {
      serial_fd = ::open(resolve_result.path.c_str(),
                         O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
      if (serial_fd < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "serial open failed");
      }

      configure_serial_port(serial_fd, settings);
      read_watch_id = g_unix_fd_add(
          serial_fd,
          static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL),
          TerminalSerialSession::on_read_ready, this);
      carrier_poll_id = g_timeout_add(100,
                                      TerminalSerialSession::on_carrier_poll,
                                      this);
      carrier_tracker = SerialCarrierTracker(settings.carrier_detect);
      line_warning_reported = false;
      connection_warning_reported = false;
      disconnected_reported = false;
      carrier_disconnected = false;
      notify_connected_state(true);
      start_writer();
      return true;
    } catch (const std::exception &error) {
      if (!connection_warning_reported) {
        std::cerr << "Warning: failed to initialize serial session: "
                  << error.what() << '\n';
        connection_warning_reported = true;
      }
      close_fd_noexcept(&serial_fd);
      return false;
    }
  }

  bool handle_read_ready(GIOCondition condition) {
    if ((condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0) {
      handle_device_connection_lost();
      return false;
    }

    if ((condition & G_IO_IN) == 0) {
      return !stopping;
    }

    std::array<unsigned char, 4096> buffer{};
    while (!stopping) {
      const ssize_t read_size =
          ::read(serial_fd, buffer.data(), buffer.size());
      if (read_size > 0) {
        notify_activity(ActivityIndicatorId::rd);
        if (transfer_active) {
          append_transfer_input(buffer.data(),
                                static_cast<std::size_t>(read_size));
        } else {
          feed_terminal(buffer.data(), static_cast<std::size_t>(read_size));
        }
        continue;
      }
      if (read_size == 0) {
        return true;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
      }

      std::cerr << "Warning: serial read failed: " << std::strerror(errno)
                << '\n';
      handle_device_connection_lost();
      return false;
    }

    return false;
  }

  bool handle_write_ready(GIOCondition condition) {
    if ((condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0) {
      handle_device_connection_lost();
      return false;
    }

    while (!stopping && serial_fd >= 0 && !outgoing.empty()) {
      std::vector<unsigned char> &chunk = outgoing.front();
      if (chunk.empty()) {
        outgoing.pop_front();
        continue;
      }

      const ssize_t written = ::write(serial_fd, chunk.data(), chunk.size());
      if (written > 0) {
        notify_activity(ActivityIndicatorId::sd);
        chunk.erase(chunk.begin(),
                    chunk.begin() + static_cast<std::size_t>(written));
        continue;
      }
      if (written == 0) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
      }

      std::cerr << "Warning: serial write failed: " << std::strerror(errno)
                << '\n';
      handle_device_connection_lost();
      return false;
    }

    return !outgoing.empty() && !stopping;
  }

  bool handle_carrier_poll() {
    if (stopping || serial_fd < 0) {
      return false;
    }

    try {
      const SerialLineSignals signals = read_serial_line_signals(serial_fd);
      notify_serial_line_state(signals);
      if (selected_carrier_signal_is_high(signals, settings.carrier_detect)) {
        handle_carrier_connected();
      }
      if (carrier_tracker.update(signals) ==
          SerialCarrierEvent::disconnected) {
        handle_carrier_disconnected();
      }
    } catch (const std::system_error &error) {
      if (serial_line_error_indicates_connection_lost(error)) {
        std::cerr << "Warning: serial carrier detection failed: "
                  << error.what() << '\n';
        notify_serial_line_state({});
        handle_device_connection_lost();
        return false;
      }

      notify_serial_line_state({});
      if (!line_warning_reported) {
        std::cerr << "Warning: serial carrier detection unavailable: "
                  << error.what() << '\n';
        line_warning_reported = true;
      }
    } catch (const std::exception &error) {
      notify_serial_line_state({});
      if (!line_warning_reported) {
        std::cerr << "Warning: serial carrier detection unavailable: "
                  << error.what() << '\n';
        line_warning_reported = true;
      }
    }

    return true;
  }

  bool handle_reconnect_poll() {
    if (stopping || serial_fd >= 0) {
      return false;
    }

    start_device_monitor();
    if (connect_if_available()) {
      return false;
    }
    return true;
  }

  static gboolean on_read_ready(gint, GIOCondition condition,
                                gpointer user_data) {
    auto *self = static_cast<TerminalSerialSession *>(user_data);
    const bool keep = self->handle_read_ready(condition);
    if (!keep) {
      self->read_watch_id = 0;
    }
    return keep ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
  }

  static gboolean on_write_ready(gint, GIOCondition condition,
                                 gpointer user_data) {
    auto *self = static_cast<TerminalSerialSession *>(user_data);
    const bool keep = self->handle_write_ready(condition);
    if (!keep) {
      self->write_watch_id = 0;
    }
    return keep ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
  }

  static gboolean on_carrier_poll(gpointer user_data) {
    auto *self = static_cast<TerminalSerialSession *>(user_data);
    const bool keep = self->handle_carrier_poll();
    if (!keep) {
      self->carrier_poll_id = 0;
    }
    return keep ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
  }

  static gboolean on_reconnect_poll(gpointer user_data) {
    auto *self = static_cast<TerminalSerialSession *>(user_data);
    const bool keep = self->handle_reconnect_poll();
    if (!keep) {
      self->reconnect_poll_id = 0;
    }
    return keep ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
  }

  static gboolean on_session_ended_idle(gpointer user_data) {
    auto *self = static_cast<TerminalSerialSession *>(user_data);
    self->ended_idle_id = 0;
    if (!self->stopping) {
      notify_session_ended(self->callbacks);
    }
    return G_SOURCE_REMOVE;
  }

  void send_user_input(std::span<const unsigned char> bytes) {
    if (bytes.empty() || stopping || serial_fd < 0 ||
        carrier_disconnected || transfer_active) {
      return;
    }

    outgoing.emplace_back(bytes.begin(), bytes.end());
    start_writer();
  }

  void cancel_transfer_noexcept() {
    if (transfer_cancel_source.has_value()) {
      (void)transfer_cancel_source->cancel();
    }
  }

  cardio::promise<void>
  send_transfer_bytes(std::span<const std::uint8_t> bytes,
                      std::uint32_t timeout_ms, std::size_t &written_len,
                      cardio::cancellation cancellation) {
    cancellation.throw_if_cancellation_requested();
    if (!transfer_active || stopping || serial_fd < 0 ||
        carrier_disconnected) {
      throw xyzm_async_io_error("serial transfer is not connected");
    }
    if (bytes.empty()) {
      written_len = 0;
      co_return;
    }

    written_len = 0;
    const std::uint64_t start_ms = monotonic_milliseconds();
    while (written_len < bytes.size()) {
      if (!transfer_active || stopping || serial_fd < 0 ||
          carrier_disconnected) {
        throw xyzm_async_io_error("serial transfer is not connected");
      }
      cancellation.throw_if_cancellation_requested();

      const ssize_t written =
          ::write(serial_fd, bytes.data() + written_len,
                  bytes.size() - written_len);
      if (written > 0) {
        notify_activity(ActivityIndicatorId::sd);
        written_len += static_cast<std::size_t>(written);
        co_return;
      }
      if (written == 0) {
        throw xyzm_async_io_error("serial transfer write made no progress");
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        throw xyzm_async_io_error(
            std::string("serial transfer write failed: ") +
            std::strerror(errno));
      }

      if (timeout_ms == 0 ||
          monotonic_milliseconds() - start_ms >= timeout_ms) {
        throw xyzm_async_timeout_error("serial transfer send timeout");
      }
      const std::uint64_t elapsed = monotonic_milliseconds() - start_ms;
      const std::uint64_t remaining =
          elapsed >= timeout_ms ? 1 : timeout_ms - elapsed;
      try {
        co_await cardio::promises::delay(
            std::min<std::uint64_t>(10, remaining), cancellation);
      } catch (const cardio::canceled_exception &) {
        throw xyzm_async_cancelled_error("serial transfer cancelled");
      }
    }
  }

  cardio::promise<void>
  recv_transfer_bytes(std::span<std::uint8_t> bytes,
                      std::uint32_t timeout_ms, std::size_t &read_len,
                      cardio::cancellation cancellation) {
    if (bytes.empty()) {
      read_len = 0;
      co_return;
    }

    const std::uint64_t start_ms = monotonic_milliseconds();
    while (transfer_incoming.empty()) {
      if (!transfer_active || stopping || serial_fd < 0 ||
          carrier_disconnected) {
        throw xyzm_async_io_error("serial transfer is not connected");
      }
      cancellation.throw_if_cancellation_requested();
      if (timeout_ms == 0 ||
          monotonic_milliseconds() - start_ms >= timeout_ms) {
        throw xyzm_async_timeout_error("serial transfer receive timeout");
      }

      const std::uint64_t elapsed = monotonic_milliseconds() - start_ms;
      const std::uint64_t remaining =
          elapsed >= timeout_ms ? 1 : timeout_ms - elapsed;
      const std::uint64_t delay_ms = std::min<std::uint64_t>(10, remaining);
      cardio::cancellation_source combined =
          transfer_cancel_source.has_value()
              ? cardio::cancellations::any(
                    cancellation,
                    transfer_cancel_source->get_cancellation())
              : cardio::cancellations::any(cancellation);
      try {
        co_await cardio::promises::delay(delay_ms,
                                         combined.get_cancellation());
      } catch (const cardio::canceled_exception &) {
        throw xyzm_async_cancelled_error("serial transfer cancelled");
      }
    }

    read_len = std::min(bytes.size(), transfer_incoming.size());
    for (std::size_t index = 0; index < read_len; ++index) {
      bytes[index] = transfer_incoming.front();
      transfer_incoming.pop_front();
    }
  }

  void finish_transfer(const TerminalTransferRequest &request,
                       bool succeeded) {
    transfer_active = false;
    transfer_incoming.clear();
    transfer_cancel_source.reset();
    if (request.status) {
      request.status("Terminal");
    }
    if (!stopping && serial_fd >= 0 && !carrier_disconnected) {
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
        std::cerr << "Warning: serial transfer cancelled" << '\n';
      }
    } catch (const cardio::canceled_exception &) {
      if (!stopping) {
        std::cerr << "Warning: serial transfer cancelled" << '\n';
      }
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: serial transfer failed: " << error.what()
                  << '\n';
      }
    }

    finish_transfer(request, succeeded);
  }

  static char flow_control_title_code(SerialFlowControl flow_control) {
    if (flow_control == SerialFlowControl::xon) {
      return 'x';
    }
    if (flow_control == SerialFlowControl::hard) {
      return 'h';
    }
    return 'n';
  }

  static std::string title_parameters(
      const SerialConnectionSettings &settings) {
    std::string parameters = serial_parity_to_string(settings.parity);
    parameters += std::to_string(settings.bits);
    parameters += std::to_string(settings.stop_bit);
    parameters.push_back(flow_control_title_code(settings.flow_control));
    return parameters;
  }

public:
  TerminalSerialSession(GtkWidget *terminal, SerialConnectionSettings settings,
                        TerminalSessionCallbacks callbacks)
      : terminal_io(terminal),
        settings(std::move(settings)),
        callbacks(callbacks),
        carrier_tracker(this->settings.carrier_detect) {
  }

  ~TerminalSerialSession() override {
    stop();
    close_fd_noexcept(&serial_fd);
  }

  bool start() override {
    if (started) {
      return true;
    }
    if (settings.device.empty()) {
      return false;
    }

    stopping = false;
    terminal_io.connect_user_input(
        [this](std::span<const unsigned char> bytes) {
          send_user_input(bytes);
        });
    started = true;

    start_device_monitor();
    if (!connect_if_available()) {
      start_reconnect_poll();
      schedule_disconnected_notification();
    }

    return true;
  }

  void stop() override {
    if (stopping) {
      return;
    }

    stopping = true;
    cancel_transfer_noexcept();
    terminal_io.disconnect_user_input();
    remove_sources();
    stop_device_monitor();
    outgoing.clear();
    close_fd_noexcept(&serial_fd);
  }

  void apply_connection_profile(
      const TerminalConnectionProfile &profile) override {
    const auto *updated_settings =
        std::get_if<SerialConnectionSettings>(&profile.settings);
    if (updated_settings == nullptr) {
      return;
    }

    const std::string current_device = settings.device;
    const SerialCarrierDetect previous_carrier_detect =
        settings.carrier_detect;
    SerialConnectionSettings next_settings = *updated_settings;
    next_settings.device = current_device;
    settings = std::move(next_settings);

    if (settings.carrier_detect != previous_carrier_detect) {
      carrier_tracker = SerialCarrierTracker(settings.carrier_detect);
    }

    if (serial_fd >= 0) {
      try {
        configure_serial_port(serial_fd, settings);
      } catch (const std::exception &error) {
        std::cerr << "Warning: failed to apply serial settings: "
                  << error.what() << '\n';
        handle_device_connection_lost();
      }
      return;
    }

    if (started) {
      start_device_monitor();
      if (!connect_if_available()) {
        start_reconnect_poll();
      }
    }
  }

  void resize(glong columns, glong rows) override {
    (void)columns;
    (void)rows;
  }

  bool supports_transfer() const override {
    return true;
  }

  bool transfer_in_progress() const override {
    return transfer_active;
  }

  bool start_transfer(TerminalTransferRequest request) override {
    if (!started || stopping || transfer_active || serial_fd < 0 ||
        carrier_disconnected) {
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
        .send = [this](std::span<const std::uint8_t> bytes,
                       std::uint32_t timeout_ms, std::size_t &written_len,
                       cardio::cancellation cancellation) {
          return send_transfer_bytes(bytes, timeout_ms, written_len,
                                     std::move(cancellation));
        },
        .recv = [this](std::span<std::uint8_t> bytes,
                       std::uint32_t timeout_ms, std::size_t &read_len,
                       cardio::cancellation cancellation) {
          return recv_transfer_bytes(bytes, timeout_ms, read_len,
                                     std::move(cancellation));
        },
        .now_ms = []() { return monotonic_milliseconds(); },
    };
    transfer_task.reset();
    transfer_task.emplace(
        transfer_loop_async(std::move(request), std::move(transport)));
    return true;
  }

  std::string title() const override {
    if (settings.device.empty()) {
      return "serial: (unknown)";
    }

    return "serial: " + settings.device +
          ":" + std::to_string(settings.baudrate) + ":" +
          title_parameters(settings);
  }
};

std::unique_ptr<TerminalSession>
create_terminal_serial_session(GtkWidget *terminal,
                               SerialConnectionSettings settings,
                               TerminalSessionCallbacks callbacks) {
  return std::make_unique<TerminalSerialSession>(terminal, std::move(settings),
                                                 callbacks);
}

} // namespace elder_terms

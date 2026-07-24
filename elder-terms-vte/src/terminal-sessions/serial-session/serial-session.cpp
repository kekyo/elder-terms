#include "serial-session.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cardio.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "../../terminal-text-send-runner.h"
#include "../../terminal-transfer-runner.h"
#include "../../terminal-zmodem-auto-start.h"
#include "../terminal-view-io.h"
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

static int open_event_fd() {
  const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "eventfd failed");
  }
  return fd;
}

static void notify_event_fd_noexcept(int fd) {
  if (fd < 0) {
    return;
  }

  while (::eventfd_write(fd, 1) < 0) {
    if (errno == EINTR) {
      continue;
    }
    return;
  }
}

static void drain_event_fd_noexcept(int fd) {
  if (fd < 0) {
    return;
  }

  eventfd_t value = 0;
  while (::eventfd_read(fd, &value) < 0) {
    if (errno == EINTR) {
      continue;
    }
    return;
  }
  while (::eventfd_read(fd, &value) == 0) {
  }
}

static bool consume_event_fd_noexcept(int fd) {
  if (fd < 0) {
    return false;
  }

  bool consumed = false;
  eventfd_t value = 0;
  while (::eventfd_read(fd, &value) < 0) {
    if (errno == EINTR) {
      continue;
    }
    return consumed;
  }
  consumed = true;
  while (::eventfd_read(fd, &value) == 0) {
  }
  return consumed;
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

static bool fd_events_indicate_connection_lost(cardio::fd_event events) {
  return ((events & cardio::fd_event::error) != cardio::fd_event::none) ||
         ((events & cardio::fd_event::hangup) != cardio::fd_event::none);
}

static cardio::promise<short>
wait_serial_modem_line_change_async(cardio::io_uring &io, int fd,
                                    cardio::cancellation cancellation) {
  return io.submit<short>(
      [fd](::io_uring_sqe *sqe) {
        ::io_uring_prep_poll_add(
            sqe, fd, POLLPRI | POLLERR | POLLHUP | POLLNVAL);
      },
      [](cardio::io_uring_completion completion) {
        if (completion.result < 0) {
          throw std::system_error(-completion.result,
                                  std::generic_category(),
                                  "serial modem-line poll failed");
        }
        return static_cast<short>(completion.result);
      },
      std::move(cancellation));
}

class TerminalSerialSession;

class TerminalSerialSession final : public TerminalSession {
private:
  TerminalViewIo terminal_io;
  SerialConnectionSettings settings;
  TerminalSessionCallbacks callbacks;
  SerialCarrierTracker carrier_tracker;
  std::optional<cardio::io_uring> io;
  cardio::cancellation_source stop_source;
  cardio::primitives::mutex backend_write_mutex;
  std::optional<cardio::cancellation_source> connection_cancel_source;
  std::deque<std::vector<unsigned char>> outgoing;
  std::unique_ptr<SerialDeviceEventMonitor> device_event_monitor;
  std::optional<cardio::cancellation_source> transfer_cancel_source;
  std::optional<cardio::cancellation_source> text_send_cancel_source;
  std::optional<cardio::promise<void>> connection_task;
  std::optional<cardio::promise<void>> read_task;
  std::optional<cardio::promise<void>> write_task;
  std::optional<cardio::promise<void>> carrier_task;
  std::optional<cardio::promise<void>> ended_task;
  std::optional<cardio::promise<void>> transfer_task;
  std::optional<cardio::promise<void>> text_send_task;
  int serial_fd = -1;
  int device_event_fd = -1;
  int transfer_input_event_fd = -1;
  bool started = false;
  bool stopping = false;
  bool connection_loop_running = false;
  bool ended_notification_pending = false;
  bool writing = false;
  bool line_warning_reported = false;
  bool connection_warning_reported = false;
  bool disconnected_reported = false;
  bool carrier_disconnected = false;
  bool transfer_active = false;
  bool text_send_active = false;
  bool zmodem_autostart_enabled = false;
  std::deque<unsigned char> transfer_incoming;
  TerminalZmodemAutoStartDetectorState zmodem_auto_start_detector;

  void feed_terminal(const unsigned char *data, std::size_t size) {
    terminal_io.feed(std::span<const unsigned char>(data, size));
  }

  void append_transfer_input(const unsigned char *data, std::size_t size) {
    if (size == 0) {
      return;
    }
    transfer_incoming.insert(transfer_incoming.end(), data, data + size);
    notify_event_fd_noexcept(transfer_input_event_fd);
  }

  bool handle_zmodem_auto_start(std::span<const unsigned char> bytes) {
    if (text_send_active || !zmodem_autostart_enabled ||
        !callbacks.zmodem_auto_start || bytes.empty()) {
      return false;
    }

    const std::string_view payload(
        reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const std::optional<TerminalTransferDirection> direction =
        feed_terminal_zmodem_auto_start_detector(&zmodem_auto_start_detector,
                                                 payload);
    if (!direction.has_value()) {
      return false;
    }

    callbacks.zmodem_auto_start(*direction);
    return true;
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

  void notify_connection_phase(TerminalSessionConnectionPhase phase) {
    if (callbacks.connection_phase) {
      callbacks.connection_phase(phase);
    }
  }

  void notify_serial_line_state(SerialLineSignals signals) {
    for (ActivityIndicatorState state : serial_line_indicator_states(signals)) {
      notify_indicator_state(state.indicator, state.active);
    }
  }

  void notify_connected_state(bool active) {
    notify_connection_phase(
        active ? TerminalSessionConnectionPhase::connected
               : TerminalSessionConnectionPhase::disconnected);
  }

  cardio::promise<void> notify_session_ended_async() {
    try {
      co_await cardio::promises::delay(1, stop_source.get_cancellation());
    } catch (const cardio::canceled_exception &) {
      co_return;
    }

    ended_notification_pending = false;
    if (!stopping) {
      notify_session_ended(callbacks);
    }
  }

  void schedule_disconnected_notification() {
    if (disconnected_reported || stopping) {
      return;
    }

    disconnected_reported = true;
    if (!ended_notification_pending) {
      ended_notification_pending = true;
      ended_task.reset();
      ended_task.emplace(notify_session_ended_async());
    }
  }

  void cancel_connection_noexcept() {
    if (connection_cancel_source.has_value()) {
      (void)connection_cancel_source->cancel();
    }
  }

  void start_writer() {
    if (stopping || serial_fd < 0 || writing || outgoing.empty() ||
        !connection_cancel_source.has_value()) {
      return;
    }

    writing = true;
    write_task.reset();
    write_task.emplace(write_loop_async(
        serial_fd, connection_cancel_source->get_cancellation()));
  }

  void stop_device_monitor() {
    device_event_monitor.reset();
  }

  bool device_monitor_has_event_sources() const {
    return device_event_monitor != nullptr &&
           device_event_monitor->has_event_sources();
  }

  void handle_device_event() {
    if (stopping || serial_fd >= 0) {
      return;
    }

    notify_event_fd_noexcept(device_event_fd);
    start_connection_loop();
  }

  bool start_device_monitor() {
    if (stopping || serial_fd >= 0 || device_event_monitor != nullptr) {
      return device_monitor_has_event_sources();
    }

    device_event_monitor = std::make_unique<SerialDeviceEventMonitor>(
        settings.device, [this]() { handle_device_event(); });
    device_event_monitor->start();
    return device_monitor_has_event_sources();
  }

  void handle_device_connection_lost(int expected_fd) {
    if (stopping || (expected_fd >= 0 && serial_fd != expected_fd)) {
      return;
    }

    cancel_modem_transfer_noexcept();
    cancel_text_send_noexcept();
    cancel_connection_noexcept();
    outgoing.clear();
    close_fd_noexcept(&serial_fd);
    connection_cancel_source.reset();
    writing = false;
    carrier_tracker = SerialCarrierTracker(settings.carrier_detect);
    carrier_disconnected = false;
    notify_serial_line_state({});
    notify_connected_state(false);
    schedule_disconnected_notification();
    start_connection_loop();
  }

  void handle_carrier_disconnected() {
    if (stopping || carrier_disconnected) {
      return;
    }

    cancel_modem_transfer_noexcept();
    cancel_text_send_noexcept();
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

  enum class CarrierCheckResult {
    supported,
    unavailable,
    connection_lost,
  };

  CarrierCheckResult check_carrier_state(int fd) {
    if (stopping || serial_fd != fd) {
      return CarrierCheckResult::connection_lost;
    }

    try {
      const SerialLineSignals signals = read_serial_line_signals(fd);
      notify_serial_line_state(signals);
      if (selected_carrier_signal_is_high(signals, settings.carrier_detect)) {
        handle_carrier_connected();
      }
      if (carrier_tracker.update(signals) ==
          SerialCarrierEvent::disconnected) {
        handle_carrier_disconnected();
      }
      return CarrierCheckResult::supported;
    } catch (const std::system_error &error) {
      if (serial_line_error_indicates_connection_lost(error)) {
        std::cerr << "Warning: serial carrier detection failed: "
                  << error.what() << '\n';
        notify_serial_line_state({});
        handle_device_connection_lost(fd);
        return CarrierCheckResult::connection_lost;
      }

      notify_serial_line_state({});
      if (!line_warning_reported) {
        std::cerr << "Warning: serial carrier detection unavailable: "
                  << error.what() << '\n';
        line_warning_reported = true;
      }
      return CarrierCheckResult::unavailable;
    } catch (const std::exception &error) {
      notify_serial_line_state({});
      if (!line_warning_reported) {
        std::cerr << "Warning: serial carrier detection unavailable: "
                  << error.what() << '\n';
        line_warning_reported = true;
      }
      return CarrierCheckResult::unavailable;
    }
  }

  void start_carrier_monitor(int fd) {
    if (stopping || serial_fd != fd || !connection_cancel_source.has_value() ||
        !io.has_value()) {
      return;
    }

    carrier_task.reset();
    if (check_carrier_state(fd) != CarrierCheckResult::supported) {
      return;
    }

    carrier_task.emplace(carrier_loop_async(
        fd, connection_cancel_source->get_cancellation()));
  }

  void start_connection_tasks(int fd) {
    connection_cancel_source.emplace();
    read_task.reset();
    read_task.emplace(read_loop_async(
        fd, connection_cancel_source->get_cancellation()));
    start_carrier_monitor(fd);
  }

  cardio::promise<bool> attempt_connect_async() {
    if (stopping || serial_fd >= 0) {
      co_return serial_fd >= 0;
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
      co_return false;
    }

    int opened_fd = -1;
    try {
      opened_fd = co_await cardio::io_urings::open(
          *io, resolve_result.path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC,
          0, stop_source.get_cancellation());
      if (stopping) {
        close_fd_noexcept(&opened_fd);
        co_return false;
      }

      configure_serial_port(opened_fd, settings);
      serial_fd = opened_fd;
      opened_fd = -1;
      carrier_tracker = SerialCarrierTracker(settings.carrier_detect);
      line_warning_reported = false;
      connection_warning_reported = false;
      disconnected_reported = false;
      carrier_disconnected = false;
      notify_connected_state(true);
      stop_device_monitor();
      start_connection_tasks(serial_fd);
      start_writer();
      co_return true;
    } catch (const cardio::canceled_exception &) {
      close_fd_noexcept(&opened_fd);
      co_return false;
    } catch (const std::exception &error) {
      if (!connection_warning_reported) {
        std::cerr << "Warning: failed to initialize serial session: "
                  << error.what() << '\n';
        connection_warning_reported = true;
      }
      close_fd_noexcept(&opened_fd);
      co_return false;
    }
  }

  cardio::promise<void> connection_loop_async() {
    try {
      while (!stopping && serial_fd < 0) {
        (void)start_device_monitor();
        const bool connected = co_await attempt_connect_async();
        if (connected || stopping || serial_fd >= 0) {
          break;
        }

        schedule_disconnected_notification();
        if (!device_monitor_has_event_sources()) {
          break;
        }
        if (consume_event_fd_noexcept(device_event_fd)) {
          continue;
        }
        co_await cardio::from_fd(device_event_fd, cardio::fd_event::read,
                                 stop_source.get_cancellation());
        (void)consume_event_fd_noexcept(device_event_fd);
      }
    } catch (const cardio::canceled_exception &) {
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: serial connection task failed: "
                  << error.what() << '\n';
      }
    }

    connection_loop_running = false;
  }

  void start_connection_loop() {
    if (stopping || serial_fd >= 0 || connection_loop_running ||
        !io.has_value()) {
      return;
    }

    connection_loop_running = true;
    connection_task.reset();
    connection_task.emplace(connection_loop_async());
  }

  bool drain_serial_reads(int fd) {
    std::array<unsigned char, 4096> buffer{};
    while (!stopping) {
      if (serial_fd != fd) {
        return false;
      }

      const ssize_t read_size = ::read(fd, buffer.data(), buffer.size());
      if (read_size > 0) {
        notify_activity(ActivityIndicatorId::rd);
        if (transfer_active) {
          // Modem protocol bytes must remain outside the terminal text codec.
          append_transfer_input(buffer.data(),
                                static_cast<std::size_t>(read_size));
        } else {
          const std::span<const unsigned char> bytes(
              buffer.data(), static_cast<std::size_t>(read_size));
          if (handle_zmodem_auto_start(bytes)) {
            continue;
          }
          feed_terminal(buffer.data(), static_cast<std::size_t>(read_size));
        }
        continue;
      }
      if (read_size == 0) {
        return false;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
      }

      std::cerr << "Warning: serial read failed: " << std::strerror(errno)
                << '\n';
      return false;
    }

    return false;
  }

  cardio::promise<void>
  read_loop_async(int fd, cardio::cancellation cancellation) {
    bool connection_lost = false;
    try {
      while (!stopping && serial_fd == fd) {
        const cardio::fd_event events =
            co_await cardio::from_fd(fd, cardio::fd_event::read,
                                     cancellation);
        if (fd_events_indicate_connection_lost(events)) {
          connection_lost = true;
          break;
        }
        if ((events & cardio::fd_event::read) == cardio::fd_event::none) {
          continue;
        }
        if (!drain_serial_reads(fd)) {
          connection_lost = true;
          break;
        }
      }
    } catch (const cardio::canceled_exception &) {
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: serial read failed: " << error.what() << '\n';
        connection_lost = true;
      }
    }

    if (connection_lost && !stopping) {
      handle_device_connection_lost(fd);
    }
  }

  cardio::promise<void>
  write_loop_async(int fd, cardio::cancellation cancellation) {
    bool connection_lost = false;
    try {
      while (!stopping && serial_fd == fd && !outgoing.empty()) {
        std::vector<unsigned char> chunk = std::move(outgoing.front());
        outgoing.pop_front();
        auto write_lock_promise = backend_write_mutex.lock(cancellation);
        auto write_lock = std::move(co_await write_lock_promise);
        std::size_t offset = 0;
        while (!stopping && serial_fd == fd && offset < chunk.size()) {
          const ssize_t written =
              ::write(fd, chunk.data() + offset, chunk.size() - offset);
          if (written > 0) {
            notify_activity(ActivityIndicatorId::sd);
            offset += static_cast<std::size_t>(written);
            continue;
          }
          if (written == 0) {
            throw std::runtime_error("serial write made no progress");
          }
          if (errno == EINTR) {
            continue;
          }
          if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "Warning: serial write failed: "
                      << std::strerror(errno) << '\n';
            connection_lost = true;
            break;
          }

          const cardio::fd_event events =
              co_await cardio::from_fd(fd, cardio::fd_event::write,
                                       cancellation);
          if (fd_events_indicate_connection_lost(events)) {
            connection_lost = true;
            break;
          }
        }
        if (connection_lost) {
          break;
        }
      }
    } catch (const cardio::canceled_exception &) {
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: serial write failed: " << error.what() << '\n';
        connection_lost = true;
      }
    }

    writing = false;
    if (connection_lost && !stopping) {
      handle_device_connection_lost(fd);
      co_return;
    }
    if (!stopping && serial_fd == fd && !outgoing.empty()) {
      start_writer();
    }
  }

  cardio::promise<void>
  carrier_loop_async(int fd, cardio::cancellation cancellation) {
    try {
      while (!stopping && serial_fd == fd) {
        const short events =
            co_await wait_serial_modem_line_change_async(
                *io, fd, cancellation);
        if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          handle_device_connection_lost(fd);
          co_return;
        }
        const CarrierCheckResult result = check_carrier_state(fd);
        if (result != CarrierCheckResult::supported) {
          co_return;
        }
      }
    } catch (const cardio::canceled_exception &) {
    } catch (const std::exception &error) {
      if (!stopping && serial_fd == fd) {
        std::cerr << "Warning: serial carrier detection failed: "
                  << error.what() << '\n';
        handle_device_connection_lost(fd);
      }
    }
  }

  cardio::promise<void>
  wait_transfer_write_ready_async(std::uint32_t timeout_ms,
                                  std::uint64_t start_ms,
                                  cardio::cancellation cancellation) {
    if (timeout_ms == 0 ||
        monotonic_milliseconds() - start_ms >= timeout_ms) {
      throw xyzm_async_timeout_error("serial transfer send timeout");
    }

    const std::uint64_t elapsed = monotonic_milliseconds() - start_ms;
    const std::uint64_t remaining =
        elapsed >= timeout_ms ? 1 : timeout_ms - elapsed;
    cardio::cancellation_source timeout_source =
        cardio::cancellations::timeout(remaining);
    cardio::cancellation_source combined =
        transfer_cancel_source.has_value()
            ? cardio::cancellations::any(
                  cancellation, transfer_cancel_source->get_cancellation(),
                  timeout_source.get_cancellation())
            : cardio::cancellations::any(cancellation,
                                         timeout_source.get_cancellation());

    try {
      co_await cardio::from_fd(serial_fd, cardio::fd_event::write,
                               combined.get_cancellation());
    } catch (const cardio::canceled_exception &) {
      if (cancellation.is_cancellation_requested() ||
          (transfer_cancel_source.has_value() &&
           transfer_cancel_source->get_cancellation()
               .is_cancellation_requested())) {
        throw xyzm_async_cancelled_error("serial transfer cancelled");
      }
      throw xyzm_async_timeout_error("serial transfer send timeout");
    }
  }

  void cancel_modem_transfer_noexcept() {
    if (transfer_cancel_source.has_value()) {
      (void)transfer_cancel_source->cancel();
    }
    notify_event_fd_noexcept(transfer_input_event_fd);
  }

  void cancel_text_send_noexcept() {
    if (text_send_cancel_source.has_value()) {
      (void)text_send_cancel_source->cancel();
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
    auto write_lock_promise = backend_write_mutex.lock(cancellation);
    auto write_lock = std::move(co_await write_lock_promise);
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

      co_await wait_transfer_write_ready_async(timeout_ms, start_ms,
                                               cancellation);
    }
  }

  cardio::promise<void>
  wait_transfer_input_async(std::uint32_t timeout_ms,
                            std::uint64_t start_ms,
                            cardio::cancellation cancellation) {
    if (timeout_ms == 0 ||
        monotonic_milliseconds() - start_ms >= timeout_ms) {
      throw xyzm_async_timeout_error("serial transfer receive timeout");
    }

    const std::uint64_t elapsed = monotonic_milliseconds() - start_ms;
    const std::uint64_t remaining =
        elapsed >= timeout_ms ? 1 : timeout_ms - elapsed;
    cardio::cancellation_source timeout_source =
        cardio::cancellations::timeout(static_cast<std::uint32_t>(remaining));
    cardio::cancellation_source combined =
        transfer_cancel_source.has_value()
            ? cardio::cancellations::any(
                  cancellation, transfer_cancel_source->get_cancellation(),
                  timeout_source.get_cancellation())
            : cardio::cancellations::any(cancellation,
                                         timeout_source.get_cancellation());

    drain_event_fd_noexcept(transfer_input_event_fd);
    try {
      co_await cardio::from_fd(transfer_input_event_fd, cardio::fd_event::read,
                               combined.get_cancellation());
      drain_event_fd_noexcept(transfer_input_event_fd);
    } catch (const cardio::canceled_exception &) {
      if (cancellation.is_cancellation_requested() ||
          (transfer_cancel_source.has_value() &&
           transfer_cancel_source->get_cancellation()
               .is_cancellation_requested())) {
        throw xyzm_async_cancelled_error("serial transfer cancelled");
      }
      throw xyzm_async_timeout_error("serial transfer receive timeout");
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
      co_await wait_transfer_input_async(timeout_ms, start_ms, cancellation);
    }

    read_len = std::min(bytes.size(), transfer_incoming.size());
    for (std::size_t index = 0; index < read_len; ++index) {
      bytes[index] = transfer_incoming.front();
      transfer_incoming.pop_front();
    }
  }

  void send_user_input(std::span<const unsigned char> bytes) {
    if (bytes.empty() || stopping || serial_fd < 0 ||
        carrier_disconnected || transfer_active || text_send_active) {
      return;
    }

    outgoing.emplace_back(bytes.begin(), bytes.end());
    start_writer();
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

  cardio::promise<void>
  send_text_bytes(std::span<const unsigned char> bytes,
                  cardio::cancellation cancellation) {
    if (!text_send_active || stopping || serial_fd < 0 ||
        carrier_disconnected) {
      throw std::runtime_error("serial text send is not connected");
    }
    auto write_lock_promise = backend_write_mutex.lock(cancellation);
    auto write_lock = std::move(co_await write_lock_promise);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      cancellation.throw_if_cancellation_requested();
      if (!text_send_active || stopping || serial_fd < 0 ||
          carrier_disconnected) {
        throw std::runtime_error("serial text send is not connected");
      }
      const ssize_t written =
          ::write(serial_fd, bytes.data() + offset, bytes.size() - offset);
      if (written > 0) {
        notify_activity(ActivityIndicatorId::sd);
        offset += static_cast<std::size_t>(written);
        continue;
      }
      if (written == 0) {
        throw std::runtime_error("serial text send write made no progress");
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        throw std::runtime_error(std::string("serial text send failed: ") +
                                 std::strerror(errno));
      }
      const cardio::fd_event events = co_await cardio::from_fd(
          serial_fd, cardio::fd_event::write, cancellation);
      if (fd_events_indicate_connection_lost(events)) {
        throw std::runtime_error("serial text send connection was lost");
      }
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
      request.status(succeeded ? "Terminal" : "Text send failed");
    }
    if (!stopping && serial_fd >= 0 && !carrier_disconnected &&
        !transfer_active) {
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
        std::cerr << "Warning: serial text send cancelled" << '\n';
      }
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: serial text send failed: " << error.what()
                  << '\n';
      }
    }
    finish_text_send(request, succeeded);
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
                        TerminalTextSettings text_settings,
                        TerminalSessionCallbacks callbacks)
      : terminal_io(terminal, text_settings, callbacks.output),
        settings(std::move(settings)),
        callbacks(callbacks),
        carrier_tracker(this->settings.carrier_detect) {
  }

  ~TerminalSerialSession() override {
    stop();
    close_fd_noexcept(&serial_fd);
    close_fd_noexcept(&device_event_fd);
    close_fd_noexcept(&transfer_input_event_fd);
  }

  bool start() override {
    if (started) {
      return true;
    }
    if (settings.device.empty()) {
      return false;
    }

    notify_connection_phase(TerminalSessionConnectionPhase::connecting);
    stopping = false;
    try {
      io.emplace(64);
      device_event_fd = open_event_fd();
      transfer_input_event_fd = open_event_fd();
    } catch (const std::exception &error) {
      std::cerr << "Warning: failed to initialize serial session: "
                << error.what() << '\n';
      close_fd_noexcept(&device_event_fd);
      close_fd_noexcept(&transfer_input_event_fd);
      io.reset();
      notify_connection_phase(TerminalSessionConnectionPhase::disconnected);
      return false;
    }
    terminal_io.connect_user_input(
        [this](std::span<const unsigned char> bytes) {
          send_user_input(bytes);
        });
    started = true;
    start_connection_loop();

    return true;
  }

  void stop() override {
    if (stopping) {
      return;
    }

    stopping = true;
    cancel_modem_transfer_noexcept();
    cancel_text_send_noexcept();
    cancel_connection_noexcept();
    (void)stop_source.cancel();
    notify_event_fd_noexcept(device_event_fd);
    terminal_io.disconnect_user_input();
    stop_device_monitor();
    ended_notification_pending = false;
    outgoing.clear();
    transfer_incoming.clear();
    close_fd_noexcept(&serial_fd);
  }

  void apply_connection_profile(
      const TerminalConnectionProfile &profile) override {
    const auto *updated_settings =
        std::get_if<SerialConnectionSettings>(&profile.settings);
    if (updated_settings == nullptr) {
      return;
    }

    (void)terminal_io.apply_text_settings(profile.text_settings);

    const std::string current_device = settings.device;
    const SerialCarrierDetect previous_carrier_detect =
        settings.carrier_detect;
    SerialConnectionSettings next_settings = *updated_settings;
    next_settings.device = current_device;
    settings = std::move(next_settings);

    if (settings.carrier_detect != previous_carrier_detect) {
      carrier_tracker = SerialCarrierTracker(settings.carrier_detect);
      if (serial_fd >= 0) {
        start_carrier_monitor(serial_fd);
      }
    }

    if (serial_fd >= 0) {
      try {
        configure_serial_port(serial_fd, settings);
      } catch (const std::exception &error) {
        std::cerr << "Warning: failed to apply serial settings: "
                  << error.what() << '\n';
        handle_device_connection_lost(serial_fd);
      }
      return;
    }

    if (started) {
      start_connection_loop();
    }
  }

  void resize(glong columns, glong rows) override {
    (void)columns;
    (void)rows;
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
    cancel_text_send_noexcept();
    return true;
  }

  void set_zmodem_autostart(bool enabled) override {
    if (zmodem_autostart_enabled == enabled) {
      return;
    }

    zmodem_autostart_enabled = enabled;
    zmodem_auto_start_detector = {};
  }

  bool start_transfer(TerminalTransferRequest request) override {
    if (!started || stopping || transfer_active || text_send_active ||
        serial_fd < 0 || carrier_disconnected) {
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

  bool start_text_send(TerminalTextSendRequest request) override {
    if (!started || stopping || transfer_active || text_send_active ||
        serial_fd < 0 || carrier_disconnected ||
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
                               TerminalTextSettings text_settings,
                               TerminalSessionCallbacks callbacks) {
  return std::make_unique<TerminalSerialSession>(terminal, std::move(settings),
                                                 std::move(text_settings),
                                                 callbacks);
}

} // namespace elder_terms

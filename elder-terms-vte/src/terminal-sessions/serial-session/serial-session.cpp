#include "serial-session.h"

#include <fcntl.h>
#include <unistd.h>

#include <glib-unix.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

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
  int serial_fd = -1;
  guint read_watch_id = 0;
  guint write_watch_id = 0;
  guint carrier_poll_id = 0;
  guint ended_idle_id = 0;
  bool started = false;
  bool stopping = false;
  bool line_warning_reported = false;
  bool connection_warning_reported = false;
  bool disconnected_reported = false;
  bool carrier_disconnected = false;

  void remove_connection_sources() {
    remove_source(&read_watch_id);
    remove_source(&write_watch_id);
    remove_source(&carrier_poll_id);
  }

  void remove_sources() {
    remove_connection_sources();
    remove_source(&ended_idle_id);
  }

  void feed_terminal(const unsigned char *data, std::size_t size) {
    terminal_io.feed(std::span<const unsigned char>(data, size));
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

  bool connect_if_available() {
    const bool connected = attempt_connect();
    if (connected) {
      stop_device_monitor();
    }
    return connected;
  }

  void handle_device_event() {
    if (stopping || serial_fd >= 0) {
      return;
    }

    (void)connect_if_available();
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

    remove_connection_sources();
    outgoing.clear();
    close_fd_noexcept(&serial_fd);
    carrier_tracker = SerialCarrierTracker(settings.carrier_detect);
    carrier_disconnected = false;
    schedule_disconnected_notification();
    start_device_monitor();
    (void)connect_if_available();
  }

  void handle_carrier_disconnected() {
    if (stopping || carrier_disconnected) {
      return;
    }

    outgoing.clear();
    carrier_disconnected = true;
    schedule_disconnected_notification();
  }

  void handle_carrier_connected() {
    if (!carrier_disconnected) {
      return;
    }

    carrier_disconnected = false;
    disconnected_reported = false;
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
        feed_terminal(buffer.data(), static_cast<std::size_t>(read_size));
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
        handle_device_connection_lost();
        return false;
      }

      if (!line_warning_reported) {
        std::cerr << "Warning: serial carrier detection unavailable: "
                  << error.what() << '\n';
        line_warning_reported = true;
      }
    } catch (const std::exception &error) {
      if (!line_warning_reported) {
        std::cerr << "Warning: serial carrier detection unavailable: "
                  << error.what() << '\n';
        line_warning_reported = true;
      }
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
        carrier_disconnected) {
      return;
    }

    outgoing.emplace_back(bytes.begin(), bytes.end());
    start_writer();
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

    stopping = false;
    terminal_io.connect_user_input(
        [this](std::span<const unsigned char> bytes) {
          send_user_input(bytes);
        });
    started = true;

    start_device_monitor();
    if (!connect_if_available()) {
      schedule_disconnected_notification();
    }

    return true;
  }

  void stop() override {
    if (stopping) {
      return;
    }

    stopping = true;
    terminal_io.disconnect_user_input();
    remove_sources();
    stop_device_monitor();
    outgoing.clear();
    close_fd_noexcept(&serial_fd);
  }

  void apply_connection_profile(
      const TerminalConnectionProfile &profile) override {
    if (profile.kind != TerminalConnectionKind::serial) {
      return;
    }

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
      (void)connect_if_available();
    }
  }

  void resize(glong columns, glong rows) override {
    (void)columns;
    (void)rows;
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

#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <cardio.h>
#include <vte/vte.h>

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "telnet-session.h"

#include "../../terminal-text-send-runner.h"
#include "../../terminal-transfer-runner.h"
#include "../../terminal-zmodem-auto-start.h"
#include "../terminal-view-io.h"
#include "telnet-protocol.h"

namespace elder_terms {

static constexpr std::uint32_t telnet_binary_timeout_ms = 5000;

struct ResolvedSocketAddress {
  sockaddr_storage storage{};
  socklen_t length = 0;
  int family = AF_UNSPEC;
};

static std::uint16_t clamp_terminal_dimension(glong value) {
  if (value <= 0) {
    return 0;
  }
  if (value > 65535) {
    return 65535;
  }
  return static_cast<std::uint16_t>(value);
}

static ResolvedSocketAddress make_socket_address(GInetAddress *address,
                                                 std::uint16_t port) {
  if (address == nullptr) {
    throw std::runtime_error("TELNET resolver returned an empty address");
  }

  const GSocketFamily family = g_inet_address_get_family(address);
  const guint8 *bytes = g_inet_address_to_bytes(address);
  ResolvedSocketAddress result;
  if (family == G_SOCKET_FAMILY_IPV4) {
    sockaddr_in native_address{};
    native_address.sin_family = AF_INET;
    native_address.sin_port = htons(port);
    std::memcpy(&native_address.sin_addr, bytes, sizeof(native_address.sin_addr));
    std::memcpy(&result.storage, &native_address, sizeof(native_address));
    result.length = sizeof(native_address);
    result.family = AF_INET;
    return result;
  }

  if (family == G_SOCKET_FAMILY_IPV6) {
    sockaddr_in6 native_address{};
    native_address.sin6_family = AF_INET6;
    native_address.sin6_port = htons(port);
    std::memcpy(&native_address.sin6_addr, bytes,
                sizeof(native_address.sin6_addr));
    std::memcpy(&result.storage, &native_address, sizeof(native_address));
    result.length = sizeof(native_address);
    result.family = AF_INET6;
    return result;
  }

  throw std::runtime_error("TELNET resolver returned an unsupported address");
}

static cardio::promise<ResolvedSocketAddress>
resolve_address_async(std::string address, std::uint16_t port,
                      cardio::cancellation cancellation) {
  auto resolver =
      std::shared_ptr<GResolver>(g_resolver_get_default(), g_object_unref);
  auto address_holder = std::make_shared<std::string>(std::move(address));
  GList *raw_addresses = co_await cardio::gio::submit<GList *>(
      [resolver, address_holder](GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data) {
        g_resolver_lookup_by_name_async(resolver.get(), address_holder->c_str(),
                                        cancellable, callback, user_data);
      },
      [resolver](GObject *object, GAsyncResult *result, GError **error) {
        (void)resolver;
        return g_resolver_lookup_by_name_finish(G_RESOLVER(object), result,
                                                error);
      },
      std::move(cancellation));

  auto addresses = std::unique_ptr<GList, decltype(&g_resolver_free_addresses)>(
      raw_addresses, g_resolver_free_addresses);
  if (addresses == nullptr) {
    throw std::runtime_error("TELNET resolver returned no addresses");
  }

  co_return make_socket_address(G_INET_ADDRESS(addresses->data), port);
}

static cardio::promise<int>
open_socket_async(cardio::io_uring &io, int family,
                  cardio::cancellation cancellation) {
  return io.submit<int>(
      [family](::io_uring_sqe *sqe) {
        ::io_uring_prep_socket(sqe, family, SOCK_STREAM | SOCK_CLOEXEC, 0, 0);
      },
      [](cardio::io_uring_completion completion) {
        if (completion.result < 0) {
          throw std::system_error(-completion.result, std::generic_category(),
                                  "TELNET socket failed");
        }
        return completion.result;
      },
      std::move(cancellation));
}

static cardio::promise<void>
connect_socket_async(cardio::io_uring &io, int fd,
                     const ResolvedSocketAddress &address,
                     cardio::cancellation cancellation) {
  auto storage = std::make_shared<sockaddr_storage>(address.storage);
  const socklen_t length = address.length;
  return io.submit<void>(
      [fd, storage, length](::io_uring_sqe *sqe) {
        ::io_uring_prep_connect(
            sqe, fd, reinterpret_cast<const sockaddr *>(storage.get()), length);
      },
      [storage](cardio::io_uring_completion completion) {
        (void)storage;
        if (completion.result < 0) {
          throw std::system_error(-completion.result, std::generic_category(),
                                  "TELNET connect failed");
        }
      },
      std::move(cancellation));
}

static void close_socket_noexcept(int *fd) {
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

static void notify_session_ended(const TerminalSessionCallbacks &callbacks) {
  if (callbacks.ended) {
    callbacks.ended();
  }
}

static std::uint64_t monotonic_milliseconds() {
  return static_cast<std::uint64_t>(g_get_monotonic_time() / 1000);
}

class TerminalTelnetSession final : public TerminalSession {
private:
  TerminalViewIo terminal_io;
  TelnetConnectionSettings settings;
  TerminalSessionCallbacks callbacks;
  TelnetProtocol protocol;
  std::optional<cardio::io_uring> io;
  cardio::cancellation_source stop_source;
  cardio::primitives::mutex backend_write_mutex;
  std::deque<TelnetBytes> outgoing;
  std::optional<cardio::cancellation_source> transfer_cancel_source;
  std::optional<cardio::cancellation_source> text_send_cancel_source;
  std::optional<cardio::promise<void>> read_task;
  std::optional<cardio::promise<void>> write_task;
  std::optional<cardio::promise<void>> transfer_task;
  std::optional<cardio::promise<void>> text_send_task;
  int socket_fd = -1;
  int transfer_input_event_fd = -1;
  int binary_negotiation_event_fd = -1;
  bool started = false;
  bool stopping = false;
  bool writing = false;
  bool transfer_active = false;
  bool text_send_active = false;
  bool initial_binary_negotiation_pending = false;
  bool zmodem_autostart_enabled = false;
  std::deque<unsigned char> transfer_incoming;
  TerminalZmodemAutoStartDetectorState zmodem_auto_start_detector;

  void set_current_window_size() {
    const TerminalViewGridSize size = terminal_io.grid_size();
    protocol.set_window_size(clamp_terminal_dimension(size.columns),
                             clamp_terminal_dimension(size.rows));
  }

  void shutdown_socket_noexcept() {
    if (socket_fd >= 0) {
      (void)::shutdown(socket_fd, SHUT_RDWR);
    }
  }

  void feed_terminal(const TelnetBytes &bytes) {
    terminal_io.feed(std::span<const unsigned char>(bytes.data(), bytes.size()));
  }

  void append_transfer_input(const TelnetBytes &bytes) {
    if (bytes.empty()) {
      return;
    }
    transfer_incoming.insert(transfer_incoming.end(), bytes.begin(),
                             bytes.end());
    notify_event_fd_noexcept(transfer_input_event_fd);
  }

  bool handle_zmodem_auto_start(const TelnetBytes &bytes) {
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

  void enqueue_bytes(TelnetBytes bytes) {
    if (bytes.empty() || stopping) {
      return;
    }

    outgoing.push_back(std::move(bytes));
    start_writer();
  }

  void handle_received(std::span<const unsigned char> bytes) {
    const bool was_binary_ready =
        protocol.is_binary_enabled() || protocol.is_binary_rejected();
    TelnetProtocolResult result = protocol.receive(bytes);
    const bool is_binary_ready =
        protocol.is_binary_enabled() || protocol.is_binary_rejected();
    if (initial_binary_negotiation_pending) {
      if (protocol.is_binary_enabled()) {
        initial_binary_negotiation_pending = false;
        std::clog
            << "Info: TELNET BINARY negotiation succeeded after connection"
            << '\n';
      } else if (protocol.is_binary_rejected()) {
        initial_binary_negotiation_pending = false;
      }
    }
    if (!was_binary_ready && is_binary_ready) {
      notify_event_fd_noexcept(binary_negotiation_event_fd);
    }
    if (transfer_active) {
      // Modem protocol bytes must remain outside the terminal text codec.
      append_transfer_input(result.terminal_data);
    } else {
      if (!handle_zmodem_auto_start(result.terminal_data)) {
        feed_terminal(result.terminal_data);
      }
    }
    for (TelnetBytes &response : result.responses) {
      enqueue_bytes(std::move(response));
    }
  }

  void start_writer() {
    if (writing || socket_fd < 0 || stopping) {
      return;
    }

    writing = true;
    write_task.reset();
    write_task.emplace(write_loop_async());
  }

  cardio::promise<void> close_current_socket_async() {
    const int fd = std::exchange(socket_fd, -1);
    if (fd >= 0 && io.has_value()) {
      try {
        co_await cardio::io_urings::close(*io, fd);
      } catch (...) {
        (void)::close(fd);
      }
    }
  }

  cardio::promise<void> read_loop_async() {
    bool connection_opened = false;
    bool natural_end = false;
    try {
      set_current_window_size();
      const auto port = static_cast<std::uint16_t>(settings.port);
      ResolvedSocketAddress address = co_await resolve_address_async(
          settings.address, port, stop_source.get_cancellation());
      if (stopping) {
        co_return;
      }

      socket_fd = co_await open_socket_async(
          *io, address.family, stop_source.get_cancellation());
      co_await connect_socket_async(*io, socket_fd, address,
                                    stop_source.get_cancellation());
      connection_opened = true;
      if (stopping) {
        co_return;
      }
      notify_indicator_state(ActivityIndicatorId::conn, true);

      initial_binary_negotiation_pending = true;
      for (TelnetBytes &bytes : protocol.encode_enable_binary()) {
        enqueue_bytes(std::move(bytes));
      }
      start_writer();
      std::array<unsigned char, 4096> buffer{};
      while (!stopping) {
        std::span<unsigned char> writable(buffer.data(), buffer.size());
        const std::size_t read_size = co_await cardio::io_urings::read(
            *io, socket_fd, std::as_writable_bytes(writable));
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
        std::cerr << "Warning: TELNET session failed: " << error.what()
                  << '\n';
      }
      natural_end = connection_opened;
    }

    const bool should_notify_session_ended = natural_end && !stopping;
    stopping = true;
    cancel_transfer_noexcept();
    cancel_text_send_noexcept();
    terminal_io.disconnect_user_input();
    outgoing.clear();
    co_await close_current_socket_async();
    if (connection_opened) {
      notify_indicator_state(ActivityIndicatorId::conn, false);
    }
    if (should_notify_session_ended) {
      notify_session_ended(callbacks);
    }
  }

  cardio::promise<void> write_loop_async() {
    try {
      while (!stopping && socket_fd >= 0 && !outgoing.empty()) {
        TelnetBytes chunk = std::move(outgoing.front());
        outgoing.pop_front();
        auto write_lock_promise =
            backend_write_mutex.lock(stop_source.get_cancellation());
        auto write_lock = std::move(co_await write_lock_promise);
        std::size_t offset = 0;
        while (!stopping && offset < chunk.size()) {
          std::span<const unsigned char> remaining(chunk.data() + offset,
                                                   chunk.size() - offset);
          const std::size_t written = co_await cardio::io_urings::write(
              *io, socket_fd, std::as_bytes(remaining),
              stop_source.get_cancellation());
          if (written == 0) {
            throw std::runtime_error("TELNET write made no progress");
          }
          notify_activity(ActivityIndicatorId::sd);
          offset += written;
        }
      }
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: TELNET write failed: " << error.what() << '\n';
      }
      cancel_transfer_noexcept();
      shutdown_socket_noexcept();
    }

    writing = false;
    if (!stopping && socket_fd >= 0 && !outgoing.empty()) {
      start_writer();
    }
  }

  void cancel_transfer_noexcept() {
    if (transfer_cancel_source.has_value()) {
      (void)transfer_cancel_source->cancel();
    }
    notify_event_fd_noexcept(transfer_input_event_fd);
    notify_event_fd_noexcept(binary_negotiation_event_fd);
  }

  void cancel_text_send_noexcept() {
    if (text_send_cancel_source.has_value()) {
      (void)text_send_cancel_source->cancel();
    }
  }

  cardio::promise<void>
  wait_binary_state_change_async(std::uint64_t start_ms,
                                 cardio::cancellation cancellation) {
    if (monotonic_milliseconds() - start_ms >= telnet_binary_timeout_ms) {
      throw xyzm_async_timeout_error("TELNET BINARY negotiation timeout");
    }

    const std::uint64_t elapsed = monotonic_milliseconds() - start_ms;
    const std::uint64_t remaining =
        elapsed >= telnet_binary_timeout_ms
            ? 1
            : telnet_binary_timeout_ms - elapsed;
    cardio::cancellation_source timeout_source =
        cardio::cancellations::timeout(remaining);
    cardio::cancellation_source combined =
        transfer_cancel_source.has_value()
            ? cardio::cancellations::any(
                  cancellation, transfer_cancel_source->get_cancellation(),
                  timeout_source.get_cancellation())
            : cardio::cancellations::any(cancellation,
                                         timeout_source.get_cancellation());

    drain_event_fd_noexcept(binary_negotiation_event_fd);
    try {
      co_await cardio::from_fd(binary_negotiation_event_fd,
                               cardio::fd_event::read,
                               combined.get_cancellation());
      drain_event_fd_noexcept(binary_negotiation_event_fd);
    } catch (const cardio::canceled_exception &) {
      if (cancellation.is_cancellation_requested() ||
          (transfer_cancel_source.has_value() &&
           transfer_cancel_source->get_cancellation()
               .is_cancellation_requested())) {
        throw xyzm_async_cancelled_error("TELNET transfer cancelled");
      }
      throw xyzm_async_timeout_error("TELNET BINARY negotiation timeout");
    }
  }

  cardio::promise<void>
  wait_binary_negotiation_async(cardio::cancellation cancellation) {
    const std::uint64_t start_ms = monotonic_milliseconds();
    while (!protocol.is_binary_enabled()) {
      if (protocol.is_binary_rejected()) {
        throw xyzm_async_protocol_error("TELNET BINARY negotiation rejected");
      }
      if (!transfer_active || stopping || socket_fd < 0) {
        throw xyzm_async_io_error("TELNET transfer is not connected");
      }
      cancellation.throw_if_cancellation_requested();
      if (monotonic_milliseconds() - start_ms >= telnet_binary_timeout_ms) {
        throw xyzm_async_timeout_error("TELNET BINARY negotiation timeout");
      }

      co_await wait_binary_state_change_async(start_ms, cancellation);
    }
  }

  cardio::promise<void>
  send_transfer_bytes(std::span<const std::uint8_t> bytes,
                      std::uint32_t timeout_ms, std::size_t &written_len,
                      cardio::cancellation cancellation) {
    cancellation.throw_if_cancellation_requested();
    if (!transfer_active || stopping || socket_fd < 0 || !io.has_value()) {
      throw xyzm_async_io_error("TELNET transfer is not connected");
    }
    if (bytes.empty()) {
      written_len = 0;
      co_return;
    }

    TelnetBytes encoded = protocol.encode_user_input(bytes);
    auto write_lock_promise = backend_write_mutex.lock(cancellation);
    auto write_lock = std::move(co_await write_lock_promise);
    std::size_t offset = 0;
    const std::uint64_t start_ms = monotonic_milliseconds();
    while (offset < encoded.size()) {
      if (!transfer_active || stopping || socket_fd < 0 || !io.has_value()) {
        throw xyzm_async_io_error("TELNET transfer is not connected");
      }
      cancellation.throw_if_cancellation_requested();
      if (timeout_ms == 0 ||
          monotonic_milliseconds() - start_ms >= timeout_ms) {
        throw xyzm_async_timeout_error("TELNET transfer send timeout");
      }

      std::span<const unsigned char> remaining(encoded.data() + offset,
                                               encoded.size() - offset);
      const std::size_t written = co_await cardio::io_urings::write(
          *io, socket_fd, std::as_bytes(remaining), cancellation);
      if (written == 0) {
        throw xyzm_async_io_error("TELNET transfer write made no progress");
      }
      notify_activity(ActivityIndicatorId::sd);
      offset += written;
    }
    written_len = bytes.size();
    co_return;
  }

  cardio::promise<void>
  wait_transfer_input_async(std::uint32_t timeout_ms,
                            std::uint64_t start_ms,
                            cardio::cancellation cancellation) {
    if (timeout_ms == 0 ||
        monotonic_milliseconds() - start_ms >= timeout_ms) {
      throw xyzm_async_timeout_error("TELNET transfer receive timeout");
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
        throw xyzm_async_cancelled_error("TELNET transfer cancelled");
      }
      throw xyzm_async_timeout_error("TELNET transfer receive timeout");
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
      if (!transfer_active || stopping || socket_fd < 0) {
        throw xyzm_async_io_error("TELNET transfer is not connected");
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

  void finish_transfer(const TerminalTransferRequest &request,
                       bool succeeded) {
    transfer_active = false;
    transfer_incoming.clear();
    transfer_cancel_source.reset();
    if (request.status) {
      request.status("Terminal");
    }
    if (!stopping && socket_fd >= 0) {
      terminal_io.connect_user_input(
          [this](std::span<const unsigned char> next_bytes) {
            send_user_input(next_bytes);
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
      co_await wait_binary_negotiation_async(
          transfer_cancel_source->get_cancellation());
      TerminalTransferRequest run_request = request;
      co_await run_terminal_transfer_async(
          std::move(run_request), std::move(transport),
          transfer_cancel_source->get_cancellation());
      succeeded = true;
    } catch (const xyzm_async_cancelled_error &) {
      if (!stopping) {
        std::cerr << "Warning: TELNET transfer cancelled" << '\n';
      }
    } catch (const cardio::canceled_exception &) {
      if (!stopping) {
        std::cerr << "Warning: TELNET transfer cancelled" << '\n';
      }
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: TELNET transfer failed: " << error.what()
                  << '\n';
      }
    }

    finish_transfer(request, succeeded);
  }

  cardio::promise<void>
  send_text_bytes(std::span<const unsigned char> bytes,
                  cardio::cancellation cancellation) {
    if (!text_send_active || stopping || socket_fd < 0 || !io.has_value()) {
      throw std::runtime_error("TELNET text send is not connected");
    }
    TelnetBytes encoded = protocol.encode_user_input(bytes);
    auto write_lock_promise = backend_write_mutex.lock(cancellation);
    auto write_lock = std::move(co_await write_lock_promise);
    std::size_t offset = 0;
    while (offset < encoded.size()) {
      cancellation.throw_if_cancellation_requested();
      if (!text_send_active || stopping || socket_fd < 0 || !io.has_value()) {
        throw std::runtime_error("TELNET text send is not connected");
      }
      const std::span<const unsigned char> remaining(
          encoded.data() + offset, encoded.size() - offset);
      const std::size_t written = co_await cardio::io_urings::write(
          *io, socket_fd, std::as_bytes(remaining), cancellation);
      if (written == 0) {
        throw std::runtime_error("TELNET text send write made no progress");
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
      request.status(succeeded ? "Terminal" : "Text send failed");
    }
    if (!stopping && socket_fd >= 0 && !transfer_active) {
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
        std::cerr << "Warning: TELNET text send cancelled" << '\n';
      }
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: TELNET text send failed: " << error.what()
                  << '\n';
      }
    }
    finish_text_send(request, succeeded);
  }

  void send_user_input(std::span<const unsigned char> bytes) {
    if (bytes.empty() || transfer_active || text_send_active) {
      return;
    }

    enqueue_bytes(protocol.encode_user_input(bytes));
  }

public:
  TerminalTelnetSession(GtkWidget *terminal, TelnetConnectionSettings settings,
                        TerminalTextSettings text_settings,
                        TerminalSessionCallbacks callbacks)
      : terminal_io(terminal, text_settings, callbacks.output),
        settings(std::move(settings)),
        callbacks(callbacks) {
  }

  ~TerminalTelnetSession() override {
    stop();
    close_socket_noexcept(&socket_fd);
    close_socket_noexcept(&transfer_input_event_fd);
    close_socket_noexcept(&binary_negotiation_event_fd);
  }

  bool start() override {
    if (started) {
      return true;
    }
    if (settings.address.empty()) {
      return false;
    }

    try {
      io.emplace(64);
      transfer_input_event_fd = open_event_fd();
      binary_negotiation_event_fd = open_event_fd();
      set_current_window_size();
      terminal_io.connect_user_input(
          [this](std::span<const unsigned char> bytes) {
            send_user_input(bytes);
          });
      read_task.emplace(read_loop_async());
      started = true;
      return true;
    } catch (const std::exception &error) {
      std::cerr << "Warning: failed to initialize TELNET session: "
                << error.what() << '\n';
      stop();
      return false;
    }
  }

  void stop() override {
    if (stopping) {
      return;
    }

    stopping = true;
    cancel_transfer_noexcept();
    cancel_text_send_noexcept();
    terminal_io.disconnect_user_input();
    outgoing.clear();
    (void)stop_source.cancel();
    shutdown_socket_noexcept();
  }

  void resize(glong columns, glong rows) override {
    protocol.set_window_size(clamp_terminal_dimension(columns),
                             clamp_terminal_dimension(rows));
    if (protocol.is_naws_enabled()) {
      enqueue_bytes(protocol.encode_naws());
    }
  }

  void apply_connection_profile(
      const TerminalConnectionProfile &profile) override {
    (void)terminal_io.apply_text_settings(profile.text_settings);
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

  void set_zmodem_autostart(bool enabled) override {
    if (zmodem_autostart_enabled == enabled) {
      return;
    }

    zmodem_autostart_enabled = enabled;
    zmodem_auto_start_detector = {};
  }

  bool start_transfer(TerminalTransferRequest request) override {
    if (!started || stopping || transfer_active || text_send_active ||
        socket_fd < 0) {
      return false;
    }
    if (request.direction == TerminalTransferDirection::send &&
        request.source_file_uris.empty()) {
      return false;
    }

    transfer_cancel_source.emplace();
    transfer_incoming.clear();
    transfer_active = true;
    initial_binary_negotiation_pending = false;
    terminal_io.disconnect_user_input();
    outgoing.clear();
    if (request.active) {
      request.active(true);
    }
    for (TelnetBytes &bytes : protocol.encode_enable_binary()) {
      enqueue_bytes(std::move(bytes));
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
        socket_fd < 0 ||
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
    if (settings.address.empty()) {
      return "telnet: (unknown)";
    }

    return "telnet: " + settings.address + ":" + std::to_string(settings.port);
  }
};

std::unique_ptr<TerminalSession>
create_terminal_telnet_session(GtkWidget *terminal,
                               TelnetConnectionSettings settings,
                               TerminalTextSettings text_settings,
                               TerminalSessionCallbacks callbacks) {
  return std::make_unique<TerminalTelnetSession>(terminal,
                                                 std::move(settings),
                                                 std::move(text_settings),
                                                 callbacks);
}

} // namespace elder_terms

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <cardio.h>
#include <vte/vte.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "telnet-session.h"

#include "../terminal-view-io.h"
#include "telnet-protocol.h"

namespace elder_terms {

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

static void notify_session_ended(const TerminalSessionCallbacks &callbacks) {
  if (callbacks.ended) {
    callbacks.ended();
  }
}

class TerminalTelnetSession final : public TerminalSession {
private:
  GtkWidget *terminal = nullptr;
  TerminalViewIo terminal_io;
  TelnetConnectionSettings settings;
  TerminalSessionCallbacks callbacks;
  TelnetProtocol protocol;
  std::optional<cardio::io_uring> io;
  cardio::cancellation_source stop_source;
  std::deque<TelnetBytes> outgoing;
  std::optional<cardio::promise<void>> read_task;
  std::optional<cardio::promise<void>> write_task;
  int socket_fd = -1;
  bool started = false;
  bool stopping = false;
  bool writing = false;

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

  void enqueue_bytes(TelnetBytes bytes) {
    if (bytes.empty() || stopping) {
      return;
    }

    outgoing.push_back(std::move(bytes));
    start_writer();
  }

  void handle_received(std::span<const unsigned char> bytes) {
    TelnetProtocolResult result = protocol.receive(bytes);
    feed_terminal(result.terminal_data);
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
    terminal_io.disconnect_user_input();
    outgoing.clear();
    co_await close_current_socket_async();
    if (should_notify_session_ended) {
      notify_session_ended(callbacks);
    }
  }

  cardio::promise<void> write_loop_async() {
    try {
      while (!stopping && socket_fd >= 0 && !outgoing.empty()) {
        TelnetBytes chunk = std::move(outgoing.front());
        outgoing.pop_front();
        std::size_t offset = 0;
        while (!stopping && offset < chunk.size()) {
          std::span<const unsigned char> remaining(chunk.data() + offset,
                                                   chunk.size() - offset);
          const std::size_t written = co_await cardio::io_urings::write(
              *io, socket_fd, std::as_bytes(remaining));
          if (written == 0) {
            throw std::runtime_error("TELNET write made no progress");
          }
          offset += written;
        }
      }
    } catch (const std::exception &error) {
      if (!stopping) {
        std::cerr << "Warning: TELNET write failed: " << error.what() << '\n';
      }
      shutdown_socket_noexcept();
    }

    writing = false;
    if (!stopping && socket_fd >= 0 && !outgoing.empty()) {
      start_writer();
    }
  }

  void send_user_input(std::span<const unsigned char> bytes) {
    if (bytes.empty()) {
      return;
    }

    enqueue_bytes(protocol.encode_user_input(bytes));
  }

public:
  TerminalTelnetSession(GtkWidget *terminal, TelnetConnectionSettings settings,
                        TerminalSessionCallbacks callbacks)
      : terminal(terminal),
        terminal_io(terminal),
        settings(std::move(settings)),
        callbacks(callbacks) {
  }

  ~TerminalTelnetSession() override {
    stop();
    close_socket_noexcept(&socket_fd);
  }

  bool start() override {
    if (started) {
      return true;
    }

    try {
      io.emplace(64);
      set_current_window_size();
      vte_terminal_set_delete_binding(VTE_TERMINAL(terminal),
                                      VTE_ERASE_ASCII_DELETE);
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
};

std::unique_ptr<TerminalSession>
create_terminal_telnet_session(GtkWidget *terminal,
                               TelnetConnectionSettings settings,
                               TerminalSessionCallbacks callbacks) {
  return std::make_unique<TerminalTelnetSession>(terminal,
                                                 std::move(settings),
                                                 callbacks);
}

} // namespace elder_terms

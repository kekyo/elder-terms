#include "ssh-socks-proxy.h"
#include "ssh-proxy.h"
#include "ssh-channel-connection.h"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace elder_terms {

struct SocksState {
  int listener = -1;
  std::string directory;
  std::string socket_path;
  std::string destination;
  std::shared_ptr<AuthenticatedSshTransport> gateway;
  cardio::cancellation_source stopping;
  std::vector<cardio::promise<void>> clients;

  void close_listener() {
    if (listener >= 0) ::close(std::exchange(listener, -1));
    if (!socket_path.empty()) (void)::unlink(socket_path.c_str());
    if (!directory.empty()) (void)::rmdir(directory.c_str());
  }
  ~SocksState() { close_listener(); }
};

struct SshSocksProxy {
  std::shared_ptr<SocksState> state;
  std::optional<cardio::promise<void>> task;
  ~SshSocksProxy() {
    if (state) state->stopping.cancel();
    if (task && !task->is_ready()) cardio::fire_and_forget(std::move(*task));
  }
};

struct SocksSocket {
  int fd;
  ~SocksSocket() { if (fd >= 0) ::close(fd); }
};

static cardio::promise<void> read_socks_async(int fd, std::span<unsigned char> bytes,
                                             cardio::cancellation cancellation) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    cancellation.throw_if_cancellation_requested();
    const auto count = ::recv(fd, bytes.data() + offset, bytes.size() - offset, MSG_DONTWAIT);
    if (count > 0) offset += count;
    else if (count < 0 && errno == EINTR) continue;
    else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      (void)co_await cardio::from_fd(fd, cardio::fd_event::read | cardio::fd_event::hangup | cardio::fd_event::error, cancellation);
    } else throw std::runtime_error("SSH proxy SOCKS request ended");
  }
}

static cardio::promise<void> write_socks_async(int fd, std::span<const unsigned char> bytes,
                                              cardio::cancellation cancellation) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    cancellation.throw_if_cancellation_requested();
    const auto count = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count > 0) offset += count;
    else if (count < 0 && errno == EINTR) continue;
    else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      (void)co_await cardio::from_fd(fd, cardio::fd_event::write | cardio::fd_event::hangup | cardio::fd_event::error, cancellation);
    } else throw std::runtime_error("SSH proxy SOCKS response ended");
  }
}

static std::string normalized_host(std::string host) {
  std::array<unsigned char, 16> address{};
  std::array<char, INET6_ADDRSTRLEN> text{};
  for (const auto family : {AF_INET, AF_INET6}) {
    if (::inet_pton(family, host.c_str(), address.data()) == 1 &&
        ::inet_ntop(family, address.data(), text.data(), text.size())) return text.data();
  }
  std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
  });
  return host;
}

static cardio::promise<void> serve_socks_client_async(int fd,
    std::shared_ptr<AuthenticatedSshTransport> gateway, std::string destination,
    cardio::cancellation cancellation) {
  const SocksSocket socket{fd};
  bool request_received = false;
  bool connected = false;
  unsigned char failure_code = 1;
  try {
    std::array<unsigned char, 2> greeting{};
    co_await read_socks_async(fd, greeting, cancellation);
    if (greeting[0] != 5 || greeting[1] == 0) co_return;
    std::array<unsigned char, 255> methods{};
    const auto offered = std::span(methods).first(greeting[1]);
    co_await read_socks_async(fd, offered, cancellation);
    const bool unauthenticated = std::find(offered.begin(), offered.end(), 0) != offered.end();
    const std::array<unsigned char, 2> method_reply{5, static_cast<unsigned char>(unauthenticated ? 0 : 255)};
    co_await write_socks_async(fd, method_reply, cancellation);
    if (!unauthenticated) co_return;
    std::array<unsigned char, 4> request{};
    co_await read_socks_async(fd, request, cancellation);
    request_received = true;
    if (request[0] != 5 || request[2] != 0) throw std::invalid_argument("Invalid SOCKS request");
    if (request[1] != 1) { failure_code = 7; throw std::invalid_argument("Only SOCKS CONNECT is supported"); }
    std::string host;
    if (request[3] == 3) {
      std::array<unsigned char, 1> length{};
      co_await read_socks_async(fd, length, cancellation);
      if (!length[0]) throw std::invalid_argument("Empty SOCKS destination");
      std::array<unsigned char, 255> name{};
      co_await read_socks_async(fd, std::span(name).first(length[0]), cancellation);
      host.assign(reinterpret_cast<const char *>(name.data()), length[0]);
    } else if (request[3] == 1 || request[3] == 4) {
      std::array<unsigned char, 16> address{};
      co_await read_socks_async(fd, std::span(address).first(request[3] == 1 ? 4 : 16), cancellation);
      std::array<char, INET6_ADDRSTRLEN> text{};
      if (!::inet_ntop(request[3] == 1 ? AF_INET : AF_INET6, address.data(), text.data(), text.size()))
        throw std::invalid_argument("Invalid SOCKS address");
      host = text.data();
    } else { failure_code = 8; throw std::invalid_argument("Unsupported SOCKS address type"); }
    std::array<unsigned char, 2> port_bytes{};
    co_await read_socks_async(fd, port_bytes, cancellation);
    const auto port = static_cast<std::uint16_t>((port_bytes[0] << 8) | port_bytes[1]);
    if (!port || host.find('\0') != std::string::npos || normalized_host(host) != destination) {
      failure_code = 2;
      throw std::invalid_argument("SOCKS destination differs from the connection endpoint");
    }
    auto opening = SshChannelConnection::open_forward_async(std::move(gateway), host, port, cancellation);
    auto channel = std::move(co_await opening);
    const std::array<unsigned char, 10> reply{5, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    co_await write_socks_async(fd, reply, cancellation);
    connected = true;
    co_await bridge_ssh_channel_async(std::move(channel), fd, cancellation);
  } catch (...) {
    // Closing this one SOCKS connection reports failure to libcurl. No direct
    // path exists here, and other forwarding channels retain their gateway.
  }
  if (request_received && !connected && !cancellation.is_cancellation_requested()) {
    try {
      const std::array<unsigned char, 10> reply{5, failure_code, 0, 1, 0, 0, 0, 0, 0, 0};
      co_await write_socks_async(fd, reply, cancellation);
    } catch (...) {}
  }
}

static cardio::promise<void> serve_socks_async(std::shared_ptr<SocksState> state) {
  const auto cancellation = state->stopping.get_cancellation();
  try {
    for (;;) {
      cancellation.throw_if_cancellation_requested();
      const auto fd = ::accept4(state->listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (fd >= 0) {
        std::erase_if(state->clients, [](const auto &task) { return task.is_ready(); });
        // Bound idle handshakes as well as active forwarding channels.
        if (state->clients.size() >= 64) { ::close(fd); continue; }
        state->clients.push_back(serve_socks_client_async(fd, state->gateway, state->destination, cancellation));
      } else if (errno == EINTR) continue;
      else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        (void)co_await cardio::from_fd(state->listener,
            cardio::fd_event::read | cardio::fd_event::error | cardio::fd_event::hangup, cancellation);
      } else throw std::system_error(errno, std::generic_category(), "SSH proxy accept");
    }
  } catch (...) { state->stopping.cancel(); }
  state->close_listener();
  for (auto &client : state->clients) co_await client;
  state->clients.clear();
  state->gateway.reset();
}

cardio::promise<std::shared_ptr<SshSocksProxy>> open_ssh_socks_proxy_async(
    SshProxySettings proxy, std::string destination, TerminalSessionCallbacks callbacks,
    AuthenticatedSshTransportOptions options, cardio::cancellation cancellation) {
  if (!proxy.validation_errors.empty()) throw std::invalid_argument(proxy.validation_errors.front());
  cancellation.throw_if_cancellation_requested();
  if (!proxy.enabled) co_return nullptr;
  if (destination.empty()) throw std::invalid_argument("SSH proxy destination is required");
  auto state = std::make_shared<SocksState>();
  state->destination = normalized_host(std::move(destination));
  auto connecting = connect_ssh_gateway_async(std::move(proxy), std::move(callbacks), std::move(options), cancellation);
  state->gateway = std::move(co_await connecting);
  std::string directory = "/tmp/elder-terms-proxy-XXXXXX";
  if (!::mkdtemp(directory.data())) throw std::system_error(errno, std::generic_category(), "SSH proxy directory");
  state->directory = directory;
  state->socket_path = directory + "/socket";
  state->listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (state->listener < 0) throw std::system_error(errno, std::generic_category(), "SSH proxy listener");
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (state->socket_path.size() >= sizeof(address.sun_path)) throw std::length_error("SSH proxy socket path");
  std::copy(state->socket_path.begin(), state->socket_path.end(), address.sun_path);
  if (::bind(state->listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
      ::chmod(state->socket_path.c_str(), 0600) != 0 || ::listen(state->listener, 16) != 0)
    throw std::system_error(errno, std::generic_category(), "SSH proxy listen");
  auto result = std::make_shared<SshSocksProxy>();
  result->state = state;
  result->task.emplace(serve_socks_async(std::move(state)));
  co_return result;
}

std::string ssh_socks_proxy_url(const std::shared_ptr<SshSocksProxy> &proxy) {
  return proxy ? "socks5h://localhost" + proxy->state->socket_path : std::string();
}

cardio::promise<void> stop_ssh_socks_proxy_async(std::shared_ptr<SshSocksProxy> proxy) {
  if (!proxy) co_return;
  proxy->state->stopping.cancel();
  if (proxy->task) co_await *proxy->task;
}
} // namespace elder_terms

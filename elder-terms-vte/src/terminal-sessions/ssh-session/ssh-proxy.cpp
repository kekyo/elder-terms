#include "ssh-proxy.h"
#include "ssh-channel-connection.h"
#include "../tcp-connector.h"

#include <array>
#include <cerrno>
#include <optional>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

struct SshProxyBridge {
  int fd = -1;
  bool owns_fd = true;
  std::unique_ptr<SshChannelConnection> channel;
  cardio::cancellation_source stop;

  ~SshProxyBridge() { if (owns_fd && fd >= 0) ::close(fd); }

  void cancel() {
    stop.cancel();
    if (fd >= 0) (void)::shutdown(fd, SHUT_RDWR);
  }
};

struct SshProxyConnection {
  int fd = -1;
  std::shared_ptr<SshProxyBridge> bridge;
  std::optional<cardio::promise<void>> task;

  ~SshProxyConnection() {
    if (bridge) bridge->cancel();
    // The runner retains the bridge until both operations have stopped. FD
    // users can release their route synchronously without dangling callbacks.
    if (task && !task->is_ready()) cardio::fire_and_forget(std::move(*task));
    if (fd >= 0) ::close(fd);
  }
};

static cardio::promise<void> copy_to_gateway_async(SshProxyBridge &bridge) {
  try {
    const auto cancellation = bridge.stop.get_cancellation();
    std::array<unsigned char, 32768> buffer{};
    for (;;) {
      cancellation.throw_if_cancellation_requested();
      const auto count = ::recv(bridge.fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
      if (count > 0) {
        co_await bridge.channel->write_all_async(
            std::span<const unsigned char>(buffer.data(), count), cancellation);
      } else if (count == 0 || (count < 0 && errno == ECONNRESET)) {
        co_await bridge.channel->send_eof_async(cancellation);
        co_return;
      } else if (errno == EINTR) {
        continue;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        (void)co_await cardio::from_fd(bridge.fd,
            cardio::fd_event::read | cardio::fd_event::error | cardio::fd_event::hangup, cancellation);
      } else {
        throw std::system_error(errno, std::generic_category(), "SSH proxy socket read");
      }
    }
  } catch (...) { bridge.cancel(); }
}

static cardio::promise<void> copy_from_gateway_async(SshProxyBridge &bridge) {
  try {
    const auto cancellation = bridge.stop.get_cancellation();
    std::array<unsigned char, 32768> buffer{};
    for (;;) {
      const auto count = co_await bridge.channel->read_async(buffer, cancellation);
      if (!count) {
        (void)::shutdown(bridge.fd, SHUT_WR);
        co_return;
      }
      std::size_t offset = 0;
      while (offset < count) {
        cancellation.throw_if_cancellation_requested();
        const auto sent = ::send(bridge.fd, buffer.data() + offset, count - offset,
                                 MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent > 0) offset += sent;
        else if (sent < 0 && errno == EINTR) continue;
        else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          (void)co_await cardio::from_fd(bridge.fd,
              cardio::fd_event::write | cardio::fd_event::error | cardio::fd_event::hangup, cancellation);
        } else if (sent < 0 && (errno == EPIPE || errno == ECONNRESET)) {
          // The local peer may close after queuing its final upload bytes.
          // Drain those bytes in the other pump before sending channel EOF;
          // aborting both directions here would silently truncate the upload.
          co_return;
        } else {
          throw std::system_error(errno, std::generic_category(), "SSH proxy socket write");
        }
      }
    }
  } catch (...) { bridge.cancel(); }
}

static cardio::promise<void> run_bridge_async(std::shared_ptr<SshProxyBridge> bridge) {
  auto sending = copy_to_gateway_async(*bridge);
  auto receiving = copy_from_gateway_async(*bridge);
  co_await sending;
  co_await receiving;
  bridge->channel.reset();
}

cardio::promise<void> bridge_ssh_channel_async(
    std::unique_ptr<SshChannelConnection> channel, int fd, cardio::cancellation cancellation) {
  auto bridge = std::make_shared<SshProxyBridge>();
  bridge->fd = fd;
  bridge->owns_fd = false;
  bridge->channel = std::move(channel);
  auto registration = cancellation.on_cancellation_requested([bridge] { bridge->cancel(); });
  if (cancellation.is_cancellation_requested()) bridge->cancel();
  co_await run_bridge_async(bridge);
  // A cancellation callback may already be queued; it must not access a
  // descriptor after its caller has closed and potentially reused it.
  bridge->fd = -1;
}

cardio::promise<std::shared_ptr<AuthenticatedSshTransport>> connect_ssh_gateway_async(
    SshProxySettings proxy,
    TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (!proxy.validation_errors.empty()) throw std::invalid_argument(proxy.validation_errors.front());
  if (!proxy.enabled || proxy.endpoint.address.empty() || proxy.endpoint.port < 1 || proxy.endpoint.port > 65535) {
    throw std::invalid_argument(_("SSH proxy requires a valid gateway address and port"));
  }
  if (callbacks.ssh_prompt) {
    callbacks.ssh_prompt = [prompt_callback = callbacks.ssh_prompt](
        const SshUserPrompt &prompt, cardio::cancellation signal) -> cardio::promise<SshUserPromptResponse> {
      auto gateway_prompt = prompt;
      gateway_prompt.title = std::string(_("SSH proxy")) + ": " + prompt.title;
      co_return co_await prompt_callback(gateway_prompt, signal);
    };
  }
  std::shared_ptr<AuthenticatedSshTransport> gateway;
  options.proxy = {};
  try {
    auto connecting = AuthenticatedSshTransport::connect_async(proxy.endpoint, callbacks, std::move(options), cancellation);
    gateway = co_await connecting;
  } catch (const cardio::canceled_exception &) { throw; }
  catch (const std::exception &error) {
    throw std::runtime_error(std::string(_("SSH proxy connection failed")) + " (" + proxy.endpoint.address + "): " + error.what());
  }
  co_return gateway;
}

cardio::promise<std::shared_ptr<SshProxyConnection>> connect_ssh_proxy_async(
    SshProxySettings proxy, std::string host, std::uint16_t port,
    TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (!proxy.validation_errors.empty()) throw std::invalid_argument(proxy.validation_errors.front());
  auto result = std::make_shared<SshProxyConnection>();
  if (!proxy.enabled) {
    cardio::io_uring io(64);
    result->fd = co_await connect_tcp_socket_async(io, std::move(host), port, cancellation);
    co_return result;
  }
  auto authenticating = connect_ssh_gateway_async(std::move(proxy), std::move(callbacks), std::move(options), cancellation);
  auto gateway = std::move(co_await authenticating);
  auto bridge = std::make_shared<SshProxyBridge>();
  auto opening = SshChannelConnection::open_forward_async(std::move(gateway), std::move(host), port, cancellation);
  bridge->channel = std::move(co_await opening);
  int sockets[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
    throw std::system_error(errno, std::generic_category(), "SSH proxy socketpair");
  }
  result->fd = sockets[0];
  bridge->fd = sockets[1];
  result->bridge = bridge;
  result->task.emplace(run_bridge_async(std::move(bridge)));
  co_return result;
}

int ssh_proxy_connection_fd(const std::shared_ptr<SshProxyConnection> &connection) {
  return connection ? connection->fd : -1;
}

cardio::promise<void> stop_ssh_proxy_connection_async(std::shared_ptr<SshProxyConnection> connection) {
  if (!connection) co_return;
  if (connection->bridge) connection->bridge->cancel();
  if (connection->task) co_await *connection->task;
  connection->bridge.reset();
  if (connection->fd >= 0) ::close(std::exchange(connection->fd, -1));
}
} // namespace elder_terms

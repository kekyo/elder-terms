// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "ssh-tcp-proxy.h"
#include "ssh-proxy.h"
#include "ssh-channel-connection.h"

#include <arpa/inet.h>
#include <cerrno>
#include <optional>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace elder_terms {

struct SshTcpPeer {
  int descriptor = -1;
  std::optional<cardio::promise<void>> task;
  void close() { if (descriptor >= 0) ::close(std::exchange(descriptor, -1)); }
  ~SshTcpPeer() { close(); }
};

struct SshTcpRouteState {
  int listener = -1;
  std::uint16_t port = 0;
  std::string host;
  std::uint16_t destination_port = 0;
  std::shared_ptr<AuthenticatedSshTransport> gateway;
  std::unique_ptr<SshChannelConnection> first_channel;
  std::vector<std::unique_ptr<SshTcpPeer>> peers;
  cardio::cancellation_source stopping;

  void close_listener() {
    if (listener >= 0) ::close(std::exchange(listener, -1));
  }
  ~SshTcpRouteState() { close_listener(); }
};

struct SshTcpRoute {
  std::shared_ptr<SshTcpRouteState> state;
  std::optional<cardio::promise<void>> task;
  ~SshTcpRoute() {
    if (state) state->stopping.cancel();
    // The runner owns its descriptors until all dispatcher callbacks retire.
    if (task && !task->is_ready()) cardio::fire_and_forget(std::move(*task));
  }
};

static cardio::promise<void> serve_ssh_tcp_peer_async(SshTcpRouteState &state,
    SshTcpPeer &peer, std::unique_ptr<SshChannelConnection> channel) {
  // The accept loop owns both objects and joins every peer before releasing
  // them. Borrowing here avoids a peer owning its own coroutine frame.
  const auto cancellation = state.stopping.get_cancellation();
  try {
    if (!channel) channel = std::move(co_await SshChannelConnection::open_forward_async(
        state.gateway, state.host, state.destination_port, cancellation));
    co_await bridge_ssh_channel_async(std::move(channel), peer.descriptor, cancellation);
  } catch (...) {
    // Closing this endpoint reports route failure to the client. Other
    // channels retain their own lifetime; no request falls back to direct TCP.
  }
  peer.close();
}

static cardio::promise<void> serve_ssh_tcp_proxy_async(std::shared_ptr<SshTcpRouteState> state) {
  const auto cancellation = state->stopping.get_cancellation();
  try {
    for (;;) {
      cancellation.throw_if_cancellation_requested();
      const int accepted = ::accept4(state->listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (accepted >= 0) {
        std::erase_if(state->peers, [](const auto &peer) { return peer->task && peer->task->is_ready(); });
        auto peer = std::make_unique<SshTcpPeer>();
        peer->descriptor = accepted;
        state->peers.push_back(std::move(peer));
        auto &active = *state->peers.back();
        active.task.emplace(serve_ssh_tcp_peer_async(*state, active, std::move(state->first_channel)));
        continue;
      }
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        throw std::system_error(errno, std::generic_category(), "SSH TCP proxy accept");
      (void)co_await cardio::from_fd(state->listener,
          cardio::fd_event::read | cardio::fd_event::error | cardio::fd_event::hangup, cancellation);
    }
  } catch (...) { state->stopping.cancel(); }
  state->close_listener();
  for (auto &peer : state->peers) if (peer->task) co_await *peer->task;
  state->peers.clear();
  state->first_channel.reset();
  state->gateway.reset();
}

struct SshTcpProxy {
  std::shared_ptr<AuthenticatedSshTransport> gateway;
  std::vector<std::unique_ptr<SshTcpRoute>> routes;
  cardio::primitives::mutex operations;
  cardio::cancellation_source stopping;
  ~SshTcpProxy() { (void)stopping.cancel(); }
};

cardio::promise<std::shared_ptr<SshTcpProxy>> open_ssh_tcp_proxy_async(
    SshProxySettings proxy, TerminalSessionCallbacks callbacks,
    AuthenticatedSshTransportOptions options, cardio::cancellation cancellation) {
  if (!proxy.validation_errors.empty()) throw std::invalid_argument(proxy.validation_errors.front());
  cancellation.throw_if_cancellation_requested();
  if (!proxy.enabled) co_return nullptr;
  auto result = std::make_shared<SshTcpProxy>();
  result->gateway = std::move(co_await connect_ssh_gateway_async(
      std::move(proxy), std::move(callbacks), std::move(options), cancellation));
  co_return result;
}

cardio::promise<std::uint16_t> forward_ssh_tcp_port_async(std::shared_ptr<SshTcpProxy> proxy,
    std::string host, std::uint16_t port, cardio::cancellation cancellation) {
  if (!proxy || host.empty() || !port) throw std::invalid_argument("SSH TCP proxy destination is required");
  auto stopped = cardio::cancellations::any(cancellation, proxy->stopping.get_cancellation());
  struct CancelScope {
    cardio::cancellation_source &source;
    ~CancelScope() { (void)source.cancel(); }
  } scope{stopped};
  auto lock = std::move(co_await proxy->operations.lock(stopped.get_cancellation()));
  stopped.get_cancellation().throw_if_cancellation_requested();
  for (const auto &route : proxy->routes)
    if (route->state->host == host && route->state->destination_port == port) co_return route->state->port;
  auto state = std::make_shared<SshTcpRouteState>();
  state->gateway = proxy->gateway;
  state->host = std::move(host);
  state->destination_port = port;
  state->first_channel = std::move(co_await SshChannelConnection::open_forward_async(
      state->gateway, state->host, port, stopped.get_cancellation()));
  stopped.get_cancellation().throw_if_cancellation_requested();
  state->listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (state->listener < 0) throw std::system_error(errno, std::generic_category(), "SSH TCP proxy listener");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(state->listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
      ::listen(state->listener, 16) != 0)
    throw std::system_error(errno, std::generic_category(), "SSH TCP proxy listen");
  socklen_t length = sizeof(address);
  if (::getsockname(state->listener, reinterpret_cast<sockaddr *>(&address), &length) != 0)
    throw std::system_error(errno, std::generic_category(), "SSH TCP proxy port");
  state->port = ntohs(address.sin_port);
  auto route = std::make_unique<SshTcpRoute>();
  route->state = state;
  route->task.emplace(serve_ssh_tcp_proxy_async(state));
  proxy->routes.push_back(std::move(route));
  co_return state->port;
}

cardio::promise<void> stop_ssh_tcp_proxy_async(std::shared_ptr<SshTcpProxy> proxy) {
  if (!proxy) co_return;
  (void)proxy->stopping.cancel();
  auto lock = std::move(co_await proxy->operations.lock({}));
  for (auto &route : proxy->routes) (void)route->state->stopping.cancel();
  for (auto &route : proxy->routes) if (route->task) co_await *route->task;
  proxy->routes.clear();
  proxy->gateway.reset();
}

} // namespace elder_terms

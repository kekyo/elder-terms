// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "smb-ssh-proxy.h"
#include "../terminal-sessions/ssh-session/ssh-proxy.h"
#include "../terminal-sessions/ssh-session/ssh-channel-connection.h"

#include <arpa/inet.h>
#include <cerrno>
#include <optional>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace elder_terms {

struct SmbProxyPeer {
  int descriptor = -1;
  std::optional<cardio::promise<void>> task;
  void close() { if (descriptor >= 0) ::close(std::exchange(descriptor, -1)); }
  ~SmbProxyPeer() { close(); }
};

struct SmbProxyState {
  int listener = -1;
  std::uint16_t port = 0;
  std::string host;
  std::uint16_t destination_port = 0;
  std::shared_ptr<AuthenticatedSshTransport> gateway;
  std::unique_ptr<SshChannelConnection> first_channel;
  std::vector<std::unique_ptr<SmbProxyPeer>> peers;
  cardio::cancellation_source stopping;

  void close_listener() {
    if (listener >= 0) ::close(std::exchange(listener, -1));
  }
  ~SmbProxyState() { close_listener(); }
};

struct SmbSshProxy {
  std::shared_ptr<SmbProxyState> state;
  std::optional<cardio::promise<void>> task;
  ~SmbSshProxy() {
    if (state) state->stopping.cancel();
    // The runner owns its descriptors until all dispatcher callbacks retire.
    if (task && !task->is_ready()) cardio::fire_and_forget(std::move(*task));
  }
};

static cardio::promise<void> serve_smb_peer_async(SmbProxyState &state,
    SmbProxyPeer &peer, std::unique_ptr<SshChannelConnection> channel) {
  // The accept loop owns both objects and joins every peer before releasing
  // them. Borrowing here avoids a peer owning its own coroutine frame.
  const auto cancellation = state.stopping.get_cancellation();
  try {
    if (!channel) channel = std::move(co_await SshChannelConnection::open_forward_async(
        state.gateway, state.host, state.destination_port, cancellation));
    co_await bridge_ssh_channel_async(std::move(channel), peer.descriptor, cancellation);
  } catch (...) {
    // Closing this endpoint reports route failure to libsmb2. Other share
    // channels retain their own lifetime; no request falls back to direct TCP.
  }
  peer.close();
}

static cardio::promise<void> serve_smb_proxy_async(std::shared_ptr<SmbProxyState> state) {
  const auto cancellation = state->stopping.get_cancellation();
  try {
    for (;;) {
      cancellation.throw_if_cancellation_requested();
      const int accepted = ::accept4(state->listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (accepted >= 0) {
        std::erase_if(state->peers, [](const auto &peer) { return peer->task && peer->task->is_ready(); });
        auto peer = std::make_unique<SmbProxyPeer>();
        peer->descriptor = accepted;
        state->peers.push_back(std::move(peer));
        auto &active = *state->peers.back();
        active.task.emplace(serve_smb_peer_async(*state, active, std::move(state->first_channel)));
        continue;
      }
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        throw std::system_error(errno, std::generic_category(), "SMB SSH proxy accept");
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

cardio::promise<std::shared_ptr<SmbSshProxy>> open_smb_ssh_proxy_async(
    SshProxySettings proxy, std::string host, std::uint16_t port,
    TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options,
    cardio::cancellation cancellation) {
  if (!proxy.validation_errors.empty()) throw std::invalid_argument(proxy.validation_errors.front());
  cancellation.throw_if_cancellation_requested();
  if (!proxy.enabled) co_return nullptr;
  if (host.empty() || !port) throw std::invalid_argument("SMB SSH proxy destination is required");
  auto connecting = connect_ssh_gateway_async(std::move(proxy), std::move(callbacks), std::move(options), cancellation);
  auto gateway = std::move(co_await connecting);
  auto opening = SshChannelConnection::open_forward_async(gateway, host, port, cancellation);
  auto state = std::make_shared<SmbProxyState>();
  state->first_channel = std::move(co_await opening);
  state->gateway = std::move(gateway);
  state->host = std::move(host);
  state->destination_port = port;
  cancellation.throw_if_cancellation_requested();
  state->listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (state->listener < 0) throw std::system_error(errno, std::generic_category(), "SMB SSH proxy listener");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(state->listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
      ::listen(state->listener, 16) != 0)
    throw std::system_error(errno, std::generic_category(), "SMB SSH proxy listen");
  socklen_t length = sizeof(address);
  if (::getsockname(state->listener, reinterpret_cast<sockaddr *>(&address), &length) != 0)
    throw std::system_error(errno, std::generic_category(), "SMB SSH proxy port");
  state->port = ntohs(address.sin_port);
  auto result = std::make_shared<SmbSshProxy>();
  result->state = state;
  result->task.emplace(serve_smb_proxy_async(std::move(state)));
  co_return result;
}

std::uint16_t smb_ssh_proxy_port(const std::shared_ptr<SmbSshProxy> &proxy) {
  return proxy ? proxy->state->port : 0;
}

cardio::promise<void> stop_smb_ssh_proxy_async(std::shared_ptr<SmbSshProxy> proxy) {
  if (!proxy) co_return;
  proxy->state->stopping.cancel();
  if (proxy->task) co_await *proxy->task;
}

} // namespace elder_terms

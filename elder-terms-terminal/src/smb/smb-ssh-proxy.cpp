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

namespace elder_terms {

struct SmbProxyState {
  int listener = -1;
  int peer = -1;
  std::uint16_t port = 0;
  std::unique_ptr<SshChannelConnection> channel;
  cardio::cancellation_source stopping;

  void close_listener() {
    if (listener >= 0) ::close(std::exchange(listener, -1));
  }
  void close_peer() {
    if (peer >= 0) ::close(std::exchange(peer, -1));
  }
  ~SmbProxyState() { close_listener(); close_peer(); }
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

static cardio::promise<void> serve_smb_proxy_async(std::shared_ptr<SmbProxyState> state) {
  const auto cancellation = state->stopping.get_cancellation();
  try {
    for (;;) {
      cancellation.throw_if_cancellation_requested();
      state->peer = ::accept4(state->listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (state->peer >= 0) break;
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        throw std::system_error(errno, std::generic_category(), "SMB SSH proxy accept");
      (void)co_await cardio::from_fd(state->listener,
          cardio::fd_event::read | cardio::fd_event::error | cardio::fd_event::hangup, cancellation);
    }
    // One SMB session owns this channel. Reconnecting opens a fresh route.
    state->close_listener();
    co_await bridge_ssh_channel_async(std::move(state->channel), state->peer, cancellation);
  } catch (...) {
    // Closing the endpoint reports route failure to libsmb2 without ever
    // retrying against the SMB server directly.
    state->stopping.cancel();
  }
  state->close_listener();
  state->close_peer();
  state->channel.reset();
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
  auto opening = SshChannelConnection::open_forward_async(std::move(gateway), std::move(host), port, cancellation);
  auto state = std::make_shared<SmbProxyState>();
  state->channel = std::move(co_await opening);
  cancellation.throw_if_cancellation_requested();
  state->listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (state->listener < 0) throw std::system_error(errno, std::generic_category(), "SMB SSH proxy listener");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(state->listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
      ::listen(state->listener, 1) != 0)
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

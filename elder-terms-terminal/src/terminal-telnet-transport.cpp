// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-transports.h"
#include "terminal-socket-read.h"
#include "../../shared/src/connection-runtime/ssh/ssh-proxy.h"
#include "terminal-sessions/telnet-session/telnet-protocol.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <system_error>

namespace elder_terms {

struct TelnetChannel {
  std::shared_ptr<SshProxyConnection> route;
  TelnetProtocol protocol;
  TerminalSessionCallbacks callbacks;
  cardio::primitives::mutex write_lock;
  std::vector<unsigned char> pending;
  std::size_t offset = 0;

  explicit TelnetChannel(std::string terminal_type) : protocol(std::move(terminal_type)) {}
};

static cardio::promise<void> write_wire(TelnetChannel *channel,
    std::span<const unsigned char> bytes, cardio::cancellation cancellation) {
  auto acquiring = channel->write_lock.lock(cancellation);
  auto lock = std::move(co_await acquiring);
  const auto fd = ssh_proxy_connection_fd(channel->route);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    co_await cardio::from_fd(fd, cardio::fd_event::write, cancellation);
    const auto count = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      if (channel->callbacks.activity) channel->callbacks.activity(ActivityIndicatorId::sd);
    }
    else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      throw std::system_error(count == 0 ? EPIPE : errno, std::generic_category(), "write TELNET");
  }
}

static cardio::promise<std::size_t> read_telnet(TelnetChannel *channel,
    std::span<unsigned char> bytes, cardio::cancellation cancellation) {
  const auto fd = ssh_proxy_connection_fd(channel->route);
  while (channel->offset == channel->pending.size()) {
    const auto count = co_await read_terminal_socket_async(fd, bytes, cancellation);
    if (count == 0) {
      channel->pending = channel->protocol.finish_receive();
      channel->offset = 0;
      if (channel->pending.empty()) co_return 0;
      break;
    }
    if (channel->callbacks.activity) channel->callbacks.activity(ActivityIndicatorId::rd);
    auto result = channel->protocol.receive(bytes.first(count));
    channel->pending = std::move(result.terminal_data);
    channel->offset = 0;
    // Keep protocol replies ordered with application writes, without adding an
    // unbounded response queue or feeding negotiation bytes to the terminal.
    for (const auto &response : result.responses) co_await write_wire(channel, response, cancellation);
  }
  const auto count = std::min(bytes.size(), channel->pending.size() - channel->offset);
  std::memcpy(bytes.data(), channel->pending.data() + channel->offset, count);
  channel->offset += count;
  co_return count;
}

static cardio::promise<void> write_telnet(TelnetChannel *channel,
    std::span<const unsigned char> bytes, cardio::cancellation cancellation) {
  const auto encoded = channel->protocol.encode_user_input(bytes);
  co_await write_wire(channel, encoded, cancellation);
}

static cardio::promise<void> resize_telnet(TelnetChannel *channel,
    std::size_t rows, std::size_t columns, cardio::cancellation cancellation) {
  channel->protocol.set_window_size(static_cast<std::uint16_t>(std::min<std::size_t>(columns, 65535)),
      static_cast<std::uint16_t>(std::min<std::size_t>(rows, 65535)));
  if (channel->protocol.is_naws_enabled()) {
    const auto bytes = channel->protocol.encode_naws();
    co_await write_wire(channel, bytes, cancellation);
  }
}

static cardio::promise<TerminalTransport> open_telnet(TelnetConnectionSettings settings,
    TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options,
    std::size_t rows, std::size_t columns, cardio::cancellation cancellation) {
  if (settings.port <= 0 || settings.port > 65535 || settings.address.empty())
    throw std::system_error(EINVAL, std::generic_category(), "TELNET destination");
  auto channel = std::make_shared<TelnetChannel>(settings.terminal_type);
  channel->callbacks = callbacks;
  channel->route = co_await connect_ssh_proxy_async(options.proxy, settings.address,
      static_cast<std::uint16_t>(settings.port), callbacks, options, cancellation);
  std::exception_ptr failure;
  try {
    co_await resize_telnet(channel.get(), rows, columns, cancellation);
    for (const auto &bytes : channel->protocol.encode_enable_binary()) co_await write_wire(channel.get(), bytes, cancellation);
  } catch (...) { failure = std::current_exception(); }
  if (failure) {
    co_await stop_ssh_proxy_connection_async(channel->route);
    std::rethrow_exception(failure);
  }
  co_return TerminalTransport{
      [channel](std::span<unsigned char> bytes, cardio::cancellation stop) { return read_telnet(channel.get(), bytes, stop); },
      [channel](std::span<const unsigned char> bytes, cardio::cancellation stop) { return write_telnet(channel.get(), bytes, stop); },
      [channel](std::size_t height, std::size_t width, cardio::cancellation stop) { return resize_telnet(channel.get(), height, width, stop); },
      [channel] { return stop_ssh_proxy_connection_async(channel->route); },
      [channel] {
        std::vector<unsigned char> bytes(channel->pending.begin() + channel->offset, channel->pending.end());
        const auto tail = channel->protocol.finish_receive();
        bytes.insert(bytes.end(), tail.begin(), tail.end());
        channel->pending.clear();
        channel->offset = 0;
        return bytes;
      },
      [channel](cardio::cancellation stop) -> cardio::promise<void> {
        const auto bytes = channel->protocol.encode_break();
        co_await write_wire(channel.get(), bytes, stop);
      }, {}, {}, true};
}

TerminalTransportRequest make_telnet_transport_request(TelnetConnectionSettings settings,
    std::string encoding, TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options) {
  TerminalTransportRequest request;
  request.encoding = std::move(encoding);
  request.open = [settings = std::move(settings), callbacks = std::move(callbacks), options = std::move(options)](
      std::size_t rows, std::size_t columns, cardio::cancellation cancellation) {
    return open_telnet(settings, callbacks, options, rows, columns, cancellation);
  };
  return request;
}

}

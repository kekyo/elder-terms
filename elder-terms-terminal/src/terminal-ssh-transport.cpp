// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-transports.h"
#include "../../shared/src/connection-runtime/ssh/ssh-channel-connection.h"

namespace elder_terms {

struct SshShellChannel {
  std::unique_ptr<SshChannelConnection> channel;
};

static cardio::promise<TerminalTransport> open_ssh(SshConnectionSettings settings,
    TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options,
    std::size_t rows, std::size_t columns, cardio::cancellation cancellation) {
  auto connecting = SshChannelConnection::connect_async(settings, static_cast<glong>(columns),
      static_cast<glong>(rows), callbacks, std::move(options), cancellation);
  auto shell = std::make_shared<SshShellChannel>();
  shell->channel = std::move(co_await connecting);
  co_return TerminalTransport{
      [shell](std::span<unsigned char> bytes, cardio::cancellation stop) { return shell->channel->read_async(bytes, stop); },
      [shell](std::span<const unsigned char> bytes, cardio::cancellation stop) { return shell->channel->write_all_async(bytes, stop); },
      [shell](std::size_t height, std::size_t width, cardio::cancellation stop) {
        return shell->channel->resize_async(static_cast<glong>(width), static_cast<glong>(height), stop);
      },
      [shell]() -> cardio::promise<void> { shell->channel.reset(); co_return; }, {},
      [shell](cardio::cancellation stop) { return shell->channel->send_break_async(500, stop); },
      [shell] { return shell->channel ? shell->channel->authenticated_transport() : nullptr; },
      {}, true};
}

TerminalTransportRequest make_ssh_transport_request(SshConnectionSettings settings,
    std::string encoding, TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options) {
  TerminalTransportRequest request;
  request.encoding = std::move(encoding);
  request.open = [settings = std::move(settings), callbacks = std::move(callbacks), options = std::move(options)](
      std::size_t rows, std::size_t columns, cardio::cancellation cancellation) {
    return open_ssh(settings, callbacks, options, rows, columns, cancellation);
  };
  return request;
}

}

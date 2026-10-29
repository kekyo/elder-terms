// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <arpa/inet.h>
#include <libssh/callbacks.h>
#include <libssh/server.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// This fixture maps the requested remote hostname to a test server (loopback by
// default) and records every TCP forwarding request, including passive FTP.
struct Forward {
  ssh_channel channel = nullptr;
  ssh_event event = nullptr;
  int fd = -1;
  bool watching = false;
  bool close_requested = false;
  bool peer_eof = false;
  bool eof_sent = false;
  std::array<unsigned char, 32768> pending{};
  std::size_t pending_size = 0;
  std::size_t pending_offset = 0;
  ssh_channel_callbacks_struct callbacks{};
};
struct Gateway {
  ssh_event event = nullptr;
  in_addr destination{htonl(INADDR_LOOPBACK)};
  std::vector<std::unique_ptr<Forward>> forwards;
};

static int on_none(ssh_session, const char *, void *) { return SSH_AUTH_SUCCESS; }
static void close_peer(Forward &forward) {
  if (forward.fd >= 0) {
    if (forward.watching) ssh_event_remove_fd(forward.event, forward.fd);
    forward.watching = false;
    ::close(forward.fd);
    forward.fd = -1;
  }
}
static int on_channel_data(ssh_session, ssh_channel, void *data, std::uint32_t size, int, void *opaque) {
  auto &forward = *static_cast<Forward *>(opaque);
  std::size_t offset = 0;
  while (offset < size) {
    const auto count = ::send(forward.fd, static_cast<unsigned char *>(data) + offset, size - offset, MSG_NOSIGNAL);
    if (count > 0) offset += count;
    else if (count < 0 && errno == EINTR) continue;
    else { forward.close_requested = true; return -1; }
  }
  return size;
}
static void on_channel_eof(ssh_session, ssh_channel, void *opaque) {
  auto &forward = *static_cast<Forward *>(opaque);
  if (forward.fd >= 0) (void)::shutdown(forward.fd, SHUT_WR);
}
static void on_channel_close(ssh_session, ssh_channel, void *opaque) {
  static_cast<Forward *>(opaque)->close_requested = true;
}
static int on_peer(socket_t fd, int, void *opaque) {
  auto &forward = *static_cast<Forward *>(opaque);
  if (forward.close_requested || forward.pending_size || forward.peer_eof) return 0;
  const auto count = ::recv(fd, forward.pending.data(), forward.pending.size(), MSG_DONTWAIT);
  if (count > 0) {
    forward.pending_size = static_cast<std::size_t>(count);
  } else if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
    forward.peer_eof = true;
  }
  return 0;
}
static void flush_peer(Forward &forward) {
  if (forward.fd < 0) return;
  // Removing an fd inside any event callback invalidates libssh's poll walk.
  if (forward.close_requested) { close_peer(forward); return; }
  // libssh writes can dispatch events even in nonblocking mode. Drain outside
  // the fd callback, with its watcher detached until the bounded buffer clears.
  if ((forward.pending_size || forward.peer_eof) && forward.watching) {
    ssh_event_remove_fd(forward.event, forward.fd);
    forward.watching = false;
  }
  if (forward.pending_offset < forward.pending_size) {
    const auto sent = ssh_channel_write(forward.channel,
        forward.pending.data() + forward.pending_offset,
        static_cast<std::uint32_t>(forward.pending_size - forward.pending_offset));
    if (sent == SSH_ERROR) { close_peer(forward); return; }
    if (sent > 0) forward.pending_offset += static_cast<std::size_t>(sent);
  }
  if (forward.close_requested) { close_peer(forward); return; }
  if (forward.pending_offset < forward.pending_size) return;
  forward.pending_offset = forward.pending_size = 0;
  if (forward.peer_eof) {
    if (!forward.eof_sent) {
      const auto result = ssh_channel_send_eof(forward.channel);
      if (result == SSH_OK) forward.eof_sent = true;
      else if (result == SSH_ERROR) close_peer(forward);
    }
  } else if (!forward.watching) {
    if (ssh_event_add_fd(forward.event, forward.fd, POLLIN, on_peer, &forward) == SSH_OK)
      forward.watching = true;
    else close_peer(forward);
  }
}
static int on_message(ssh_session, ssh_message message, void *opaque) {
  auto &gateway = *static_cast<Gateway *>(opaque);
  if (ssh_message_type(message) != SSH_REQUEST_CHANNEL_OPEN ||
      ssh_message_subtype(message) != SSH_CHANNEL_DIRECT_TCPIP) return 1;
  const char *host = ssh_message_channel_request_open_destination(message);
  const int port = ssh_message_channel_request_open_destination_port(message);
  if (!host || port < 1 || port > 65535) return 1;
  const std::string name(host);
  if (name != "proxy-test.invalid" && name != "127.0.0.1" && name != "localhost" && name != "::1") return 1;
  auto forward = std::make_unique<Forward>();
  forward->event = gateway.event;
  forward->fd = ::socket(name == "::1" ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_addr = name == "proxy-test.invalid" ? gateway.destination : in_addr{htonl(INADDR_LOOPBACK)};
  peer.sin_port = htons(port);
  sockaddr_in6 peer6{};
  peer6.sin6_family = AF_INET6;
  peer6.sin6_addr = in6addr_loopback;
  peer6.sin6_port = htons(port);
  const auto *target = name == "::1" ? reinterpret_cast<sockaddr *>(&peer6) : reinterpret_cast<sockaddr *>(&peer);
  if (forward->fd < 0 || ::connect(forward->fd, target, name == "::1" ? sizeof(peer6) : sizeof(peer)) != 0) {
    close_peer(*forward);
    return 1;
  }
  forward->channel = ssh_message_channel_request_open_reply_accept(message);
  if (!forward->channel) { close_peer(*forward); return 1; }
  forward->callbacks.userdata = forward.get();
  forward->callbacks.channel_data_function = on_channel_data;
  forward->callbacks.channel_eof_function = on_channel_eof;
  forward->callbacks.channel_close_function = on_channel_close;
  ssh_callbacks_init(&forward->callbacks);
  if (ssh_set_channel_callbacks(forward->channel, &forward->callbacks) != SSH_OK ||
      ssh_event_add_fd(gateway.event, forward->fd, POLLIN, on_peer, forward.get()) != SSH_OK) return 1;
  forward->watching = true;
  std::cout << "FORWARD " << host << " " << port << std::endl;
  gateway.forwards.push_back(std::move(forward));
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) return 2;
  Gateway gateway;
  if (argc == 3 && ::inet_pton(AF_INET, argv[2], &gateway.destination) != 1) return 2;
  ::signal(SIGPIPE, SIG_IGN);
  ::alarm(300);
  ssh_bind listener = ssh_bind_new();
  unsigned int port = 0;
  if (!listener || ssh_bind_options_set(listener, SSH_BIND_OPTIONS_BINDADDR, "127.0.0.1") != SSH_OK ||
      ssh_bind_options_set(listener, SSH_BIND_OPTIONS_BINDPORT, &port) != SSH_OK ||
      ssh_bind_options_set(listener, SSH_BIND_OPTIONS_HOSTKEY, argv[1]) != SSH_OK || ssh_bind_listen(listener) != SSH_OK) return 3;
  sockaddr_in address{};
  socklen_t size = sizeof(address);
  if (::getsockname(ssh_bind_get_fd(listener), reinterpret_cast<sockaddr *>(&address), &size) != 0) return 4;
  std::cout << "PORT " << ntohs(address.sin_port) << std::endl;
  ssh_session session = ssh_new();
  if (!session || ssh_bind_accept(listener, session) != SSH_OK) return 5;
  ssh_server_callbacks_struct callbacks{};
  callbacks.auth_none_function = on_none;
  ssh_callbacks_init(&callbacks);
  if (ssh_set_server_callbacks(session, &callbacks) != SSH_OK) return 6;
  ssh_set_auth_methods(session, SSH_AUTH_METHOD_NONE);
  ssh_set_message_callback(session, on_message, &gateway);
  if (ssh_handle_key_exchange(session) != SSH_OK) return 7;
  ssh_set_blocking(session, 0);
  gateway.event = ssh_event_new();
  if (!gateway.event || ssh_event_add_session(gateway.event, session) != SSH_OK) return 8;
  while (ssh_is_connected(session) && ssh_event_dopoll(gateway.event, -1) != SSH_ERROR) {
    // A write may accept another channel, so do not retain vector iterators.
    for (std::size_t i = 0; i < gateway.forwards.size(); ++i) flush_peer(*gateway.forwards[i]);
  }
  for (auto &forward : gateway.forwards) {
    close_peer(*forward);
    ssh_channel_free(forward->channel);
  }
  ssh_event_remove_session(gateway.event, session);
  ssh_event_free(gateway.event);
  ssh_disconnect(session);
  ssh_free(session);
  ssh_bind_free(listener);
  return gateway.forwards.empty() ? 9 : 0;
}

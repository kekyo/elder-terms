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

// This fixture maps the requested remote hostname to loopback and records every
// TCP forwarding request, including FTP's dynamically chosen passive ports.
struct Forward {
  ssh_channel channel = nullptr;
  ssh_event event = nullptr;
  int fd = -1;
  ssh_channel_callbacks_struct callbacks{};
};
struct Gateway {
  ssh_event event = nullptr;
  std::vector<std::unique_ptr<Forward>> forwards;
};

static int on_none(ssh_session, const char *, void *) { return SSH_AUTH_SUCCESS; }
static void close_peer(Forward &forward) {
  if (forward.fd >= 0) {
    ssh_event_remove_fd(forward.event, forward.fd);
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
    else { close_peer(forward); return -1; }
  }
  return size;
}
static void on_channel_eof(ssh_session, ssh_channel, void *opaque) {
  auto &forward = *static_cast<Forward *>(opaque);
  if (forward.fd >= 0) (void)::shutdown(forward.fd, SHUT_WR);
}
static void on_channel_close(ssh_session, ssh_channel, void *opaque) { close_peer(*static_cast<Forward *>(opaque)); }
static int on_peer(socket_t fd, int, void *opaque) {
  auto &forward = *static_cast<Forward *>(opaque);
  std::array<unsigned char, 32768> buffer{};
  const auto count = ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
  if (count > 0) {
    std::size_t offset = 0;
    while (offset < static_cast<std::size_t>(count)) {
      const auto sent = ssh_channel_write(forward.channel, buffer.data() + offset, count - offset);
      if (sent <= 0) { close_peer(forward); return 0; }
      offset += sent;
    }
  } else if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
    (void)ssh_channel_send_eof(forward.channel);
    ssh_event_remove_fd(forward.event, fd);
  }
  return 0;
}
static int on_message(ssh_session, ssh_message message, void *opaque) {
  auto &gateway = *static_cast<Gateway *>(opaque);
  if (ssh_message_type(message) != SSH_REQUEST_CHANNEL_OPEN ||
      ssh_message_subtype(message) != SSH_CHANNEL_DIRECT_TCPIP) return 1;
  const char *host = ssh_message_channel_request_open_destination(message);
  const int port = ssh_message_channel_request_open_destination_port(message);
  if (!host || port < 1 || port > 65535) return 1;
  const std::string name(host);
  if (name != "proxy-test.invalid" && name != "127.0.0.1" && name != "localhost") return 1;
  auto forward = std::make_unique<Forward>();
  forward->event = gateway.event;
  forward->fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  peer.sin_port = htons(port);
  if (forward->fd < 0 || ::connect(forward->fd, reinterpret_cast<sockaddr *>(&peer), sizeof(peer)) != 0) {
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
  std::cout << "FORWARD " << host << " " << port << std::endl;
  gateway.forwards.push_back(std::move(forward));
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
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
  Gateway gateway;
  ssh_server_callbacks_struct callbacks{};
  callbacks.auth_none_function = on_none;
  ssh_callbacks_init(&callbacks);
  if (ssh_set_server_callbacks(session, &callbacks) != SSH_OK) return 6;
  ssh_set_auth_methods(session, SSH_AUTH_METHOD_NONE);
  ssh_set_message_callback(session, on_message, &gateway);
  if (ssh_handle_key_exchange(session) != SSH_OK) return 7;
  gateway.event = ssh_event_new();
  if (!gateway.event || ssh_event_add_session(gateway.event, session) != SSH_OK) return 8;
  while (ssh_is_connected(session) && ssh_event_dopoll(gateway.event, -1) != SSH_ERROR) {}
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

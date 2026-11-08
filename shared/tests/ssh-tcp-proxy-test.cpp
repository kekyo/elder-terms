// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../src/connection-runtime/ssh/ssh-tcp-proxy.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <iostream>

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> exercise(char **argv) {
  using namespace elder_terms;
  SshProxySettings settings{.enabled = true,
      .endpoint = {.address = "127.0.0.1", .port = std::stoi(argv[1]), .username = "gateway-user", .identity_file = {}}};
  int prompts = 0;
  TerminalSessionCallbacks callbacks{};
  callbacks.ssh_prompt = [&](const SshUserPrompt &prompt, cardio::cancellation) -> cardio::promise<SshUserPromptResponse> {
    require(prompt.title.starts_with("SSH proxy"), "Identify gateway authentication");
    ++prompts;
    co_return SshUserPromptResponse{.accepted = true, .text = prompt.initial_text};
  };
  auto proxy = co_await open_ssh_tcp_proxy_async(settings, callbacks, {.known_hosts_file = argv[4], .config_file = {}}, {});
  std::array<std::uint16_t, 2> ports{};
  for (int index = 0; index < 2; ++index)
    ports[index] = co_await forward_ssh_tcp_port_async(proxy, "proxy-test.invalid", std::stoi(argv[index + 2]), {});
  require(ports[0] != ports[1], "Different destinations need distinct loopback endpoints");
  for (const int index : {0, 1, 0}) {
    require(ports[index] == co_await forward_ssh_tcp_port_async(proxy, "proxy-test.invalid", std::stoi(argv[index + 2]), {}),
        "Repeated destinations must retain their endpoint");
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    require(fd >= 0, "Create test socket");
    struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(ports[index]);
    const auto connected = ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    require(connected == 0 || errno == EINPROGRESS, "Connect to loopback endpoint");
    (void)co_await cardio::from_fd(fd, cardio::fd_event::write, {});
    const std::string payload = "forwarded payload";
    require(::send(fd, payload.data(), payload.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()), "Send test payload");
    ::shutdown(fd, SHUT_WR);
    std::string response;
    std::array<char, 128> buffer{};
    for (;;) {
      const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
      if (count > 0) response.append(buffer.data(), count);
      else if (count == 0) break;
      else {
        require(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR, "Read forwarded response");
        (void)co_await cardio::from_fd(fd, cardio::fd_event::read, {});
      }
    }
    require(response == std::to_string(index) + ":" + payload, "Route bytes to the requested destination");
  }
  require(prompts == 2, "Multiple destinations must share a single gateway authentication");
  co_await stop_ssh_tcp_proxy_async(proxy);
  co_await stop_ssh_tcp_proxy_async(proxy);
  bool stopped = false;
  try { (void)co_await forward_ssh_tcp_port_async(proxy, "proxy-test.invalid", std::stoi(argv[2]), {}); }
  catch (const cardio::canceled_exception &) { stopped = true; }
  require(stopped, "A stopped proxy must not reopen an endpoint");
  for (const auto port : ports) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "Create closure-check socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const auto status = ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    ::close(fd);
    require(status != 0, "Shutdown must close every forwarding listener");
  }
  std::cout << "PASS shared SSH TCP destinations, reuse, authentication, bytes and shutdown\n";
}

static cardio::promise<void> run(cardio::dispatcher_group_glib &group, char **argv, std::exception_ptr &failure) {
  try { co_await exercise(argv); } catch (...) { failure = std::current_exception(); }
  group.shutdown();
}

int main(int argc, char **argv) {
  if (argc != 5) return 2;
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  std::exception_ptr failure;
  auto pending = run(group, argv, failure);
  dispatcher.park();
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
    return 1;
  }
}

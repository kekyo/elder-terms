#include "control-socket.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>

static void print_usage() {
  std::cout << "Usage: etctl open-application\n"
               "       etctl open-connection <saved-connection-name>\n"
               "Controls the running launcher in the same XDG_RUNTIME_DIR.\n";
}

static std::runtime_error io_error() {
  return std::runtime_error(std::string("Control socket: ") + std::strerror(errno));
}

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    print_usage();
    return 0;
  }
  const std::string command = argc > 1 ? argv[1] : "";
  if (!((command == "open-application" && argc == 2) ||
        (command == "open-connection" && argc == 3 && argv[2][0] != '\0'))) {
    print_usage();
    return 2;
  }
  try {
    const auto address = elder_terms::control_socket_address(
        elder_terms::control_socket_path());
    elder_terms::ControlDescriptor socket;
    socket.value = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const timeval timeout {.tv_sec = 10, .tv_usec = 0};
    if (socket.value < 0 ||
        setsockopt(socket.value, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(socket.value, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
      throw io_error();
    }
    if (connect(socket.value, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
      throw std::runtime_error("Cannot reach elder-terms. Start the launcher in this desktop session first.");
    }
    if (!elder_terms::control_peer_is_owner(socket.value)) {
      throw std::runtime_error("The control socket belongs to a different user");
    }
    const char *token = std::getenv("XDG_ACTIVATION_TOKEN");
    const std::string packet = command + '\0' +
        (command == "open-connection" ? argv[2] : "") + '\0' +
        (token == nullptr ? "" : token);
    if (packet.size() > elder_terms::control_request_limit) {
      throw std::runtime_error("Control request is too large");
    }
    std::size_t offset = 0;
    while (offset < packet.size()) {
      const auto count = send(socket.value, packet.data() + offset,
                              packet.size() - offset, MSG_NOSIGNAL);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        throw io_error();
      }
      offset += static_cast<std::size_t>(count);
    }
    if (shutdown(socket.value, SHUT_WR) != 0) {
      throw io_error();
    }
    std::string response;
    std::array<char, 1024> buffer {};
    while (true) {
      const auto count = recv(socket.value, buffer.data(), buffer.size(), 0);
      if (count == 0) {
        break;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0) {
        throw io_error();
      }
      response.append(buffer.data(), static_cast<std::size_t>(count));
      if (response.size() > elder_terms::control_request_limit) {
        throw std::runtime_error("Invalid control response");
      }
    }
    if (response != "OK\n") {
      throw std::runtime_error(response.starts_with("ERROR ")
                                   ? response.substr(6) : "Invalid control response");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "etctl: " << error.what() << '\n';
    return 1;
  }
}

#include "control-socket.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static void print_usage() {
  std::cout << "Usage: etctl open-application\n"
               "       etctl open-connection <saved-connection-name>\n"
               "       etctl setup\n"
               "Controls the launcher in the same XDG_RUNTIME_DIR.\n"
               "Setup starts the launcher when needed.\n";
}

static std::runtime_error io_error() {
  return std::runtime_error(std::string("Control socket: ") + std::strerror(errno));
}

class LauncherUnavailable : public std::runtime_error {
public:
  LauncherUnavailable()
      : std::runtime_error("Cannot reach elder-terms. Start the launcher in this desktop session first.") {}
};

static std::string send_request(const std::string &command,
                                const std::string &connection) {
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
      if (errno == ENOENT || errno == ECONNREFUSED) {
        throw LauncherUnavailable();
      }
      throw io_error();
    }
    if (!elder_terms::control_peer_is_owner(socket.value)) {
      throw std::runtime_error("The control socket belongs to a different user");
    }
    const char *token = std::getenv("XDG_ACTIVATION_TOKEN");
    const std::string packet = command + '\0' +
        connection + '\0' +
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
    return response;
}

static pid_t start_launcher() {
  const auto executable =
      std::filesystem::read_symlink("/proc/self/exe").parent_path() /
      "elder-terms";
  const std::string path = executable.string();
  char *const arguments[] = {
      const_cast<char *>(path.c_str()),
      const_cast<char *>("--autostart"),
      nullptr,
  };
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (error != 0) {
    throw std::runtime_error("Cannot prepare launcher: " +
                             std::string(std::strerror(error)));
  }
  for (const auto &[descriptor, flags] : {
           std::pair{STDIN_FILENO, O_RDONLY},
           std::pair{STDOUT_FILENO, O_WRONLY},
           std::pair{STDERR_FILENO, O_WRONLY}}) {
    error = posix_spawn_file_actions_addopen(
        &actions, descriptor, "/dev/null", flags, 0);
    if (error != 0) {
      (void)posix_spawn_file_actions_destroy(&actions);
      throw std::runtime_error("Cannot prepare launcher output: " +
                               std::string(std::strerror(error)));
    }
  }
  pid_t child = 0;
  error = posix_spawn(&child, path.c_str(), &actions, nullptr,
                      arguments, environ);
  (void)posix_spawn_file_actions_destroy(&actions);
  if (error != 0) {
    throw std::runtime_error("Cannot start elder-terms: " +
                             std::string(std::strerror(error)));
  }
  return child;
}

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    print_usage();
    return 0;
  }
  const std::string command = argc > 1 ? argv[1] : "";
  if (!((command == "open-application" && argc == 2) ||
        (command == "setup" && argc == 2) ||
        (command == "open-connection" && argc == 3 && argv[2][0] != '\0'))) {
    print_usage();
    return 2;
  }
  try {
    const std::string connection = command == "open-connection" ? argv[2] : "";
    bool launched = false;
    pid_t launched_pid = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(120);
    while (true) {
      try {
        const std::string response = send_request(command, connection);
        if (response == "OK\n") {
          return 0;
        }
        if (response.starts_with("OK ") && response.ends_with('\n')) {
          std::cout << response.substr(3);
          return 0;
        }
        if (command == "setup" && response.starts_with("PENDING ") &&
            std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
          continue;
        }
        if (response.starts_with("ERROR ") && response.ends_with('\n')) {
          throw std::runtime_error(response.substr(6, response.size() - 7));
        }
        throw std::runtime_error(
            response.starts_with("PENDING ")
                ? "Timed out waiting for hotkey registration"
                : "Invalid control response");
      } catch (const LauncherUnavailable &) {
        if (command != "setup") {
          throw;
        }
        if (!launched) {
          const char *display = std::getenv("DISPLAY");
          const char *wayland = std::getenv("WAYLAND_DISPLAY");
          if ((display == nullptr || *display == '\0') &&
              (wayland == nullptr || *wayland == '\0')) {
            throw std::runtime_error(
                "Run setup inside a graphical desktop session");
          }
          launched_pid = start_launcher();
          launched = true;
        } else {
          int status = 0;
          if (waitpid(launched_pid, &status, WNOHANG) == launched_pid) {
            throw std::runtime_error(
                "The launcher exited before hotkey setup completed");
          }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          throw std::runtime_error("Timed out waiting for the launcher");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  } catch (const std::exception &error) {
    std::cerr << "etctl: " << error.what() << '\n';
    return 1;
  }
}

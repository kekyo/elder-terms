#include "control-server.h"
#include "control-socket.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <cardio.h>

namespace elder_terms {

struct ControlServerState {
  ControlCallback callback;
  std::string path;
  ControlDescriptor lock;
  ControlDescriptor listener;
  bool bound = false;
  cardio::cancellation_source cancellation;
  cardio::promise<void> accepting;
  std::vector<cardio::promise<void>> clients;
};

static std::runtime_error socket_error(const char *operation) {
  return std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

static std::string dispatch_request(ControlServerState *state,
                                    const std::string &packet) {
  // One half-closed stream carries command NUL connection NUL token.
  // NUL separation preserves whitespace, quotes and UTF-8 in connection names.
  const auto first = packet.find('\0');
  const auto second = first == std::string::npos
                          ? std::string::npos : packet.find('\0', first + 1);
  if (second == std::string::npos ||
      packet.find('\0', second + 1) != std::string::npos) {
    return "ERROR Invalid control request\n";
  }
  const std::string command = packet.substr(0, first);
  const std::string connection = packet.substr(first + 1, second - first - 1);
  const std::string token = packet.substr(second + 1);
  if ((command != "open-application" && command != "open-connection" &&
       command != "setup") ||
      ((command == "open-application" || command == "setup") &&
       !connection.empty()) ||
      (command == "open-connection" && connection.empty())) {
    return "ERROR Invalid control command\n";
  }
  const ControlReply reply = state->callback({
      .command = command == "setup"
                     ? ControlCommand::setup
                     : command == "open-connection"
                           ? ControlCommand::open_connection
                           : ControlCommand::open_application,
      .connection = command == "open-connection"
                        ? std::optional<std::string>(connection) : std::nullopt,
      .activation_token = token.empty() ? std::nullopt
                                       : std::optional<std::string>(token),
  });
  if (reply.pending) {
    return "PENDING " + reply.message + "\n";
  }
  if (!reply.success) {
    return "ERROR " + reply.message + "\n";
  }
  return reply.message.empty() ? "OK\n" : "OK " + reply.message + "\n";
}

static cardio::promise<void> serve_client(ControlServerState *state, int fd) {
  ControlDescriptor client;
  client.value = fd;
  try {
    const auto deadline = cardio::cancellations::timeout(10000);
    const auto cancellation = cardio::cancellations::any(
        state->cancellation.get_cancellation(), deadline.get_cancellation());
    std::string packet;
    bool oversized = false;
    std::array<char, 4096> buffer {};
    while (true) {
      const auto count = recv(fd, buffer.data(), buffer.size(), 0);
      if (count == 0) {
        break;
      }
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          co_return;
        }
        (void)co_await cardio::from_fd(
            fd, cardio::fd_event::read, cancellation.get_cancellation());
        continue;
      }
      if (!oversized && packet.size() + static_cast<std::size_t>(count) <=
                            control_request_limit) {
        packet.append(buffer.data(), static_cast<std::size_t>(count));
      } else {
        oversized = true;
      }
    }
    const std::string response = oversized
        ? "ERROR Control request is too large\n" : dispatch_request(state, packet);
    std::size_t offset = 0;
    while (offset < response.size()) {
      const auto count = send(fd, response.data() + offset,
                              response.size() - offset, MSG_NOSIGNAL);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        (void)co_await cardio::from_fd(
            fd, cardio::fd_event::write, cancellation.get_cancellation());
      } else {
        co_return;
      }
    }
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) {
    std::cerr << "Control request failed: " << error.what() << '\n';
  }
}

static cardio::promise<void> accept_clients(ControlServerState *state) {
  try {
    while (true) {
      (void)co_await cardio::from_fd(
          state->listener.value, cardio::fd_event::read,
          state->cancellation.get_cancellation());
      const int fd = accept4(state->listener.value, nullptr, nullptr,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        throw socket_error("accept");
      }
      std::erase_if(state->clients, [](const auto &task) { return task.is_ready(); });
      if (state->clients.size() >= 32 || !control_peer_is_owner(fd)) {
        close(fd);
        continue;
      }
      state->clients.emplace_back(serve_client(state, fd));
    }
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) {
    std::cerr << "Control listener failed: " << error.what() << '\n';
  }
}

ControlServerState *create_control_server(ControlCallback callback) {
  std::unique_ptr<ControlServerState, decltype(&destroy_control_server)> state(
      new ControlServerState(), destroy_control_server);
  state->callback = std::move(callback);
  state->path = control_socket_path();
  const auto address = control_socket_address(state->path);
  const auto directory = std::filesystem::path(state->path).parent_path();
  if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
    throw socket_error("create control directory");
  }
  struct stat status {};
  if (lstat(directory.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 0077) != 0) {
    throw std::runtime_error("The control directory is not private or owned by you");
  }
  const auto lock_path = directory / "control.lock";
  state->lock.value = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (state->lock.value < 0 || fstat(state->lock.value, &status) != 0 ||
      !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
      status.st_nlink != 1 || (status.st_mode & 0077) != 0) {
    throw std::runtime_error("Cannot open the private control lock");
  }
  if (flock(state->lock.value, LOCK_EX | LOCK_NB) != 0) {
    throw std::runtime_error("Another launcher already owns the control socket");
  }
  // Keep the lock file permanently: unlinking it could let two launchers lock
  // different inodes. Only its owner may reclaim an abandoned socket.
  if (lstat(state->path.c_str(), &status) == 0) {
    if (!S_ISSOCK(status.st_mode) || status.st_uid != geteuid()) {
      throw std::runtime_error("Refusing to replace a non-socket control path");
    }
    if (unlink(state->path.c_str()) != 0) {
      throw socket_error("remove abandoned control socket");
    }
  } else if (errno != ENOENT) {
    throw socket_error("inspect control socket");
  }
  state->listener.value = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (state->listener.value < 0 ||
      bind(state->listener.value, reinterpret_cast<const sockaddr *>(&address),
           sizeof(address)) != 0) {
    throw socket_error("bind control socket");
  }
  state->bound = true;
  if (chmod(state->path.c_str(), 0600) != 0 || listen(state->listener.value, 16) != 0) {
    throw socket_error("listen on control socket");
  }
  state->accepting = accept_clients(state.get());
  return state.release();
}

void destroy_control_server(ControlServerState *state) {
  if (state == nullptr) {
    return;
  }
  (void)state->cancellation.cancel();
  state->accepting = {};
  state->clients.clear();
  if (state->bound) {
    (void)unlink(state->path.c_str());
  }
  delete state;
}

} // namespace elder_terms

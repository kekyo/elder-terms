#include <fcntl.h>
#include <stdlib.h>
#include <sys/select.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

static void set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

static std::string to_hex(const unsigned char *data, ssize_t size) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (ssize_t index = 0; index < size; ++index) {
    output << std::setw(2) << static_cast<int>(data[index]);
  }
  return output.str();
}

static void write_all(int fd, const std::string &text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t written =
        ::write(fd, text.data() + offset, text.size() - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    return;
  }
}

static void handle_command(int master_fd, const std::string &line,
                           bool *running) {
  if (line == "QUIT") {
    *running = false;
    return;
  }

  static constexpr char tx_prefix[] = "TX ";
  if (line.rfind(tx_prefix, 0) == 0) {
    write_all(master_fd, line.substr(std::strlen(tx_prefix)));
  }
}

} // namespace

int main() {
  const int master_fd = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (master_fd < 0 || ::grantpt(master_fd) < 0 ||
      ::unlockpt(master_fd) < 0) {
    std::cerr << "failed to create pty: " << std::strerror(errno) << '\n';
    return 1;
  }

  char *slave_name = ::ptsname(master_fd);
  if (slave_name == nullptr) {
    std::cerr << "failed to resolve pty slave: " << std::strerror(errno)
              << '\n';
    return 1;
  }

  set_nonblocking(STDIN_FILENO);
  set_nonblocking(master_fd);
  std::cout << "READY " << slave_name << '\n';
  std::cout.flush();

  bool running = true;
  std::string input_buffer;
  while (running) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);
    FD_SET(master_fd, &read_fds);
    const int max_fd = master_fd > STDIN_FILENO ? master_fd : STDIN_FILENO;
    const int selected = ::select(max_fd + 1, &read_fds, nullptr, nullptr,
                                  nullptr);
    if (selected < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    if (FD_ISSET(STDIN_FILENO, &read_fds)) {
      std::array<char, 1024> buffer{};
      const ssize_t read_size =
          ::read(STDIN_FILENO, buffer.data(), buffer.size());
      if (read_size == 0) {
        running = false;
      } else if (read_size > 0) {
        input_buffer.append(buffer.data(), static_cast<std::size_t>(read_size));
        std::size_t newline = input_buffer.find('\n');
        while (newline != std::string::npos) {
          const std::string line = input_buffer.substr(0, newline);
          input_buffer.erase(0, newline + 1);
          handle_command(master_fd, line, &running);
          newline = input_buffer.find('\n');
        }
      }
    }

    if (FD_ISSET(master_fd, &read_fds)) {
      std::array<unsigned char, 1024> buffer{};
      const ssize_t read_size =
          ::read(master_fd, buffer.data(), buffer.size());
      if (read_size > 0) {
        std::cout << "RX " << to_hex(buffer.data(), read_size) << '\n';
        std::cout.flush();
      }
    }
  }

  ::close(master_fd);
  return 0;
}

#include "../../src/terminal-transfer-runner.h"

#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gio/gio.h>

namespace elder_terms {

class ScopedEnvironment {
public:
  ScopedEnvironment(const char *name, const std::string &value)
      : name_(name) {
    const char *old_value = g_getenv(name);
    if (old_value != nullptr) {
      old_value_ = old_value;
    }
    g_setenv(name, value.c_str(), TRUE);
  }

  ~ScopedEnvironment() {
    if (old_value_.has_value()) {
      g_setenv(name_.c_str(), old_value_->c_str(), TRUE);
    } else {
      g_unsetenv(name_.c_str());
    }
  }

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

struct ScopedFd {
  int fd = -1;

  ScopedFd() = default;

  explicit ScopedFd(int next_fd) : fd(next_fd) {
  }

  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;

  ScopedFd(ScopedFd &&other) noexcept : fd(other.release()) {
  }

  ScopedFd &operator=(ScopedFd &&other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  ~ScopedFd() {
    reset();
  }

  int get() const {
    return fd;
  }

  int release() {
    return std::exchange(fd, -1);
  }

  void reset(int next_fd = -1) {
    if (fd >= 0) {
      ::close(fd);
    }
    fd = next_fd;
  }
};

struct LrzszChild {
  pid_t pid = -1;
  ScopedFd stderr_fd;
};

struct SocketPair {
  ScopedFd parent_fd;
  ScopedFd child_fd;
};

struct TransferIntegrationCase {
  TerminalTransferProtocol protocol = TerminalTransferProtocol::zmodem;
  TerminalTransferDirection direction = TerminalTransferDirection::send;
  std::uint64_t start_delay_ms = 0;
};

static void expect_equal(const std::string &actual,
                         const std::string &expected,
                         const char *message) {
  if (actual != expected) {
    throw std::runtime_error(std::string(message) + ": expected [" + expected +
                             "], actual [" + actual + "]");
  }
}

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static std::string protocol_name(TerminalTransferProtocol protocol) {
  return terminal_transfer_protocol_token(protocol);
}

static std::string direction_name(TerminalTransferDirection direction) {
  return direction == TerminalTransferDirection::send ? "send" : "receive";
}

static std::string file_uri_for_path(const std::filesystem::path &path) {
  GFile *file = g_file_new_for_path(path.c_str());
  char *uri = g_file_get_uri(file);
  std::string result = uri == nullptr ? std::string() : std::string(uri);
  if (uri != nullptr) {
    g_free(uri);
  }
  g_object_unref(file);
  return result;
}

static std::vector<std::uint8_t> make_transfer_payload() {
  std::vector<std::uint8_t> data(2048);
  for (std::size_t index = 0; index < data.size(); ++index) {
    data[index] =
        static_cast<std::uint8_t>((index * 37U + (index >> 2U) + 17U) & 0xffU);
  }
  data[3] = 0x00;
  data[7] = 0xff;
  data[1024] = 0x18;
  return data;
}

static void write_bytes(const std::filesystem::path &path,
                        std::span<const std::uint8_t> data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("failed to open transfer payload for writing");
  }
  output.write(reinterpret_cast<const char *>(data.data()),
               static_cast<std::streamsize>(data.size()));
  if (!output) {
    throw std::runtime_error("failed to write transfer payload");
  }
}

static std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open transfer result: " +
                             path.string());
  }
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>());
}

static void expect_file_bytes(const std::filesystem::path &path,
                              std::span<const std::uint8_t> expected) {
  const std::vector<std::uint8_t> actual = read_bytes(path);
  if (!std::ranges::equal(actual, expected)) {
    throw std::runtime_error("transfer result mismatch: " + path.string() +
                             " expected " + std::to_string(expected.size()) +
                             " bytes, actual " +
                             std::to_string(actual.size()) + " bytes");
  }
}

static SocketPair make_socket_pair() {
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "socketpair failed");
  }
  return SocketPair{
      .parent_fd = ScopedFd(fds[0]),
      .child_fd = ScopedFd(fds[1]),
  };
}

static std::string read_fd_text(int fd) {
  std::string result;
  char buffer[512];
  ssize_t read_len = 0;
  while ((read_len = ::read(fd, buffer, sizeof(buffer))) > 0) {
    result.append(buffer, static_cast<std::size_t>(read_len));
  }
  return result;
}

static LrzszChild spawn_lrzsz_child(const std::vector<std::string> &arguments,
                                    const std::filesystem::path &directory,
                                    int child_fd) {
  expect_true(!arguments.empty(), "lrzsz command should not be empty");
  expect_true(std::filesystem::exists(arguments[0]),
              "missing lrzsz command: " + arguments[0]);

  int stderr_pipe[2] = {-1, -1};
  if (::pipe(stderr_pipe) != 0) {
    throw std::system_error(errno, std::generic_category(), "pipe failed");
  }

  std::vector<char *> argv;
  argv.reserve(arguments.size() + 1);
  for (const std::string &argument : arguments) {
    argv.push_back(const_cast<char *>(argument.c_str()));
  }
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    const int error = errno;
    ::close(stderr_pipe[0]);
    ::close(stderr_pipe[1]);
    throw std::system_error(error, std::generic_category(), "fork failed");
  }

  if (pid == 0) {
    ::close(stderr_pipe[0]);
    if (::chdir(directory.c_str()) != 0) {
      _exit(126);
    }
    (void)::dup2(child_fd, STDIN_FILENO);
    (void)::dup2(child_fd, STDOUT_FILENO);
    (void)::dup2(stderr_pipe[1], STDERR_FILENO);
    if (child_fd > STDERR_FILENO) {
      ::close(child_fd);
    }
    ::close(stderr_pipe[1]);
    ::execv(argv[0], argv.data());
    _exit(127);
  }

  ::close(stderr_pipe[1]);
  return LrzszChild{
      .pid = pid,
      .stderr_fd = ScopedFd(stderr_pipe[0]),
  };
}

static void wait_child_ok(LrzszChild *child) {
  if (child == nullptr || child->pid < 0) {
    return;
  }

  int status = 0;
  if (::waitpid(child->pid, &status, 0) != child->pid) {
    throw std::system_error(errno, std::generic_category(), "waitpid failed");
  }
  child->pid = -1;

  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    child->stderr_fd.reset();
    return;
  }

  const std::string stderr_text = read_fd_text(child->stderr_fd.get());
  child->stderr_fd.reset();
  throw std::runtime_error("lrzsz child failed: " + stderr_text);
}

static void kill_child_noexcept(LrzszChild *child) {
  if (child != nullptr && child->pid > 0) {
    (void)::kill(child->pid, SIGKILL);
    int status = 0;
    (void)::waitpid(child->pid, &status, 0);
    child->pid = -1;
  }
}

static cardio::promise<void>
wait_fd_ready(int fd, cardio::fd_event event, std::uint32_t timeout_ms,
              cardio::cancellation cancellation) {
  cardio::cancellation_source timeout_source =
      cardio::cancellations::timeout(timeout_ms);
  cardio::cancellation_source combined =
      cardio::cancellations::any(cancellation,
                                 timeout_source.get_cancellation());
  try {
    co_await cardio::from_fd(fd, event, combined.get_cancellation());
  } catch (const cardio::canceled_exception &) {
    if (cancellation.is_cancellation_requested()) {
      throw xyzm_async_cancelled_error("fd transfer cancelled");
    }
    throw xyzm_async_timeout_error("fd transfer timeout");
  }
}

static TerminalTransferTransport make_fd_transport(int fd) {
  return TerminalTransferTransport{
      .send =
          [fd](std::span<const std::uint8_t> bytes,
               std::uint32_t timeout_ms, std::size_t &written_len,
               cardio::cancellation cancellation) -> cardio::promise<void> {
        if (bytes.empty()) {
          written_len = 0;
          co_return;
        }
        co_await wait_fd_ready(fd, cardio::fd_event::write, timeout_ms,
                               cancellation);
#ifdef MSG_NOSIGNAL
        const ssize_t result =
            ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
#else
        const ssize_t result = ::write(fd, bytes.data(), bytes.size());
#endif
        if (result <= 0) {
          throw xyzm_async_io_error("fd transfer send failed");
        }
        written_len = static_cast<std::size_t>(result);
      },
      .recv =
          [fd](std::span<std::uint8_t> bytes, std::uint32_t timeout_ms,
               std::size_t &read_len,
               cardio::cancellation cancellation) -> cardio::promise<void> {
        if (bytes.empty()) {
          read_len = 0;
          co_return;
        }
        co_await wait_fd_ready(fd, cardio::fd_event::read, timeout_ms,
                               cancellation);
        const ssize_t result = ::read(fd, bytes.data(), bytes.size());
        if (result <= 0) {
          throw xyzm_async_io_error("fd transfer receive failed");
        }
        read_len = static_cast<std::size_t>(result);
      },
      .now_ms =
          []() {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
          },
  };
}

static std::vector<std::string>
lrzsz_command(TerminalTransferProtocol protocol,
              TerminalTransferDirection direction,
              const std::string &file_name) {
  if (direction == TerminalTransferDirection::send) {
    if (protocol == TerminalTransferProtocol::zmodem) {
      return {"/usr/bin/rz"};
    }
    if (protocol == TerminalTransferProtocol::ymodem) {
      return {"/usr/bin/rb"};
    }
    return {"/usr/bin/rx", file_name};
  }

  if (protocol == TerminalTransferProtocol::zmodem) {
    return {"/usr/bin/sz", file_name};
  }
  if (protocol == TerminalTransferProtocol::ymodem) {
    return {"/usr/bin/sb", file_name};
  }
  return {"/usr/bin/sx", file_name};
}

static void run_transfer_integration_case(TransferIntegrationCase test_case) {
  const std::string case_name =
      protocol_name(test_case.protocol) + "-" +
      direction_name(test_case.direction) + "-" +
      std::to_string(test_case.start_delay_ms);
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("elder-terms-transfer-integration-" + case_name + "-" +
       std::to_string(static_cast<long long>(g_get_monotonic_time())));
  const std::filesystem::path local_directory = root / "local";
  const std::filesystem::path remote_directory = root / "remote";
  const std::filesystem::path receive_directory = root / "receive";
  const std::vector<std::uint8_t> payload = make_transfer_payload();
  std::optional<LrzszChild> child;

  try {
    std::filesystem::create_directories(local_directory);
    std::filesystem::create_directories(remote_directory);
    std::filesystem::create_directories(receive_directory);

    SocketPair sockets = make_socket_pair();
    TerminalTransferTransport transport =
        make_fd_transport(sockets.parent_fd.get());

    cardio::dispatcher_group_glib dispatcher_group;
    cardio::dispatcher_host_glib dispatcher(dispatcher_group);

    if (test_case.direction == TerminalTransferDirection::send) {
      const std::filesystem::path source_path =
          local_directory / "payload.bin";
      write_bytes(source_path, std::span<const std::uint8_t>(payload.data(),
                                                             payload.size()));
      const std::string remote_name =
          test_case.protocol == TerminalTransferProtocol::xmodem
              ? "received.bin"
              : "payload.bin";
      const std::vector<std::string> command =
          lrzsz_command(test_case.protocol, test_case.direction, remote_name);
      child.emplace(spawn_lrzsz_child(command, remote_directory,
                                      sockets.child_fd.get()));
      sockets.child_fd.reset();

      TerminalTransferRequest request{
          .protocol = test_case.protocol,
          .direction = test_case.direction,
          .base_path = receive_directory.string(),
          .source_file_uris = {file_uri_for_path(source_path)},
          .active = nullptr,
          .status = nullptr,
          .finished = nullptr,
      };
      std::exception_ptr async_error;
      auto root_task = [&]() -> cardio::promise<void> {
        try {
          co_await cardio::promises::delay(test_case.start_delay_ms);
          cardio::cancellation_source timeout =
              cardio::cancellations::timeout(30000);
          co_await run_terminal_transfer_async(
              std::move(request), std::move(transport),
              timeout.get_cancellation());
        } catch (...) {
          async_error = std::current_exception();
        }
        dispatcher_group.shutdown();
      }();
      dispatcher.park();
      root_task.unsafe_result();
      if (async_error) {
        std::rethrow_exception(async_error);
      }
      sockets.parent_fd.reset();
      wait_child_ok(&*child);

      expect_file_bytes(remote_directory / remote_name,
                        std::span<const std::uint8_t>(payload.data(),
                                                      payload.size()));
    } else {
      const std::string remote_name = "remote.bin";
      const std::filesystem::path remote_source =
          remote_directory / remote_name;
      write_bytes(remote_source,
                  std::span<const std::uint8_t>(payload.data(),
                                                payload.size()));

      TerminalTransferRequest request{
          .protocol = test_case.protocol,
          .direction = test_case.direction,
          .base_path = receive_directory.string(),
          .source_file_uris = {},
          .active = nullptr,
          .status = nullptr,
          .finished = nullptr,
      };
      const std::vector<std::string> command =
          lrzsz_command(test_case.protocol, test_case.direction, remote_name);
      std::exception_ptr async_error;
      auto root_task = [&]() -> cardio::promise<void> {
        try {
          cardio::cancellation_source timeout =
              cardio::cancellations::timeout(30000);
          auto transfer = run_terminal_transfer_async(
              std::move(request), std::move(transport),
              timeout.get_cancellation());
          co_await cardio::promises::delay(test_case.start_delay_ms);
          child.emplace(spawn_lrzsz_child(command, remote_directory,
                                          sockets.child_fd.get()));
          sockets.child_fd.reset();
          co_await transfer;
        } catch (...) {
          async_error = std::current_exception();
        }
        dispatcher_group.shutdown();
      }();
      dispatcher.park();
      root_task.unsafe_result();
      if (async_error) {
        std::rethrow_exception(async_error);
      }
      sockets.parent_fd.reset();
      wait_child_ok(&*child);

      const std::string local_name =
          test_case.protocol == TerminalTransferProtocol::xmodem
              ? "received.bin"
              : remote_name;
      expect_file_bytes(receive_directory / local_name,
                        std::span<const std::uint8_t>(payload.data(),
                                                      payload.size()));
    }

    std::filesystem::remove_all(root);
  } catch (...) {
    kill_child_noexcept(child.has_value() ? &*child : nullptr);
    std::filesystem::remove_all(root);
    throw;
  }
}

static void xyzmodem_send_receive_with_lrzsz() {
  const TerminalTransferProtocol protocols[] = {
      TerminalTransferProtocol::xmodem,
      TerminalTransferProtocol::ymodem,
      TerminalTransferProtocol::zmodem,
  };
  const TerminalTransferDirection directions[] = {
      TerminalTransferDirection::send,
      TerminalTransferDirection::receive,
  };
  const std::uint64_t delays[] = {250, 1000, 3000};

  for (TerminalTransferProtocol protocol : protocols) {
    for (TerminalTransferDirection direction : directions) {
      for (std::uint64_t delay : delays) {
        std::cout << "terminal-transfer-runner-test: "
                  << protocol_name(protocol) << " "
                  << direction_name(direction) << " delay=" << delay << "ms"
                  << '\n';
        run_transfer_integration_case(TransferIntegrationCase{
            .protocol = protocol,
            .direction = direction,
            .start_delay_ms = delay,
        });
      }
    }
  }
}

static void sanitize_received_file_names() {
  expect_equal(sanitize_transfer_file_name("/tmp/foobar.tar.gz",
                                           "received.bin"),
               "foobar.tar.gz",
               "sanitize should strip POSIX directory components");
  expect_equal(sanitize_transfer_file_name("C:\\temp\\payload.bin",
                                           "received.bin"),
               "payload.bin",
               "sanitize should strip Windows directory components");
  expect_equal(sanitize_transfer_file_name(std::string("bad") +
                                               static_cast<char>(1) + "name",
                                           "received.bin"),
               "bad_name",
               "sanitize should replace control characters");
  expect_equal(sanitize_transfer_file_name("..", "received.bin"),
               "received.bin",
               "sanitize should reject parent-directory names");
}

static void format_progress_status() {
  expect_equal(format_transfer_status("foobar.tar.gz", 650ULL * 1024ULL,
                                      2560ULL * 1024ULL),
               "foobar.tar.gz 650KiB/2.5MiB (25%)",
               "progress should format known total bytes");
  expect_equal(format_transfer_status("stream.bin", 42, std::nullopt),
               "stream.bin 42B",
               "progress should omit unknown total bytes");
  expect_equal(format_transfer_status("", 0, 0),
               "received.bin 0B/0B (0%)",
               "progress should use the receive fallback name");
}

static void resolve_base_path_as_path_or_uri() {
  expect_equal(resolve_transfer_base_path_uri("/tmp/elder terms"),
               "file:///tmp/elder%20terms",
               "plain paths should resolve as file URIs");
  expect_equal(resolve_transfer_base_path_uri("file:///tmp/elder%20terms"),
               "file:///tmp/elder%20terms",
               "file URIs should remain file URIs");
  expect_equal(resolve_transfer_base_path_uri("sftp://example/home/kouji"),
               "sftp://example/home/kouji",
               "non-file URIs should remain URIs");

  const std::string default_uri = resolve_transfer_base_path_uri("");
  if (default_uri.empty() || default_uri.find("://") == std::string::npos) {
    throw std::runtime_error(
        "empty transfer base path should resolve to a default GIO URI");
  }
}

static void resolve_default_base_path_from_xdg_download_dir() {
  const auto root =
      std::filesystem::temp_directory_path() /
      ("elder-terms-transfer-runner-test-" +
       std::to_string(static_cast<long long>(g_get_monotonic_time())));
  const auto home = root / "home";
  const auto config_home = root / "config";
  const auto downloads = home / "XDG Downloads";

  try {
    std::filesystem::create_directories(downloads);
    std::filesystem::create_directories(config_home);
    {
      std::ofstream user_dirs(config_home / "user-dirs.dirs");
      user_dirs << "XDG_DOWNLOAD_DIR=\"$HOME/XDG Downloads\"\n";
    }

    {
      ScopedEnvironment home_env("HOME", home.string());
      ScopedEnvironment config_env("XDG_CONFIG_HOME", config_home.string());
      g_reload_user_special_dirs_cache();

      expect_equal(resolve_transfer_base_path_uri(""),
                   file_uri_for_path(downloads),
                   "empty transfer base path should use XDG Downloads");
    }

    g_reload_user_special_dirs_cache();
    std::filesystem::remove_all(root);
  } catch (...) {
    g_reload_user_special_dirs_cache();
    std::filesystem::remove_all(root);
    throw;
  }
}

} // namespace elder_terms

int main() {
  try {
    elder_terms::sanitize_received_file_names();
    elder_terms::format_progress_status();
    elder_terms::resolve_base_path_as_path_or_uri();
    elder_terms::resolve_default_base_path_from_xdg_download_dir();
    elder_terms::xyzmodem_send_receive_with_lrzsz();
  } catch (const std::exception &error) {
    std::cerr << "terminal-transfer-runner-test: FAIL: " << error.what()
              << '\n';
    return 1;
  }

  std::cout << "terminal-transfer-runner-test: PASS" << '\n';
  return 0;
}

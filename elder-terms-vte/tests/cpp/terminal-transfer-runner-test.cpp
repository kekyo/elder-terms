#include "../../src/terminal-transfer-runner.h"

#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
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

enum class TransferProtocolOptionCase {
  standard,
  xmodem_128,
  xmodem_1k,
  xmodem_checksum,
  xmodem_crc,
  ymodem_g,
  zmodem_resume,
};

struct TransferIntegrationCase {
  TerminalTransferProtocol protocol = TerminalTransferProtocol::zmodem;
  TerminalTransferDirection direction = TerminalTransferDirection::send;
  std::uint64_t start_delay_ms = 0;
  TransferProtocolOptionCase option_case =
      TransferProtocolOptionCase::standard;
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

static std::string option_case_name(TransferProtocolOptionCase option_case) {
  if (option_case == TransferProtocolOptionCase::xmodem_128) {
    return "xmodem-128";
  }
  if (option_case == TransferProtocolOptionCase::xmodem_1k) {
    return "xmodem-1k";
  }
  if (option_case == TransferProtocolOptionCase::xmodem_checksum) {
    return "xmodem-checksum";
  }
  if (option_case == TransferProtocolOptionCase::xmodem_crc) {
    return "xmodem-crc";
  }
  if (option_case == TransferProtocolOptionCase::ymodem_g) {
    return "ymodem-g";
  }
  if (option_case == TransferProtocolOptionCase::zmodem_resume) {
    return "zmodem-resume";
  }
  return "standard";
}

static std::optional<TerminalTransferProtocol>
parse_protocol(const std::string &value) {
  if (value == "xmodem") {
    return TerminalTransferProtocol::xmodem;
  }
  if (value == "ymodem") {
    return TerminalTransferProtocol::ymodem;
  }
  if (value == "zmodem") {
    return TerminalTransferProtocol::zmodem;
  }
  return std::nullopt;
}

static std::optional<TerminalTransferDirection>
parse_direction(const std::string &value) {
  if (value == "send") {
    return TerminalTransferDirection::send;
  }
  if (value == "receive") {
    return TerminalTransferDirection::receive;
  }
  return std::nullopt;
}

static std::optional<std::uint64_t> parse_delay_ms(const std::string &value) {
  std::uint64_t result = 0;
  const auto parse_result =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parse_result.ec != std::errc() ||
      parse_result.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return result;
}

static std::optional<TransferProtocolOptionCase>
parse_option_case(const std::string &value) {
  if (value == "standard") {
    return TransferProtocolOptionCase::standard;
  }
  if (value == "xmodem-128") {
    return TransferProtocolOptionCase::xmodem_128;
  }
  if (value == "xmodem-1k") {
    return TransferProtocolOptionCase::xmodem_1k;
  }
  if (value == "xmodem-checksum") {
    return TransferProtocolOptionCase::xmodem_checksum;
  }
  if (value == "xmodem-crc") {
    return TransferProtocolOptionCase::xmodem_crc;
  }
  if (value == "ymodem-g") {
    return TransferProtocolOptionCase::ymodem_g;
  }
  if (value == "zmodem-resume") {
    return TransferProtocolOptionCase::zmodem_resume;
  }
  return std::nullopt;
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

static constexpr std::size_t zmodem_resume_offset = 1024;

static bool is_zmodem_resume_case(const TransferIntegrationCase &test_case) {
  return test_case.protocol == TerminalTransferProtocol::zmodem &&
         test_case.option_case == TransferProtocolOptionCase::zmodem_resume;
}

static std::vector<std::uint8_t>
corrupt_prefix(std::span<const std::uint8_t> payload, std::size_t prefix_size) {
  std::vector<std::uint8_t> corrupted(payload.begin(), payload.end());
  const std::size_t limit = std::min(prefix_size, corrupted.size());
  for (std::size_t index = 0; index < limit; ++index) {
    corrupted[index] ^= 0xffU;
  }
  return corrupted;
}

static void
expect_status_contains(const std::vector<std::string> &status_updates,
                       const std::string &needle,
                       const std::string &message) {
  if (std::ranges::any_of(status_updates, [&needle](const std::string &status) {
        return status.find(needle) != std::string::npos;
      })) {
    return;
  }

  std::string joined;
  for (const std::string &status : status_updates) {
    if (!joined.empty()) {
      joined += " | ";
    }
    joined += status;
  }
  throw std::runtime_error(message + ": missing [" + needle + "] in [" +
                           joined + "]");
}

static std::string
format_progress_updates(const std::vector<TerminalTransferProgress> &updates) {
  std::string joined;
  for (const TerminalTransferProgress &progress : updates) {
    if (!joined.empty()) {
      joined += " | ";
    }
    joined += progress.mode == TerminalTransferProgressMode::determinate
                  ? "determinate"
                  : "indeterminate";
    if (progress.fraction.has_value()) {
      joined += ":";
      joined += std::to_string(*progress.fraction);
    }
  }
  return joined;
}

static void expect_progress_updates_for_protocol(
    const std::vector<TerminalTransferProgress> &updates,
    TerminalTransferProtocol protocol) {
  const bool has_indeterminate =
      std::ranges::any_of(updates, [](const TerminalTransferProgress &progress) {
        return progress.mode == TerminalTransferProgressMode::indeterminate;
      });
  expect_true(has_indeterminate,
              "transfer should publish an initial indeterminate progress state");

  const bool has_determinate =
      std::ranges::any_of(updates, [](const TerminalTransferProgress &progress) {
        return progress.mode == TerminalTransferProgressMode::determinate &&
               progress.fraction.has_value() && *progress.fraction >= 0.0 &&
               *progress.fraction <= 1.0;
      });
  if (protocol == TerminalTransferProtocol::xmodem) {
    if (has_determinate) {
      throw std::runtime_error(
          "XMODEM should not publish determinate progress: " +
          format_progress_updates(updates));
    }
    return;
  }
  if (!has_determinate) {
    throw std::runtime_error(
        "Y/ZMODEM should publish determinate progress: " +
        format_progress_updates(updates));
  }
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

static std::string shell_quote(const std::string &value) {
  std::string result = "'";
  for (char character : value) {
    if (character == '\'') {
      result += "'\\''";
    } else {
      result += character;
    }
  }
  result += "'";
  return result;
}

static std::vector<std::string>
lrzsz_command(const TransferIntegrationCase &test_case,
              const std::string &file_name) {
  const TerminalTransferProtocol protocol = test_case.protocol;
  const TerminalTransferDirection direction = test_case.direction;
  if (direction == TerminalTransferDirection::send) {
    if (protocol == TerminalTransferProtocol::zmodem) {
      if (test_case.option_case == TransferProtocolOptionCase::zmodem_resume) {
        return {"/usr/bin/rz", "--resume"};
      }
      return {"/usr/bin/rz"};
    }
    if (protocol == TerminalTransferProtocol::ymodem) {
      return {"/usr/bin/rb"};
    }
    return {"/usr/bin/rx", file_name};
  }

  if (protocol == TerminalTransferProtocol::zmodem) {
    if (test_case.option_case == TransferProtocolOptionCase::zmodem_resume) {
      return {"/usr/bin/sz", "--resume", file_name};
    }
    return {"/usr/bin/sz", file_name};
  }
  if (protocol == TerminalTransferProtocol::ymodem) {
    if (test_case.option_case == TransferProtocolOptionCase::ymodem_g) {
      return {"/bin/sh", "-c",
              "/usr/bin/sb --ymodem -b -q " + shell_quote(file_name) +
                  "; sleep 1"};
    }
    return {"/usr/bin/sb", "--ymodem", "-b", "-q", file_name};
  }
  return {"/usr/bin/sx", file_name};
}

static TerminalTransferOptions
transfer_options_for_case(const TransferIntegrationCase &test_case) {
  TerminalTransferOptions options;
  if (test_case.protocol == TerminalTransferProtocol::xmodem &&
      test_case.direction == TerminalTransferDirection::send) {
    if (test_case.option_case == TransferProtocolOptionCase::xmodem_128) {
      options.xmodem_packet_size =
          TerminalTransferXmodemPacketSize::bytes_128;
    } else if (test_case.option_case ==
               TransferProtocolOptionCase::xmodem_1k) {
      options.xmodem_packet_size =
          TerminalTransferXmodemPacketSize::bytes_1024;
    }
    options.xmodem_checksum_mode =
        TerminalTransferXmodemChecksumMode::automatic;
    return options;
  }

  if (test_case.protocol == TerminalTransferProtocol::xmodem &&
      test_case.direction == TerminalTransferDirection::receive) {
    if (test_case.option_case == TransferProtocolOptionCase::xmodem_checksum) {
      options.xmodem_checksum_mode =
          TerminalTransferXmodemChecksumMode::checksum;
    } else if (test_case.option_case ==
               TransferProtocolOptionCase::xmodem_crc) {
      options.xmodem_checksum_mode = TerminalTransferXmodemChecksumMode::crc;
    }
    return options;
  }

  if (test_case.protocol == TerminalTransferProtocol::ymodem &&
      test_case.direction == TerminalTransferDirection::receive &&
      test_case.option_case == TransferProtocolOptionCase::ymodem_g) {
    options.ymodem_variant = TerminalTransferYmodemVariant::g;
  }
  return options;
}

static void run_transfer_integration_case(TransferIntegrationCase test_case) {
  const std::string case_name =
      protocol_name(test_case.protocol) + "-" +
      direction_name(test_case.direction) + "-" +
      option_case_name(test_case.option_case) + "-" +
      std::to_string(test_case.start_delay_ms);
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("elder-terms-transfer-integration-" + case_name + "-" +
       std::to_string(static_cast<long long>(::getpid())) + "-" +
       std::to_string(static_cast<long long>(g_get_monotonic_time())));
  const std::filesystem::path local_directory = root / "local";
  const std::filesystem::path remote_directory = root / "remote";
  const std::filesystem::path receive_directory = root / "receive";
  const std::vector<std::uint8_t> payload = make_transfer_payload();
  std::vector<std::string> status_updates;
  std::vector<TerminalTransferProgress> progress_updates;
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
      if (is_zmodem_resume_case(test_case)) {
        write_bytes(
            remote_directory / remote_name,
            std::span<const std::uint8_t>(payload.data(), zmodem_resume_offset));
      }
      const std::vector<std::string> command =
          lrzsz_command(test_case, remote_name);
      child.emplace(spawn_lrzsz_child(command, remote_directory,
                                      sockets.child_fd.get()));
      sockets.child_fd.reset();

      TerminalTransferRequest request{
          .protocol = test_case.protocol,
          .direction = test_case.direction,
          .base_path = receive_directory.string(),
          .source_file_uris = {file_uri_for_path(source_path)},
          .options = transfer_options_for_case(test_case),
          .active = nullptr,
          .status =
              [&status_updates](const std::string &status) {
                status_updates.push_back(status);
              },
          .progress =
              [&progress_updates](TerminalTransferProgress progress) {
                progress_updates.push_back(progress);
              },
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
      if (is_zmodem_resume_case(test_case)) {
        expect_status_contains(status_updates, "1KiB/2KiB (50%)",
                               "ZMODEM send resume should publish the resume "
                               "offset");
      }
    } else {
      const std::string remote_name = "remote.bin";
      const std::filesystem::path remote_source =
          remote_directory / remote_name;
      const bool spawn_child_before_receive =
          test_case.protocol == TerminalTransferProtocol::ymodem &&
          test_case.option_case == TransferProtocolOptionCase::ymodem_g;
      if (is_zmodem_resume_case(test_case)) {
        const std::vector<std::uint8_t> corrupted =
            corrupt_prefix(std::span<const std::uint8_t>(payload.data(),
                                                         payload.size()),
                           zmodem_resume_offset);
        write_bytes(remote_source,
                    std::span<const std::uint8_t>(corrupted.data(),
                                                  corrupted.size()));
        write_bytes(
            receive_directory / (remote_name + ".partial"),
            std::span<const std::uint8_t>(payload.data(), zmodem_resume_offset));
      } else {
        write_bytes(remote_source,
                    std::span<const std::uint8_t>(payload.data(),
                                                  payload.size()));
      }

      TerminalTransferRequest request{
          .protocol = test_case.protocol,
          .direction = test_case.direction,
          .base_path = receive_directory.string(),
          .source_file_uris = {},
          .options = transfer_options_for_case(test_case),
          .active = nullptr,
          .status =
              [&status_updates](const std::string &status) {
                status_updates.push_back(status);
              },
          .progress =
              [&progress_updates](TerminalTransferProgress progress) {
                progress_updates.push_back(progress);
              },
          .finished = nullptr,
      };
      const std::vector<std::string> command =
          lrzsz_command(test_case, remote_name);
      if (spawn_child_before_receive) {
        child.emplace(spawn_lrzsz_child(command, remote_directory,
                                        sockets.child_fd.get()));
        sockets.child_fd.reset();
      }
      std::exception_ptr async_error;
      auto root_task = [&]() -> cardio::promise<void> {
        try {
          cardio::cancellation_source timeout =
              cardio::cancellations::timeout(30000);
          if (spawn_child_before_receive) {
            co_await cardio::promises::delay(test_case.start_delay_ms);
          }
          auto transfer = run_terminal_transfer_async(
              std::move(request), std::move(transport),
              timeout.get_cancellation());
          if (!spawn_child_before_receive) {
            co_await cardio::promises::delay(test_case.start_delay_ms);
            child.emplace(spawn_lrzsz_child(command, remote_directory,
                                            sockets.child_fd.get()));
            sockets.child_fd.reset();
          }
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
      if (is_zmodem_resume_case(test_case)) {
        expect_true(!std::filesystem::exists(receive_directory /
                                             (local_name + ".partial")),
                    "completed ZMODEM resume receive should remove the "
                    "partial file");
	        expect_status_contains(status_updates, "1KiB/2KiB (50%)",
	                               "ZMODEM receive resume should publish the "
	                               "resume offset");
	      }
	    }

    expect_progress_updates_for_protocol(progress_updates, test_case.protocol);
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
                                      2560ULL * 1024ULL, std::nullopt),
               "foobar.tar.gz 650KiB/2.5MiB (25%)",
               "progress should format known total bytes");
  expect_equal(format_transfer_status("stream.bin", 42, std::nullopt,
                                      std::nullopt),
               "stream.bin 42B",
               "progress should omit unknown total bytes");
  expect_equal(format_transfer_status("", 0, 0, std::nullopt),
               "received.bin 0B/0B (0%)",
               "progress should use the receive fallback name");
  expect_equal(format_transfer_status("foobar.tar.gz", 650ULL * 1024ULL,
                                      2560ULL * 1024ULL, 90),
               "foobar.tar.gz 650KiB/2.5MiB (25%, ETA 01:30)",
               "progress should format ETA inside the percent group");
  expect_equal(format_transfer_status("done.bin", 2ULL * 1024ULL,
                                      2ULL * 1024ULL, 0),
               "done.bin 2KiB/2KiB (100%, ETA 00:00)",
               "progress should format completed ETA");
  expect_equal(format_transfer_status("long.bin", 1ULL, 2ULL, 75ULL * 60ULL + 3),
               "long.bin 1B/2B (50%, ETA 75:03)",
               "progress should keep ETA minutes beyond one hour");
}

static void estimate_progress_eta() {
  expect_true(!estimate_transfer_eta_seconds(0, 0, 1000, 1000).has_value(),
              "ETA should be omitted until bytes advance");
  expect_true(!estimate_transfer_eta_seconds(0, 100, 1000, 999).has_value(),
              "ETA should be omitted before the minimum elapsed time");
  expect_true(!estimate_transfer_eta_seconds(0, 100, 0, 1000).has_value(),
              "ETA should be omitted for zero-sized totals");
  expect_true(estimate_transfer_eta_seconds(0, 100, 1000, 1000) ==
                  std::optional<std::uint64_t>(9),
              "ETA should use cumulative average speed");
  expect_true(estimate_transfer_eta_seconds(0, 300, 1000, 1000) ==
                  std::optional<std::uint64_t>(3),
              "ETA should round remaining seconds up");
  expect_true(estimate_transfer_eta_seconds(0, 1000, 1000, 0) ==
                  std::optional<std::uint64_t>(0),
              "ETA should be zero after completion");
  expect_true(estimate_transfer_eta_seconds(1024, 1536, 2048, 1000) ==
                  std::optional<std::uint64_t>(1),
              "ETA should calculate from the resume baseline");
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
       std::to_string(static_cast<long long>(::getpid())) + "-" +
       std::to_string(static_cast<long long>(g_get_monotonic_time())));
  const auto home = std::filesystem::path(g_get_home_dir());
  const auto config_home = root / "config";
  const auto downloads = root / "XDG Downloads";

  try {
    std::filesystem::create_directories(downloads);
    std::filesystem::create_directories(config_home);

    {
      ScopedEnvironment config_env("XDG_CONFIG_HOME", config_home.string());
      g_reload_user_special_dirs_cache();

      expect_equal(resolve_transfer_base_path_uri(""),
                   file_uri_for_path(home / "Downloads"),
                   "missing XDG Downloads should fall back to HOME/Downloads");

      {
        std::ofstream user_dirs(config_home / "user-dirs.dirs");
        user_dirs << "XDG_DOWNLOAD_DIR=\"" << downloads.string() << "\"\n";
      }
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

static void transfer_request_options_map_to_libxyzm() {
  TerminalTransferRequest request;

  const xyzm_xmodem_send_opts_t default_xmodem_send =
      terminal_transfer_xmodem_send_options(request);
  expect_true(default_xmodem_send.checksum_mode ==
                  XYZM_XMODEM_CHECKSUM_MODE_AUTO,
              "XMODEM send default should use peer checksum negotiation");

  const xyzm_xmodem_receive_opts_t default_xmodem_receive =
      terminal_transfer_xmodem_receive_options(request);
  expect_true(default_xmodem_receive.checksum_mode ==
                  XYZM_XMODEM_CHECKSUM_MODE_CRC,
              "XMODEM receive default should request CRC");

  const xyzm_ymodem_opts_t default_ymodem_send =
      terminal_transfer_ymodem_send_options(request);
  expect_true(default_ymodem_send.variant == XYZM_YMODEM_VARIANT_AUTO,
              "YMODEM send default should follow receiver requests");

  const xyzm_ymodem_opts_t default_ymodem_receive =
      terminal_transfer_ymodem_receive_options(request);
  expect_true(default_ymodem_receive.variant == XYZM_YMODEM_VARIANT_STANDARD,
              "YMODEM receive default should use standard mode");

  request.options = TerminalTransferOptions{
      .xmodem_packet_size = TerminalTransferXmodemPacketSize::bytes_128,
      .xmodem_checksum_mode = TerminalTransferXmodemChecksumMode::checksum,
      .ymodem_variant = TerminalTransferYmodemVariant::g,
  };

  const xyzm_xmodem_send_opts_t xmodem_send =
      terminal_transfer_xmodem_send_options(request);
  expect_true(xmodem_send.packet_size == XYZM_XMODEM_PACKET_SIZE_128,
              "XMODEM send should use the selected 128B packet size");
  expect_true(xmodem_send.checksum_mode ==
                  XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM,
              "XMODEM send should use the selected checksum mode");

  const xyzm_xmodem_receive_opts_t xmodem_receive =
      terminal_transfer_xmodem_receive_options(request);
  expect_true(xmodem_receive.checksum_mode ==
                  XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM,
              "XMODEM receive should use the selected checksum mode");

  const xyzm_ymodem_opts_t ymodem_send =
      terminal_transfer_ymodem_send_options(request);
  expect_true(ymodem_send.variant == XYZM_YMODEM_VARIANT_G,
              "YMODEM send should use the selected g variant");

  const xyzm_ymodem_opts_t ymodem_receive =
      terminal_transfer_ymodem_receive_options(request);
  expect_true(ymodem_receive.variant == XYZM_YMODEM_VARIANT_G,
              "YMODEM receive should use the selected g variant");

  request.options = TerminalTransferOptions{
      .xmodem_packet_size = TerminalTransferXmodemPacketSize::bytes_1024,
      .xmodem_checksum_mode = TerminalTransferXmodemChecksumMode::crc,
      .ymodem_variant = TerminalTransferYmodemVariant::standard,
  };

  const xyzm_xmodem_send_opts_t xmodem_send_crc =
      terminal_transfer_xmodem_send_options(request);
  expect_true(xmodem_send_crc.packet_size == XYZM_XMODEM_PACKET_SIZE_1K,
              "XMODEM send should use the selected 1K packet size");
  expect_true(xmodem_send_crc.checksum_mode == XYZM_XMODEM_CHECKSUM_MODE_CRC,
              "XMODEM send should use the selected CRC mode");

  const xyzm_xmodem_receive_opts_t xmodem_receive_crc =
      terminal_transfer_xmodem_receive_options(request);
  expect_true(xmodem_receive_crc.checksum_mode ==
                  XYZM_XMODEM_CHECKSUM_MODE_CRC,
              "XMODEM receive should use the selected CRC mode");

  const xyzm_ymodem_opts_t ymodem_send_standard =
      terminal_transfer_ymodem_send_options(request);
  expect_true(ymodem_send_standard.variant == XYZM_YMODEM_VARIANT_STANDARD,
              "YMODEM send should use the selected standard variant");

  const xyzm_ymodem_opts_t ymodem_receive_standard =
      terminal_transfer_ymodem_receive_options(request);
  expect_true(ymodem_receive_standard.variant == XYZM_YMODEM_VARIANT_STANDARD,
              "YMODEM receive should use the selected standard variant");
}

static void run_unit_cases() {
  sanitize_received_file_names();
  format_progress_status();
  estimate_progress_eta();
  resolve_default_base_path_from_xdg_download_dir();
  resolve_base_path_as_path_or_uri();
  transfer_request_options_map_to_libxyzm();
}

static TransferIntegrationCase parse_transfer_case_arguments(int argc,
                                                             char **argv) {
  if (argc != 5 && argc != 6) {
    throw std::runtime_error(
        "--transfer-case requires protocol, direction, delay, and optional "
        "option-case arguments");
  }

  const std::optional<TerminalTransferProtocol> protocol =
      parse_protocol(argv[2]);
  if (!protocol.has_value()) {
    throw std::runtime_error("unknown transfer protocol: " +
                             std::string(argv[2]));
  }

  const std::optional<TerminalTransferDirection> direction =
      parse_direction(argv[3]);
  if (!direction.has_value()) {
    throw std::runtime_error("unknown transfer direction: " +
                             std::string(argv[3]));
  }

  const std::optional<std::uint64_t> delay = parse_delay_ms(argv[4]);
  if (!delay.has_value()) {
    throw std::runtime_error("invalid transfer delay: " + std::string(argv[4]));
  }

  TransferProtocolOptionCase option_case =
      TransferProtocolOptionCase::standard;
  if (argc == 6) {
    const std::optional<TransferProtocolOptionCase> parsed_option_case =
        parse_option_case(argv[5]);
    if (!parsed_option_case.has_value()) {
      throw std::runtime_error("unknown transfer option case: " +
                               std::string(argv[5]));
    }
    option_case = *parsed_option_case;
  }

  return TransferIntegrationCase{
      .protocol = *protocol,
      .direction = *direction,
      .start_delay_ms = *delay,
      .option_case = option_case,
  };
}

static void run_from_arguments(int argc, char **argv) {
  if (argc == 1) {
    run_unit_cases();
    xyzmodem_send_receive_with_lrzsz();
    return;
  }

  const std::string command = argv[1];
  if (command == "--unit") {
    if (argc != 2) {
      throw std::runtime_error("--unit does not accept arguments");
    }
    run_unit_cases();
    return;
  }

  if (command == "--transfer-case") {
    run_transfer_integration_case(parse_transfer_case_arguments(argc, argv));
    return;
  }

  throw std::runtime_error("unknown argument: " + command);
}

} // namespace elder_terms

int main(int argc, char **argv) {
  try {
    elder_terms::run_from_arguments(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << "terminal-transfer-runner-test: FAIL: " << error.what()
              << '\n';
    return 1;
  }

  std::cout << "terminal-transfer-runner-test: PASS" << '\n';
  return 0;
}

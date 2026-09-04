#include "ftp-client.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <glib.h>

#include "../terminal-sessions/tcp-connector.h"
#include "ftp-protocol.h"

namespace elder_terms {

static constexpr std::size_t maximum_listing_size = 64U * 1024U * 1024U;
static constexpr std::size_t maximum_listing_line_size = 64U * 1024U;

class FtpStatusError final : public std::runtime_error {
public:
  int code;

  FtpStatusError(int code, std::string message)
      : std::runtime_error(std::move(message)), code(code) {
  }
};

struct FtpAcceptedSocket {
  int fd = -1;
  sockaddr_storage peer{};
  socklen_t peer_length = 0;
};

struct FtpAcceptStorage {
  sockaddr_storage peer{};
  socklen_t peer_length = sizeof(peer);
};

struct FtpDataEndpoint {
  int connected_fd = -1;
  int listener_fd = -1;
};

static void close_socket(int *fd) noexcept {
  if (fd != nullptr && *fd >= 0) {
    (void)::close(*fd);
    *fd = -1;
  }
}

static std::string ftp_reply_summary(const FtpReply &reply) {
  if (reply.lines.empty()) {
    return "FTP status " + std::to_string(reply.code);
  }
  return reply.lines.back();
}

static FtpStatusError ftp_status_error(const std::string &operation,
                                       const FtpReply &reply) {
  return FtpStatusError(
      reply.code, operation + " failed: " + ftp_reply_summary(reply));
}

static bool reply_is_positive_completion(const FtpReply &reply) {
  return reply.code >= 200 && reply.code < 300;
}

static bool reply_is_positive_preliminary(const FtpReply &reply) {
  return reply.code >= 100 && reply.code < 200;
}

static void require_positive_completion(const FtpReply &reply,
                                        const std::string &operation) {
  if (!reply_is_positive_completion(reply)) {
    throw ftp_status_error(operation, reply);
  }
}

static bool extension_is_unavailable(const FtpReply &reply) {
  return reply.code == 500 || reply.code == 501 || reply.code == 502 ||
         reply.code == 504 || reply.code == 522;
}

static bool socket_addresses_have_same_host(const sockaddr_storage &left,
                                            const sockaddr_storage &right) {
  if (left.ss_family != right.ss_family) {
    return false;
  }
  if (left.ss_family == AF_INET) {
    const auto *left_ipv4 = reinterpret_cast<const sockaddr_in *>(&left);
    const auto *right_ipv4 = reinterpret_cast<const sockaddr_in *>(&right);
    return std::memcmp(&left_ipv4->sin_addr, &right_ipv4->sin_addr,
                       sizeof(left_ipv4->sin_addr)) == 0;
  }
  if (left.ss_family == AF_INET6) {
    const auto *left_ipv6 = reinterpret_cast<const sockaddr_in6 *>(&left);
    const auto *right_ipv6 = reinterpret_cast<const sockaddr_in6 *>(&right);
    return std::memcmp(&left_ipv6->sin6_addr, &right_ipv6->sin6_addr,
                       sizeof(left_ipv6->sin6_addr)) == 0;
  }
  return false;
}

static void set_socket_port(sockaddr_storage *address, std::uint16_t port) {
  if (address->ss_family == AF_INET) {
    reinterpret_cast<sockaddr_in *>(address)->sin_port = htons(port);
    return;
  }
  if (address->ss_family == AF_INET6) {
    reinterpret_cast<sockaddr_in6 *>(address)->sin6_port = htons(port);
    return;
  }
  throw std::runtime_error("FTP control connection has an unsupported family");
}

static std::string numeric_socket_host(const sockaddr_storage &address) {
  std::array<char, INET6_ADDRSTRLEN> buffer{};
  const void *bytes = nullptr;
  if (address.ss_family == AF_INET) {
    bytes = &reinterpret_cast<const sockaddr_in *>(&address)->sin_addr;
  } else if (address.ss_family == AF_INET6) {
    bytes = &reinterpret_cast<const sockaddr_in6 *>(&address)->sin6_addr;
  } else {
    throw std::runtime_error("FTP socket has an unsupported address family");
  }
  if (::inet_ntop(address.ss_family, bytes, buffer.data(), buffer.size()) ==
      nullptr) {
    throw std::system_error(errno, std::generic_category(),
                            "FTP address formatting failed");
  }
  return buffer.data();
}

static std::uint16_t socket_port(const sockaddr_storage &address) {
  if (address.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<const sockaddr_in *>(&address)->sin_port);
  }
  if (address.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<const sockaddr_in6 *>(&address)->sin6_port);
  }
  throw std::runtime_error("FTP socket has an unsupported address family");
}

static std::string ftp_path_name(std::string path) {
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  const std::size_t separator = path.find_last_of('/');
  return separator == std::string::npos ? path : path.substr(separator + 1);
}

static std::string ftp_child_path(const std::string &directory,
                                  const std::string &name) {
  if (name.empty() || name == "." || name == ".." ||
      name.find('/') != std::string::npos ||
      !ftp_command_argument_is_safe(name) ||
      !g_utf8_validate(name.data(), static_cast<gssize>(name.size()), nullptr)) {
    throw std::runtime_error(
        "FTP server returned an invalid directory entry name");
  }
  if (directory == "/") {
    return "/" + name;
  }
  if (directory.empty() || directory == ".") {
    return directory.empty() ? name : "./" + name;
  }
  return directory.back() == '/' ? directory + name
                                 : directory + "/" + name;
}

static void validate_ftp_argument(const std::string &value,
                                  const char *description,
                                  bool allow_empty) {
  if ((!allow_empty && value.empty()) ||
      !ftp_command_argument_is_safe(value)) {
    throw std::invalid_argument(std::string("Invalid FTP ") + description);
  }
}

static RemoteFileType portable_file_type(FtpDirectoryEntryType type) {
  switch (type) {
  case FtpDirectoryEntryType::regular:
    return RemoteFileType::regular;
  case FtpDirectoryEntryType::directory:
    return RemoteFileType::directory;
  case FtpDirectoryEntryType::current_directory:
  case FtpDirectoryEntryType::parent_directory:
  case FtpDirectoryEntryType::other:
    return RemoteFileType::other;
  }
  return RemoteFileType::other;
}

static RemoteFileAttributes
portable_attributes(const FtpDirectoryEntry &entry, std::string path,
                    std::string name) {
  return {
      .name = std::move(name),
      .path = std::move(path),
      .type = portable_file_type(entry.type),
      .size = entry.size,
      .permissions = std::nullopt,
      .access_time_unix_seconds = std::nullopt,
      .modification_time_unix_seconds =
          entry.modification_time_unix_seconds,
  };
}

class FtpSessionState final {
private:
  cardio::promise<int> open_socket_async(
      int family, cardio::cancellation cancellation) {
    return io.submit<int>(
        [family](::io_uring_sqe *sqe) {
          ::io_uring_prep_socket(
              sqe, family, SOCK_STREAM | SOCK_CLOEXEC, 0, 0);
        },
        [](cardio::io_uring_completion completion) {
          if (completion.result < 0) {
            throw std::system_error(-completion.result,
                                    std::generic_category(),
                                    "FTP socket creation failed");
          }
          return completion.result;
        },
        std::move(cancellation));
  }

  cardio::promise<void> connect_socket_async(
      int fd, sockaddr_storage address, socklen_t length,
      cardio::cancellation cancellation) {
    auto holder = std::make_shared<sockaddr_storage>(address);
    co_await io.submit<void>(
        [fd, holder, length](::io_uring_sqe *sqe) {
          ::io_uring_prep_connect(
              sqe, fd,
              reinterpret_cast<const sockaddr *>(holder.get()), length);
        },
        [holder](cardio::io_uring_completion completion) {
          (void)holder;
          if (completion.result < 0) {
            throw std::system_error(-completion.result,
                                    std::generic_category(),
                                    "FTP data connection failed");
          }
        },
        std::move(cancellation));
  }

  cardio::promise<int> connect_data_socket_async(
      std::uint16_t port, cardio::cancellation cancellation) {
    sockaddr_storage endpoint = control_peer;
    set_socket_port(&endpoint, port);
    int fd = co_await open_socket_async(endpoint.ss_family, cancellation);
    try {
      co_await connect_socket_async(
          fd, endpoint, control_peer_length, cancellation);
      co_return fd;
    } catch (...) {
      close_socket(&fd);
      throw;
    }
  }

  cardio::promise<FtpDataEndpoint> prepare_passive_endpoint_async(
      cardio::cancellation cancellation) {
    const FtpReply epsv = co_await command_async("EPSV", cancellation);
    if (epsv.code == 229) {
      const std::uint16_t port = parse_ftp_epsv_port(epsv);
      co_return FtpDataEndpoint{
          .connected_fd =
              co_await connect_data_socket_async(port, cancellation),
          .listener_fd = -1,
      };
    }
    if (control_peer.ss_family != AF_INET ||
        !extension_is_unavailable(epsv)) {
      throw ftp_status_error("FTP EPSV", epsv);
    }

    const FtpReply pasv = co_await command_async("PASV", cancellation);
    if (pasv.code != 227) {
      throw ftp_status_error("FTP PASV", pasv);
    }
    const FtpPassiveEndpoint advertised = parse_ftp_pasv_endpoint(pasv);
    // RFC 2577 describes PASV address substitution attacks. The control peer
    // is authoritative; only the advertised port is used.
    co_return FtpDataEndpoint{
        .connected_fd =
            co_await connect_data_socket_async(advertised.port, cancellation),
        .listener_fd = -1,
    };
  }

  cardio::promise<int> create_active_listener_async(
      cardio::cancellation cancellation) {
    int fd = co_await open_socket_async(control_local.ss_family, cancellation);
    try {
      const int enabled = 1;
      if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                       sizeof(enabled)) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "FTP active setsockopt failed");
      }
      sockaddr_storage listener_address = control_local;
      set_socket_port(&listener_address, 0);
      if (::bind(fd,
                 reinterpret_cast<const sockaddr *>(&listener_address),
                 control_local_length) != 0 ||
          ::listen(fd, 1) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "FTP active listener failed");
      }
      co_return fd;
    } catch (...) {
      close_socket(&fd);
      throw;
    }
  }

  cardio::promise<FtpDataEndpoint> prepare_active_endpoint_async(
      cardio::cancellation cancellation) {
    int listener_fd = co_await create_active_listener_async(cancellation);
    try {
      sockaddr_storage address{};
      socklen_t length = sizeof(address);
      if (::getsockname(listener_fd, reinterpret_cast<sockaddr *>(&address),
                        &length) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "FTP active getsockname failed");
      }
      const std::string host = numeric_socket_host(address);
      const std::uint16_t port = socket_port(address);
      const int protocol = address.ss_family == AF_INET ? 1 : 2;
      const FtpReply eprt = co_await command_async(
          "EPRT |" + std::to_string(protocol) + "|" + host + "|" +
              std::to_string(port) + "|",
          cancellation);
      if (!reply_is_positive_completion(eprt)) {
        if (address.ss_family != AF_INET ||
            !extension_is_unavailable(eprt)) {
          throw ftp_status_error("FTP EPRT", eprt);
        }
        const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(&address);
        const auto *octets = reinterpret_cast<const unsigned char *>(
            &ipv4->sin_addr.s_addr);
        const std::string port_command =
            "PORT " + std::to_string(octets[0]) + "," +
            std::to_string(octets[1]) + "," +
            std::to_string(octets[2]) + "," +
            std::to_string(octets[3]) + "," +
            std::to_string(port / 256U) + "," +
            std::to_string(port % 256U);
        const FtpReply port_reply =
            co_await command_async(port_command, cancellation);
        require_positive_completion(port_reply, "FTP PORT");
      }
      co_return FtpDataEndpoint{
          .connected_fd = -1,
          .listener_fd = listener_fd,
      };
    } catch (...) {
      close_socket(&listener_fd);
      throw;
    }
  }

  cardio::promise<FtpAcceptedSocket> accept_socket_async(
      int listener_fd, cardio::cancellation cancellation) {
    auto holder = std::make_shared<FtpAcceptStorage>();
    co_return co_await io.submit<FtpAcceptedSocket>(
        [listener_fd, holder](::io_uring_sqe *sqe) {
          ::io_uring_prep_accept(
              sqe, listener_fd,
              reinterpret_cast<sockaddr *>(&holder->peer),
              &holder->peer_length, SOCK_CLOEXEC);
        },
        [holder](cardio::io_uring_completion completion) {
          if (completion.result < 0) {
            throw std::system_error(-completion.result,
                                    std::generic_category(),
                                    "FTP active accept failed");
          }
          return FtpAcceptedSocket{
              .fd = completion.result,
              .peer = holder->peer,
              .peer_length = holder->peer_length,
          };
        },
        std::move(cancellation));
  }

public:
  cardio::io_uring io{64};
  cardio::primitives::mutex operation_mutex;
  FtpReplyParser reply_parser;
  int control_fd = -1;
  sockaddr_storage control_peer{};
  socklen_t control_peer_length = 0;
  sockaddr_storage control_local{};
  socklen_t control_local_length = 0;
  FtpDataConnectionMode data_connection_mode;
  bool has_mlsd = false;
  std::atomic_bool transfer_active = false;

  explicit FtpSessionState(FtpDataConnectionMode mode)
      : data_connection_mode(mode) {
  }

  ~FtpSessionState() {
    close_control();
  }

  FtpSessionState(const FtpSessionState &) = delete;
  FtpSessionState &operator=(const FtpSessionState &) = delete;

  void close_control() noexcept {
    if (control_fd >= 0) {
      (void)::shutdown(control_fd, SHUT_RDWR);
      close_socket(&control_fd);
    }
  }

  void capture_control_endpoints() {
    control_peer_length = sizeof(control_peer);
    if (::getpeername(control_fd,
                      reinterpret_cast<sockaddr *>(&control_peer),
                      &control_peer_length) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "FTP getpeername failed");
    }
    control_local_length = sizeof(control_local);
    if (::getsockname(control_fd,
                      reinterpret_cast<sockaddr *>(&control_local),
                      &control_local_length) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "FTP getsockname failed");
    }
  }

  cardio::promise<void> write_all_async(
      int fd, std::span<const std::byte> bytes,
      cardio::cancellation cancellation) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const std::size_t count = co_await cardio::io_urings::write(
          io, fd, bytes.subspan(offset), cancellation);
      if (count == 0) {
        throw std::runtime_error("FTP socket write made no progress");
      }
      offset += count;
    }
  }

  cardio::promise<void> send_command_async(
      std::string command, cardio::cancellation cancellation) {
    if (control_fd < 0) {
      throw std::runtime_error("FTP control connection is closed");
    }
    command += "\r\n";
    const auto *data = reinterpret_cast<const std::byte *>(command.data());
    co_await write_all_async(
        control_fd, std::span<const std::byte>(data, command.size()),
        std::move(cancellation));
  }

  cardio::promise<FtpReply> receive_reply_async(
      cardio::cancellation cancellation) {
    for (;;) {
      std::optional<FtpReply> ready = reply_parser.take_reply();
      if (ready.has_value()) {
        co_return std::move(*ready);
      }
      if (control_fd < 0) {
        throw std::runtime_error("FTP control connection is closed");
      }
      std::array<std::byte, 4096> buffer{};
      const std::size_t count = co_await cardio::io_urings::read(
          io, control_fd, buffer, cancellation);
      if (count == 0) {
        reply_parser.finish();
        throw std::runtime_error("FTP control connection ended");
      }
      reply_parser.feed(std::string_view(
          reinterpret_cast<const char *>(buffer.data()), count));
    }
  }

  cardio::promise<FtpReply> command_async(
      std::string command, cardio::cancellation cancellation) {
    co_await send_command_async(std::move(command), cancellation);
    co_return co_await receive_reply_async(std::move(cancellation));
  }

  cardio::promise<int> start_data_command_async(
      std::string command, cardio::cancellation cancellation) {
    FtpDataEndpoint endpoint;
    if (data_connection_mode == FtpDataConnectionMode::passive) {
      endpoint =
          co_await prepare_passive_endpoint_async(cancellation);
    } else {
      endpoint =
          co_await prepare_active_endpoint_async(cancellation);
    }
    try {
      const FtpReply preliminary =
          co_await command_async(std::move(command), cancellation);
      if (!reply_is_positive_preliminary(preliminary)) {
        throw ftp_status_error("FTP data command", preliminary);
      }
      if (endpoint.connected_fd >= 0) {
        co_return std::exchange(endpoint.connected_fd, -1);
      }
      FtpAcceptedSocket accepted =
          co_await accept_socket_async(endpoint.listener_fd, cancellation);
      close_socket(&endpoint.listener_fd);
      if (!socket_addresses_have_same_host(accepted.peer, control_peer)) {
        close_socket(&accepted.fd);
        throw std::runtime_error(
            "FTP active data connection came from a different host");
      }
      co_return accepted.fd;
    } catch (...) {
      close_socket(&endpoint.connected_fd);
      close_socket(&endpoint.listener_fd);
      throw;
    }
  }

  cardio::promise<void> finish_data_command_async(
      cardio::cancellation cancellation) {
    const FtpReply completion =
        co_await receive_reply_async(std::move(cancellation));
    if (!reply_is_positive_completion(completion)) {
      throw std::runtime_error(
          "FTP data transfer did not complete: " +
          ftp_reply_summary(completion));
    }
  }

  cardio::promise<std::string> read_data_command_async(
      std::string command, cardio::cancellation cancellation) {
    int data_fd = co_await start_data_command_async(
        std::move(command), cancellation);
    std::string data;
    try {
      std::array<std::byte, 16384> buffer{};
      for (;;) {
        const std::size_t count = co_await cardio::io_urings::read(
            io, data_fd, buffer, cancellation);
        if (count == 0) {
          break;
        }
        if (data.size() > maximum_listing_size - count) {
          throw std::runtime_error("FTP directory listing exceeds size limit");
        }
        data.append(reinterpret_cast<const char *>(buffer.data()), count);
      }
      close_socket(&data_fd);
      co_await finish_data_command_async(std::move(cancellation));
      co_return data;
    } catch (...) {
      close_socket(&data_fd);
      throw;
    }
  }
};

static std::vector<std::string_view> ftp_listing_lines(
    const std::string &listing) {
  std::vector<std::string_view> result;
  std::size_t begin = 0;
  while (begin < listing.size()) {
    const std::size_t newline = listing.find('\n', begin);
    const std::size_t end =
        newline == std::string::npos ? listing.size() : newline;
    std::size_t content_end = end;
    if (content_end > begin && listing[content_end - 1] == '\r') {
      --content_end;
    }
    if (content_end - begin > maximum_listing_line_size) {
      throw std::runtime_error("FTP directory listing line exceeds size limit");
    }
    if (content_end > begin) {
      result.emplace_back(listing.data() + begin, content_end - begin);
    }
    if (newline == std::string::npos) {
      break;
    }
    begin = newline + 1;
  }
  return result;
}

static std::vector<FtpDirectoryEntry>
parse_ftp_listing(const std::string &listing, bool machine_readable) {
  std::vector<FtpDirectoryEntry> result;
  for (const std::string_view line : ftp_listing_lines(listing)) {
    std::optional<FtpDirectoryEntry> entry =
        machine_readable ? parse_ftp_mlsd_entry(line)
                         : parse_ftp_list_entry(line);
    if (entry.has_value()) {
      result.push_back(std::move(*entry));
    }
  }
  return result;
}

static std::string_view trim_ascii_space(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

static bool feature_line_starts_with(std::string_view line,
                                     std::string_view feature) {
  line = trim_ascii_space(line);
  if (line.size() < feature.size()) {
    return false;
  }
  for (std::size_t index = 0; index < feature.size(); ++index) {
    const unsigned char character =
        static_cast<unsigned char>(line[index]);
    if (static_cast<char>(g_ascii_toupper(character)) != feature[index]) {
      return false;
    }
  }
  return line.size() == feature.size() || line[feature.size()] == ' ' ||
         line[feature.size()] == ';';
}

class FtpFileReader final : public RemoteFileReader {
private:
  std::shared_ptr<FtpSessionState> state;
  int data_fd;
  cardio::primitives::lock_handle operation_lock;
  bool completed = false;

  void abandon() noexcept {
    close_socket(&data_fd);
    if (!completed) {
      state->close_control();
      completed = true;
    }
    operation_lock.release();
  }

public:
  FtpFileReader(std::shared_ptr<FtpSessionState> state, int data_fd,
                cardio::primitives::lock_handle operation_lock)
      : state(std::move(state)), data_fd(data_fd),
        operation_lock(std::move(operation_lock)) {
  }

  ~FtpFileReader() override {
    abandon();
  }

  cardio::promise<std::size_t>
  read_async(std::span<std::byte> buffer,
             cardio::cancellation cancellation) override {
    if (completed || buffer.empty()) {
      co_return 0;
    }
    try {
      const std::size_t count = co_await cardio::io_urings::read(
          state->io, data_fd, buffer, cancellation);
      if (count != 0) {
        co_return count;
      }
      close_socket(&data_fd);
      co_await state->finish_data_command_async(std::move(cancellation));
      completed = true;
      operation_lock.release();
      co_return 0;
    } catch (...) {
      abandon();
      throw;
    }
  }

  cardio::promise<void>
  close_async(cardio::cancellation cancellation) override {
    (void)cancellation;
    if (!completed) {
      // Closing RETR before EOF can yield more than one control reply. Closing
      // the control connection is the only deterministic way to avoid reusing
      // a desynchronized reply stream.
      abandon();
    }
    co_return;
  }
};

class FtpFileWriter final : public RemoteFileWriter {
private:
  std::shared_ptr<FtpSessionState> state;
  int data_fd;
  cardio::primitives::lock_handle operation_lock;
  bool completed = false;

  void abandon() noexcept {
    close_socket(&data_fd);
    if (!completed) {
      state->close_control();
      completed = true;
    }
    operation_lock.release();
  }

public:
  FtpFileWriter(std::shared_ptr<FtpSessionState> state, int data_fd,
                cardio::primitives::lock_handle operation_lock)
      : state(std::move(state)), data_fd(data_fd),
        operation_lock(std::move(operation_lock)) {
  }

  ~FtpFileWriter() override {
    abandon();
  }

  cardio::promise<void>
  write_all_async(std::span<const std::byte> buffer,
                  cardio::cancellation cancellation) override {
    if (completed) {
      throw std::runtime_error("FTP writer is closed");
    }
    try {
      co_await state->write_all_async(data_fd, buffer,
                                      std::move(cancellation));
    } catch (...) {
      abandon();
      throw;
    }
  }

  cardio::promise<void>
  close_async(cardio::cancellation cancellation) override {
    if (completed) {
      co_return;
    }
    close_socket(&data_fd);
    try {
      co_await state->finish_data_command_async(std::move(cancellation));
      completed = true;
      operation_lock.release();
    } catch (...) {
      abandon();
      throw;
    }
  }
};

class FtpClient final : public RemoteFileClient {
private:
  std::shared_ptr<FtpSessionState> state;

  explicit FtpClient(std::shared_ptr<FtpSessionState> state)
      : state(std::move(state)) {
  }

  [[noreturn]] static void handle_operation_failure(
      const std::shared_ptr<FtpSessionState> &state) {
    try {
      throw;
    } catch (const FtpStatusError &) {
      throw;
    } catch (...) {
      state->close_control();
      throw;
    }
  }

  cardio::promise<RemoteDirectorySnapshot> load_directory_locked_async(
      std::string path, cardio::cancellation cancellation) {
    if (path.empty()) {
      path = ".";
    }
    const FtpReply cwd =
        co_await state->command_async("CWD " + path, cancellation);
    require_positive_completion(cwd, "FTP CWD");
    const FtpReply pwd = co_await state->command_async("PWD", cancellation);
    if (pwd.code != 257) {
      throw ftp_status_error("FTP PWD", pwd);
    }
    const std::string canonical = parse_ftp_pwd_path(pwd);
    const std::string command = state->has_mlsd ? "MLSD" : "LIST";
    const std::string listing =
        co_await state->read_data_command_async(command, cancellation);
    const std::vector<FtpDirectoryEntry> parsed =
        parse_ftp_listing(listing, state->has_mlsd);
    RemoteDirectorySnapshot result{
        .canonical_path = canonical,
        .entries = {},
    };
    for (const FtpDirectoryEntry &entry : parsed) {
      if (entry.type == FtpDirectoryEntryType::current_directory ||
          entry.type == FtpDirectoryEntryType::parent_directory) {
        continue;
      }
      result.entries.push_back(portable_attributes(
          entry, ftp_child_path(canonical, entry.name), entry.name));
    }
    std::sort(result.entries.begin(), result.entries.end(),
              [](const RemoteFileAttributes &left,
                 const RemoteFileAttributes &right) {
                return left.name < right.name;
              });
    co_return result;
  }

  cardio::promise<std::optional<RemoteFileAttributes>>
  lstat_with_mlst_locked_async(std::string path,
                               cardio::cancellation cancellation) {
    const FtpReply reply =
        co_await state->command_async("MLST " + path, cancellation);
    if (reply.code == 550) {
      co_return std::nullopt;
    }
    if (reply.code != 250) {
      throw ftp_status_error("FTP MLST", reply);
    }
    for (const std::string &line_text : reply.lines) {
      std::string_view line = trim_ascii_space(line_text);
      const std::optional<FtpDirectoryEntry> entry =
          parse_ftp_mlsd_entry(line);
      if (!entry.has_value()) {
        continue;
      }
      const std::string name = ftp_path_name(path);
      co_return portable_attributes(*entry, std::move(path), name);
    }
    throw std::runtime_error("FTP MLST response contained no facts");
  }

  cardio::promise<std::optional<RemoteFileAttributes>>
  lstat_with_list_locked_async(std::string path,
                               cardio::cancellation cancellation) {
    std::string trimmed = path;
    while (trimmed.size() > 1 && trimmed.back() == '/') {
      trimmed.pop_back();
    }
    const std::size_t separator = trimmed.find_last_of('/');
    const std::string name = ftp_path_name(trimmed);
    if (name.empty()) {
      throw std::runtime_error(
          "FTP server does not support MLST for the root item");
    }
    std::string parent = ".";
    if (separator == 0) {
      parent = "/";
    } else if (separator != std::string::npos) {
      parent = trimmed.substr(0, separator);
    }
    const RemoteDirectorySnapshot snapshot =
        co_await load_directory_locked_async(parent, cancellation);
    const auto found = std::find_if(
        snapshot.entries.begin(), snapshot.entries.end(),
        [&name](const RemoteFileAttributes &entry) {
          return entry.name == name;
        });
    if (found == snapshot.entries.end()) {
      co_return std::nullopt;
    }
    RemoteFileAttributes result = *found;
    result.path = std::move(path);
    co_return result;
  }

public:
  FtpClient(const FtpClient &) = delete;
  FtpClient &operator=(const FtpClient &) = delete;

  static cardio::promise<std::shared_ptr<RemoteFileClient>> open_async(
      FtpClientOpenOptions options, cardio::cancellation cancellation) {
    validate_ftp_argument(options.connection.username, "username", false);
    validate_ftp_argument(options.password, "password", true);
    if (options.connection.address.empty() || options.connection.port <= 0 ||
        options.connection.port > 65535) {
      throw std::invalid_argument("FTP endpoint is invalid");
    }

    auto state = std::make_shared<FtpSessionState>(
        options.connection.data_connection_mode);
    try {
      state->control_fd = co_await connect_tcp_socket_async(
          state->io, options.connection.address,
          static_cast<std::uint16_t>(options.connection.port), cancellation);
      state->capture_control_endpoints();

      FtpReply greeting = co_await state->receive_reply_async(cancellation);
      if (greeting.code == 120) {
        greeting = co_await state->receive_reply_async(cancellation);
      }
      if (greeting.code != 220) {
        throw ftp_status_error("FTP greeting", greeting);
      }

      FtpReply login = co_await state->command_async(
          "USER " + options.connection.username, cancellation);
      if (login.code == 331) {
        login = co_await state->command_async(
            "PASS " + options.password, cancellation);
      }
      require_positive_completion(login, "FTP login");

      const FtpReply features =
          co_await state->command_async("FEAT", cancellation);
      bool has_utf8 = false;
      if (features.code == 211) {
        for (const std::string &line : features.lines) {
          state->has_mlsd =
              state->has_mlsd || feature_line_starts_with(line, "MLST") ||
              feature_line_starts_with(line, "MLSD");
          has_utf8 =
              has_utf8 || feature_line_starts_with(line, "UTF8");
        }
      }
      if (has_utf8) {
        (void)co_await state->command_async("OPTS UTF8 ON", cancellation);
      }
      const FtpReply type =
          co_await state->command_async("TYPE I", cancellation);
      require_positive_completion(type, "FTP TYPE I");
      co_return std::shared_ptr<RemoteFileClient>(new FtpClient(state));
    } catch (...) {
      state->close_control();
      throw;
    }
  }

  auto capabilities() const noexcept
      -> RemoteFileCapabilities override {
    return {
        .symbolic_links = false,
        .permissions = false,
        .access_time = false,
        .modification_time = false,
    };
  }

  cardio::promise<RemoteDirectorySnapshot> load_directory_async(
      std::string path, cardio::cancellation cancellation) override {
    validate_ftp_argument(path, "path", true);
    auto lock =
        std::move(co_await state->operation_mutex.lock(cancellation));
    try {
      co_return co_await load_directory_locked_async(
          std::move(path), std::move(cancellation));
    } catch (...) {
      handle_operation_failure(state);
    }
  }

  cardio::promise<std::optional<RemoteFileAttributes>> lstat_async(
      std::string path, cardio::cancellation cancellation) override {
    validate_ftp_argument(path, "path", false);
    auto lock =
        std::move(co_await state->operation_mutex.lock(cancellation));
    try {
      if (state->has_mlsd) {
        co_return co_await lstat_with_mlst_locked_async(
            std::move(path), std::move(cancellation));
      }
      co_return co_await lstat_with_list_locked_async(
          std::move(path), std::move(cancellation));
    } catch (...) {
      handle_operation_failure(state);
    }
  }

  cardio::promise<std::string> read_link_async(
      std::string path, cardio::cancellation cancellation) override {
    (void)path;
    (void)cancellation;
    throw std::runtime_error("FTP symbolic links are not supported");
  }

  cardio::promise<void> make_directory_async(
      std::string path, std::optional<std::uint32_t> permissions,
      cardio::cancellation cancellation) override {
    (void)permissions;
    validate_ftp_argument(path, "path", false);
    auto lock =
        std::move(co_await state->operation_mutex.lock(cancellation));
    try {
      const FtpReply reply = co_await state->command_async(
          "MKD " + path, std::move(cancellation));
      require_positive_completion(reply, "FTP MKD");
    } catch (...) {
      handle_operation_failure(state);
    }
  }

  cardio::promise<void> remove_file_async(
      std::string path, cardio::cancellation cancellation) override {
    validate_ftp_argument(path, "path", false);
    auto lock =
        std::move(co_await state->operation_mutex.lock(cancellation));
    try {
      const FtpReply reply = co_await state->command_async(
          "DELE " + path, std::move(cancellation));
      require_positive_completion(reply, "FTP DELE");
    } catch (...) {
      handle_operation_failure(state);
    }
  }

  cardio::promise<void> remove_directory_async(
      std::string path, cardio::cancellation cancellation) override {
    validate_ftp_argument(path, "path", false);
    auto lock =
        std::move(co_await state->operation_mutex.lock(cancellation));
    try {
      const FtpReply reply = co_await state->command_async(
          "RMD " + path, std::move(cancellation));
      require_positive_completion(reply, "FTP RMD");
    } catch (...) {
      handle_operation_failure(state);
    }
  }

  cardio::promise<void> rename_async(
      std::string source_path, std::string destination_path,
      cardio::cancellation cancellation) override {
    validate_ftp_argument(source_path, "source path", false);
    validate_ftp_argument(destination_path, "destination path", false);
    auto lock =
        std::move(co_await state->operation_mutex.lock(cancellation));
    try {
      const FtpReply from = co_await state->command_async(
          "RNFR " + source_path, cancellation);
      if (from.code != 350) {
        throw ftp_status_error("FTP RNFR", from);
      }
      const FtpReply to = co_await state->command_async(
          "RNTO " + destination_path, std::move(cancellation));
      require_positive_completion(to, "FTP RNTO");
    } catch (...) {
      handle_operation_failure(state);
    }
  }

  cardio::promise<void> make_symbolic_link_async(
      std::string target, std::string path,
      cardio::cancellation cancellation) override {
    (void)target;
    (void)path;
    (void)cancellation;
    throw std::runtime_error("FTP symbolic links are not supported");
  }

  cardio::promise<void> set_attributes_async(
      std::string path, RemoteFileAttributes attributes,
      cardio::cancellation cancellation) override {
    (void)path;
    (void)cancellation;
    if (attributes.permissions.has_value() ||
        attributes.access_time_unix_seconds.has_value() ||
        attributes.modification_time_unix_seconds.has_value()) {
      throw std::runtime_error("FTP metadata updates are not supported");
    }
    co_return;
  }

  cardio::promise<std::unique_ptr<RemoteFileReader>> open_read_async(
      std::string path, cardio::cancellation cancellation) override {
    validate_ftp_argument(path, "path", false);
    auto lock =
        std::move(co_await state->operation_mutex.lock(cancellation));
    try {
      const int data_fd = co_await state->start_data_command_async(
          "RETR " + path, cancellation);
      co_return std::make_unique<FtpFileReader>(
          state, data_fd, std::move(lock));
    } catch (...) {
      handle_operation_failure(state);
    }
  }

  cardio::promise<std::unique_ptr<RemoteFileWriter>> open_write_async(
      std::string path, std::optional<std::uint32_t> permissions,
      cardio::cancellation cancellation) override {
    (void)permissions;
    validate_ftp_argument(path, "path", false);
    auto lock =
        std::move(co_await state->operation_mutex.lock(cancellation));
    try {
      const int data_fd = co_await state->start_data_command_async(
          "STOR " + path, cancellation);
      co_return std::make_unique<FtpFileWriter>(
          state, data_fd, std::move(lock));
    } catch (...) {
      handle_operation_failure(state);
    }
  }

  bool try_begin_transfer() override {
    bool expected = false;
    return state->transfer_active.compare_exchange_strong(expected, true);
  }

  void end_transfer() override {
    state->transfer_active.store(false);
  }
};

cardio::promise<std::shared_ptr<RemoteFileClient>>
open_ftp_client_async(FtpClientOpenOptions options,
                      cardio::cancellation cancellation) {
  return FtpClient::open_async(std::move(options), std::move(cancellation));
}

} // namespace elder_terms

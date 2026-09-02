#include "../../src/ftp/ftp-client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <glib.h>

namespace elder_terms_ftp_client_test {

struct Listener {
  int fd = -1;
  std::uint16_t port = 0;
};

struct ChildServer {
  pid_t pid = -1;
  int error_fd = -1;
  std::uint16_t port = 0;
};

static void expect(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static void close_fd(int *fd) noexcept {
  if (fd != nullptr && *fd >= 0) {
    (void)::close(*fd);
    *fd = -1;
  }
}

static Listener create_loopback_listener() {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(), "socket failed");
  }
  try {
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                     sizeof(enabled)) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "setsockopt failed");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(fd, 4) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "listen failed");
    }
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address),
                      &length) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "getsockname failed");
    }
    return {
        .fd = fd,
        .port = ntohs(address.sin_port),
    };
  } catch (...) {
    (void)::close(fd);
    throw;
  }
}

static void write_all(int fd, std::string_view text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t count = ::send(fd, text.data() + offset,
                                 text.size() - offset, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      throw std::system_error(errno, std::generic_category(),
                              "server write failed");
    }
    offset += static_cast<std::size_t>(count);
  }
}

static std::string read_line(int fd) {
  std::string line;
  while (line.size() < 8192) {
    char character = '\0';
    const ssize_t count = ::read(fd, &character, 1);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      throw std::runtime_error("FTP client closed the control connection");
    }
    line.push_back(character);
    if (line.ends_with("\r\n")) {
      line.resize(line.size() - 2);
      return line;
    }
  }
  throw std::runtime_error("FTP client command exceeded test limit");
}

static void expect_command(int fd, std::string_view expected) {
  const std::string actual = read_line(fd);
  expect(actual == expected,
         "expected command '" + std::string(expected) + "', got '" +
             actual + "'");
}

static int accept_connection(int listener_fd) {
  for (;;) {
    const int fd = ::accept4(listener_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd >= 0) {
      return fd;
    }
    if (errno != EINTR) {
      throw std::system_error(errno, std::generic_category(),
                              "accept failed");
    }
  }
}

static void expect_no_control_command(int fd) {
  pollfd descriptor{
      .fd = fd,
      .events = POLLIN,
      .revents = 0,
  };
  int result = -1;
  do {
    result = ::poll(&descriptor, 1, 150);
  } while (result < 0 && errno == EINTR);
  if (result < 0) {
    throw std::system_error(errno, std::generic_category(), "poll failed");
  }
  expect(result == 0,
         "FTP client interleaved a command into an active logical operation");
}

static void send_login_replies(int control_fd, std::string_view username,
                               std::string_view password, bool utf8) {
  write_all(control_fd, "220 Test FTP ready\r\n");
  expect_command(control_fd, "USER " + std::string(username));
  write_all(control_fd, "331 Password required\r\n");
  expect_command(control_fd, "PASS " + std::string(password));
  write_all(control_fd, "230 Login successful\r\n");
  expect_command(control_fd, "FEAT");
  write_all(control_fd,
            utf8
                ? "211-Features\r\n MLST type*;size*;modify*;\r\n UTF8\r\n211 End\r\n"
                : "211-Features\r\n MLST type*;size*;modify*;\r\n211 End\r\n");
  if (utf8) {
    expect_command(control_fd, "OPTS UTF8 ON");
    write_all(control_fd, "200 UTF8 enabled\r\n");
  }
  expect_command(control_fd, "TYPE I");
  write_all(control_fd, "200 Binary type selected\r\n");
}

static void serve_passive_listing(int control_fd, std::string_view requested,
                                  std::string_view canonical,
                                  std::string_view listing,
                                  bool verify_serialization) {
  expect_command(control_fd, "CWD " + std::string(requested));
  if (verify_serialization) {
    expect_no_control_command(control_fd);
  }
  write_all(control_fd, "250 Directory changed\r\n");
  expect_command(control_fd, "PWD");
  write_all(control_fd,
            "257 \"" + std::string(canonical) + "\" is current\r\n");

  Listener data_listener = create_loopback_listener();
  expect_command(control_fd, "EPSV");
  write_all(control_fd, "229 Entering Extended Passive Mode (|||" +
                            std::to_string(data_listener.port) + "|)\r\n");
  int data_fd = accept_connection(data_listener.fd);
  close_fd(&data_listener.fd);
  expect_command(control_fd, "MLSD");
  write_all(control_fd, "150 Opening data connection\r\n");
  write_all(data_fd, listing);
  close_fd(&data_fd);
  write_all(control_fd, "226 Directory send complete\r\n");
}

static void passive_server(int control_fd) {
  send_login_replies(control_fd, "alice", "secret", true);
  serve_passive_listing(
      control_fd, "/first", "/canonical-first",
      "type=cdir;modify=20240101000000; .\r\n"
      "type=file;size=7;modify=20240203040506; zebra.txt\r\n"
      "type=dir;modify=20240203040507; archive\r\n",
      true);
  serve_passive_listing(
      control_fd, "/second", "/canonical-second",
      "type=file;size=2;modify=20240203040508; second.txt\r\n", false);

  Listener data_listener = create_loopback_listener();
  expect_command(control_fd, "EPSV");
  write_all(control_fd, "229 Entering Extended Passive Mode (|||" +
                            std::to_string(data_listener.port) + "|)\r\n");
  int data_fd = accept_connection(data_listener.fd);
  close_fd(&data_listener.fd);
  expect_command(control_fd, "RETR /canonical-first/zebra.txt");
  write_all(control_fd, "150 Opening file\r\n");
  write_all(data_fd, "content");
  expect_no_control_command(control_fd);
  close_fd(&data_fd);
  write_all(control_fd, "226 Transfer complete\r\n");

  expect_command(control_fd, "DELE /canonical-first/zebra.txt");
  write_all(control_fd, "250 File removed\r\n");

  expect_command(control_fd, "RNFR /canonical-first/old-name.txt");
  expect_no_control_command(control_fd);
  write_all(control_fd, "350 Ready for destination name\r\n");
  expect_command(control_fd, "RNTO /canonical-first/new-name.txt");
  write_all(control_fd, "250 Rename successful\r\n");
  expect_command(control_fd, "DELE /after-rename.txt");
  write_all(control_fd, "250 Queued command completed\r\n");

  Listener rejected_listener = create_loopback_listener();
  expect_command(control_fd, "EPSV");
  write_all(control_fd, "229 Entering Extended Passive Mode (|||" +
                            std::to_string(rejected_listener.port) +
                            "|)\r\n");
  int rejected_data_fd = accept_connection(rejected_listener.fd);
  close_fd(&rejected_listener.fd);
  expect_command(control_fd, "STOR /denied.txt");
  write_all(control_fd, "550 Permission denied\r\n");
  close_fd(&rejected_data_fd);

  expect_command(control_fd, "DELE /after-denial.txt");
  write_all(control_fd, "250 Control connection remains usable\r\n");
}

static std::pair<std::string, std::uint16_t>
parse_port_command(std::string_view command) {
  expect(command.starts_with("PORT "), "expected PORT fallback command");
  command.remove_prefix(5);
  unsigned fields[6]{};
  for (std::size_t index = 0; index < 6; ++index) {
    const std::size_t comma = command.find(',');
    const std::string token(
        index == 5 ? command : command.substr(0, comma));
    expect(!token.empty() && (index == 5 || comma != std::string_view::npos),
           "malformed PORT command");
    std::size_t consumed = 0;
    fields[index] = static_cast<unsigned>(std::stoul(token, &consumed));
    expect(consumed == token.size() && fields[index] <= 255,
           "invalid PORT command field");
    if (index != 5) {
      command.remove_prefix(comma + 1);
    }
  }
  return {
      std::to_string(fields[0]) + "." + std::to_string(fields[1]) + "." +
          std::to_string(fields[2]) + "." + std::to_string(fields[3]),
      static_cast<std::uint16_t>(fields[4] * 256U + fields[5]),
  };
}

static int connect_ipv4(std::string_view address, std::uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(), "socket failed");
  }
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  if (::inet_pton(AF_INET, std::string(address).c_str(),
                  &endpoint.sin_addr) != 1 ||
      ::connect(fd, reinterpret_cast<const sockaddr *>(&endpoint),
                sizeof(endpoint)) != 0) {
    const int error = errno;
    (void)::close(fd);
    throw std::system_error(error, std::generic_category(), "connect failed");
  }
  return fd;
}

static void active_server(int control_fd) {
  send_login_replies(control_fd, "anonymous", "anonymous@", false);
  expect_command(control_fd, "CWD /");
  write_all(control_fd, "250 Directory changed\r\n");
  expect_command(control_fd, "PWD");
  write_all(control_fd, "257 \"/\" is current\r\n");

  const std::string extended = read_line(control_fd);
  expect(extended.starts_with("EPRT |1|127.0.0.1|"),
         "active IPv4 mode should try EPRT first");
  write_all(control_fd, "500 EPRT unavailable\r\n");
  const auto [address, port] = parse_port_command(read_line(control_fd));
  write_all(control_fd, "200 PORT accepted\r\n");
  expect_command(control_fd, "MLSD");
  write_all(control_fd, "150 Opening active data connection\r\n");
  int data_fd = connect_ipv4(address, port);
  write_all(data_fd, "type=file;size=3;modify=20240304050607; active.txt\r\n");
  close_fd(&data_fd);
  write_all(control_fd, "226 Directory send complete\r\n");
}

using ServerFunction = void (*)(int);

static ChildServer start_server(ServerFunction function) {
  Listener listener = create_loopback_listener();
  int error_pipe[2] = {-1, -1};
  if (::pipe2(error_pipe, O_CLOEXEC) != 0) {
    close_fd(&listener.fd);
    throw std::system_error(errno, std::generic_category(), "pipe failed");
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    const int error = errno;
    close_fd(&listener.fd);
    close_fd(&error_pipe[0]);
    close_fd(&error_pipe[1]);
    throw std::system_error(error, std::generic_category(), "fork failed");
  }
  if (pid == 0) {
    close_fd(&error_pipe[0]);
    try {
      int control_fd = accept_connection(listener.fd);
      close_fd(&listener.fd);
      function(control_fd);
      close_fd(&control_fd);
      _exit(0);
    } catch (const std::exception &exception) {
      const std::string message = exception.what();
      (void)::write(error_pipe[1], message.data(), message.size());
      _exit(1);
    }
  }
  close_fd(&listener.fd);
  close_fd(&error_pipe[1]);
  return {
      .pid = pid,
      .error_fd = error_pipe[0],
      .port = listener.port,
  };
}

static std::string read_server_error(int fd) {
  std::string result;
  char buffer[512];
  ssize_t count = 0;
  while ((count = ::read(fd, buffer, sizeof(buffer))) > 0) {
    result.append(buffer, static_cast<std::size_t>(count));
  }
  return result;
}

static std::string stop_server(ChildServer *server) {
  if (server == nullptr) {
    return {};
  }
  if (server->pid > 0) {
    (void)::kill(server->pid, SIGKILL);
    int status = 0;
    while (::waitpid(server->pid, &status, 0) < 0 && errno == EINTR) {
    }
    server->pid = -1;
  }
  const std::string error = read_server_error(server->error_fd);
  close_fd(&server->error_fd);
  return error;
}

static void finish_server(ChildServer *server) {
  int status = 0;
  pid_t result = -1;
  do {
    result = ::waitpid(server->pid, &status, 0);
  } while (result < 0 && errno == EINTR);
  if (result != server->pid) {
    throw std::system_error(errno, std::generic_category(), "waitpid failed");
  }
  server->pid = -1;
  const std::string error = read_server_error(server->error_fd);
  close_fd(&server->error_fd);
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "FTP test server failed: " + error);
}

struct TimeoutState {
  cardio::cancellation_source *source = nullptr;
  bool fired = false;
};

static gboolean cancel_timeout(gpointer data) {
  auto *state = static_cast<TimeoutState *>(data);
  state->fired = true;
  (void)state->source->cancel();
  return G_SOURCE_REMOVE;
}

static cardio::promise<void>
run_passive_client(std::uint16_t port,
                   cardio::cancellation cancellation) {
  auto client = co_await elder_terms::open_ftp_client_async(
      {
          .connection =
              {
                  .address = "127.0.0.1",
                  .port = port,
                  .username = "alice",
                  .data_connection_mode =
                      elder_terms::FtpDataConnectionMode::passive,
                  .local_directory = {},
                  .remote_directory = "/",
              },
          .password = "secret",
      },
      cancellation);

  cardio::promise<elder_terms::RemoteDirectorySnapshot> first_task =
      client->load_directory_async("/first", cancellation);
  cardio::promise<elder_terms::RemoteDirectorySnapshot> second_task =
      client->load_directory_async("/second", cancellation);
  const elder_terms::RemoteDirectorySnapshot first = co_await first_task;
  const elder_terms::RemoteDirectorySnapshot second = co_await second_task;
  expect(first.canonical_path == "/canonical-first" &&
             first.entries.size() == 2 &&
             first.entries[0].name == "archive" &&
             first.entries[0].type == elder_terms::RemoteFileType::directory &&
             first.entries[1].name == "zebra.txt" &&
             first.entries[1].size == 7,
         "passive MLSD directory snapshot was not parsed and sorted");
  expect(second.canonical_path == "/canonical-second" &&
             second.entries.size() == 1 &&
             second.entries[0].name == "second.txt",
         "queued FTP directory operation returned the wrong result");

  std::unique_ptr<elder_terms::RemoteFileReader> reader =
      std::move(co_await client->open_read_async(
          "/canonical-first/zebra.txt", cancellation));
  cardio::promise<void> remove_task = client->remove_file_async(
      "/canonical-first/zebra.txt", cancellation);
  std::string content;
  std::byte buffer[16];
  for (;;) {
    const std::size_t count =
        co_await reader->read_async(buffer, cancellation);
    if (count == 0) {
      break;
    }
    content.append(reinterpret_cast<const char *>(buffer), count);
  }
  co_await reader->close_async(cancellation);
  co_await remove_task;
  expect(content == "content", "FTP RETR returned incorrect bytes");

  cardio::promise<void> rename_task = client->rename_async(
      "/canonical-first/old-name.txt",
      "/canonical-first/new-name.txt", cancellation);
  cardio::promise<void> remove_after_rename_task =
      client->remove_file_async("/after-rename.txt", cancellation);
  co_await rename_task;
  co_await remove_after_rename_task;

  bool write_rejected = false;
  try {
    (void)co_await client->open_write_async(
        "/denied.txt", std::nullopt, cancellation);
  } catch (const std::runtime_error &error) {
    write_rejected = std::string(error.what()).find("550") !=
                     std::string::npos;
  }
  expect(write_rejected,
         "FTP STOR rejection should complete with the server status");
  co_await client->remove_file_async("/after-denial.txt", cancellation);

  bool rejected_injection = false;
  try {
    co_await client->remove_file_async(
        "/safe\r\nDELE /other", cancellation);
  } catch (const std::invalid_argument &) {
    rejected_injection = true;
  }
  expect(rejected_injection,
         "FTP command argument injection should be rejected locally");
  expect(client->try_begin_transfer() && !client->try_begin_transfer(),
         "FTP client should expose a single bulk-transfer slot");
  client->end_transfer();
}

static cardio::promise<void>
run_active_client(std::uint16_t port,
                  cardio::cancellation cancellation) {
  auto client = co_await elder_terms::open_ftp_client_async(
      {
          .connection =
              {
                  .address = "127.0.0.1",
                  .port = port,
                  .username = "anonymous",
                  .data_connection_mode =
                      elder_terms::FtpDataConnectionMode::active,
                  .local_directory = {},
                  .remote_directory = "/",
              },
          .password = "anonymous@",
      },
      cancellation);
  const elder_terms::RemoteDirectorySnapshot snapshot =
      co_await client->load_directory_async("/", cancellation);
  expect(snapshot.canonical_path == "/" && snapshot.entries.size() == 1 &&
             snapshot.entries[0].name == "active.txt",
         "active PORT fallback did not load the directory");
}

static void rejects_implicit_anonymous_login() {
  cardio::dispatcher_group_glib dispatcher_group;
  cardio::dispatcher_host_glib dispatcher(dispatcher_group);
  bool rejected = false;
  auto task = [&]() -> cardio::promise<void> {
    try {
      (void)co_await elder_terms::open_ftp_client_async(
          {
              .connection =
                  {
                      .address = {},
                      .port = 21,
                      .username = {},
                      .data_connection_mode =
                          elder_terms::FtpDataConnectionMode::passive,
                      .local_directory = {},
                      .remote_directory = {},
                  },
              .password = {},
          },
          {});
    } catch (const std::invalid_argument &exception) {
      rejected = std::string(exception.what()) == "Invalid FTP username";
    }
    dispatcher_group.shutdown();
  }();
  dispatcher.park();
  task.unsafe_result();
  expect(rejected,
         "an empty FTP username should not select anonymous login");
}

static void run_case(ServerFunction server_function, bool passive) {
  ChildServer server = start_server(server_function);
  cardio::cancellation_source cancellation_source;
  TimeoutState timeout_state{
      .source = &cancellation_source,
      .fired = false,
  };
  const guint timeout_id =
      g_timeout_add_seconds(10, cancel_timeout, &timeout_state);
  cardio::dispatcher_group_glib dispatcher_group;
  cardio::dispatcher_host_glib dispatcher(dispatcher_group);
  std::exception_ptr error;
  auto task = [&]() -> cardio::promise<void> {
    try {
      if (passive) {
        co_await run_passive_client(
            server.port, cancellation_source.get_cancellation());
      } else {
        co_await run_active_client(
            server.port, cancellation_source.get_cancellation());
      }
    } catch (...) {
      error = std::current_exception();
    }
    dispatcher_group.shutdown();
  }();
  dispatcher.park();
  task.unsafe_result();
  if (!timeout_state.fired) {
    (void)g_source_remove(timeout_id);
  }
  if (error != nullptr) {
    const std::string server_error = stop_server(&server);
    try {
      std::rethrow_exception(error);
    } catch (const std::exception &exception) {
      throw std::runtime_error(
          std::string(exception.what()) +
          (server_error.empty() ? std::string()
                                : "; FTP test server: " + server_error));
    }
  }
  try {
    finish_server(&server);
  } catch (...) {
    (void)stop_server(&server);
    throw;
  }
}

} // namespace elder_terms_ftp_client_test

int main() {
  try {
    elder_terms_ftp_client_test::run_case(
        elder_terms_ftp_client_test::passive_server, true);
    elder_terms_ftp_client_test::run_case(
        elder_terms_ftp_client_test::active_server, false);
    elder_terms_ftp_client_test::rejects_implicit_anonymous_login();
  } catch (const std::exception &exception) {
    std::cerr << "ftp-client-test: FAIL: " << exception.what() << '\n';
    return 1;
  }
  std::cout << "ftp-client-test: PASS\n";
  return 0;
}

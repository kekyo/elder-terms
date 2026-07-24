#include "../../src/terminal-sessions/ssh-session/ssh-channel-connection.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <glib.h>
#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/server.h>

namespace elder_terms_ssh_channel_connection_test {

enum class ServerAuthMode {
  none,
  password,
  public_key,
  keyboard_interactive,
};

struct ServerOptions {
  ServerAuthMode auth_mode = ServerAuthMode::none;
  std::filesystem::path host_key_path;
  std::filesystem::path authorized_key_path;
  std::string username = "test-user";
  std::string password = "test-password";
  std::string terminal_type = "screen-256color";
  int columns = 90;
  int rows = 30;
  int resized_columns = 101;
  int resized_rows = 37;
  std::string payload = "SSH integration payload";
};

struct ServerState {
  ServerOptions options;
  ssh_key authorized_key = nullptr;
  ssh_channel channel = nullptr;
  ssh_channel_callbacks_struct channel_callbacks{};
  bool none_requested = false;
  bool password_requested = false;
  bool password_accepted = false;
  bool public_key_requested = false;
  bool public_key_accepted = false;
  bool keyboard_interactive_requested = false;
  bool keyboard_interactive_accepted = false;
  bool pty_requested = false;
  bool shell_requested = false;
  bool window_changed = false;
  bool payload_echoed = false;
  bool done = false;
  std::string username;
  std::string password;
  std::string keyboard_interactive_answer;
  std::string terminal_type;
  int columns = 0;
  int rows = 0;
  int resized_columns = 0;
  int resized_rows = 0;
  std::string payload;
};

struct ChildServer {
  pid_t pid = -1;
  int port = 0;
};

struct ClientCase {
  ServerAuthMode auth_mode = ServerAuthMode::none;
  std::filesystem::path identity_file;
  std::filesystem::path known_hosts_file;
  std::string authentication_answer;
  std::vector<elder_terms::SshUserPromptKind> expected_prompts;
};

struct ClientTimeout {
  cardio::cancellation_source *cancellation_source = nullptr;
  guint source_id = 0;
};

struct TemporaryDirectoryCleanup {
  std::filesystem::path path;

  ~TemporaryDirectoryCleanup() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static const char *auth_mode_name(ServerAuthMode mode) {
  switch (mode) {
  case ServerAuthMode::none:
    return "none";
  case ServerAuthMode::password:
    return "password";
  case ServerAuthMode::public_key:
    return "public-key";
  case ServerAuthMode::keyboard_interactive:
    return "keyboard-interactive";
  }
  return "unknown";
}

static bool write_all_fd(int fd, const void *data, std::size_t size) {
  const auto *bytes = static_cast<const unsigned char *>(data);
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t written = ::write(fd, bytes + offset, size - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

static bool read_all_fd(int fd, void *data, std::size_t size) {
  auto *bytes = static_cast<unsigned char *>(data);
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t read_size = ::read(fd, bytes + offset, size - offset);
    if (read_size > 0) {
      offset += static_cast<std::size_t>(read_size);
      continue;
    }
    if (read_size < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

static std::filesystem::path
test_root_directory(const std::string &suffix) {
  return std::filesystem::temp_directory_path() /
         ("elder-terms-ssh-channel-test-" + std::to_string(::getpid()) +
          "-" + suffix);
}

static void generate_key_pair(const std::filesystem::path &private_key_path,
                              const std::filesystem::path &public_key_path,
                              const char *passphrase) {
  ssh_key key = nullptr;
  if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK &&
      ssh_pki_generate(SSH_KEYTYPE_RSA, 2048, &key) != SSH_OK) {
    throw std::runtime_error("failed to generate SSH test key");
  }
  const int private_result = ssh_pki_export_privkey_file(
      key, passphrase, nullptr, nullptr, private_key_path.c_str());
  const int public_result =
      ssh_pki_export_pubkey_file(key, public_key_path.c_str());
  ssh_key_free(key);
  if (private_result != SSH_OK || public_result != SSH_OK) {
    throw std::runtime_error("failed to export SSH test key");
  }
  if (::chmod(private_key_path.c_str(), S_IRUSR | S_IWUSR) != 0) {
    throw std::runtime_error("failed to protect SSH test private key");
  }
}

static int on_none_auth(ssh_session, const char *user, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  state->none_requested = true;
  state->username = user == nullptr ? "" : user;
  return state->options.auth_mode == ServerAuthMode::none
             ? SSH_AUTH_SUCCESS
             : SSH_AUTH_DENIED;
}

static int on_password_auth(ssh_session, const char *user,
                            const char *password, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  state->password_requested = true;
  state->username = user == nullptr ? "" : user;
  state->password = password == nullptr ? "" : password;
  state->password_accepted =
      state->options.auth_mode == ServerAuthMode::password &&
      state->username == state->options.username &&
      state->password == state->options.password;
  return state->password_accepted ? SSH_AUTH_SUCCESS : SSH_AUTH_DENIED;
}

static int on_public_key_auth(ssh_session, const char *user, ssh_key key,
                              char signature_state, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  state->public_key_requested = true;
  state->username = user == nullptr ? "" : user;
  const bool key_matches =
      state->authorized_key != nullptr && key != nullptr &&
      ssh_key_cmp(state->authorized_key, key, SSH_KEY_CMP_PUBLIC) == 0;
  const bool signature_is_usable =
      signature_state == SSH_PUBLICKEY_STATE_NONE ||
      signature_state == SSH_PUBLICKEY_STATE_VALID;
  const bool accepted =
      state->options.auth_mode == ServerAuthMode::public_key &&
      state->username == state->options.username && key_matches &&
      signature_is_usable;
  if (accepted && signature_state == SSH_PUBLICKEY_STATE_VALID) {
    state->public_key_accepted = true;
  }
  return accepted ? SSH_AUTH_SUCCESS : SSH_AUTH_DENIED;
}

static int on_keyboard_interactive_message(ssh_session session,
                                           ssh_message message,
                                           void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  if (ssh_message_type(message) != SSH_REQUEST_AUTH) {
    return 1;
  }
  if (ssh_message_subtype(message) != SSH_AUTH_METHOD_INTERACTIVE) {
    (void)ssh_message_auth_set_methods(message,
                                       SSH_AUTH_METHOD_INTERACTIVE);
    (void)ssh_message_reply_default(message);
    return 0;
  }

  state->keyboard_interactive_requested = true;
  const char *user = ssh_message_auth_user(message);
  if (user != nullptr && user[0] != '\0') {
    state->username = user;
  }
  if (ssh_message_auth_kbdint_is_response(message) == 0) {
    const char *prompts[] = {"Password: "};
    char echo[] = {0};
    return ssh_message_auth_interactive_request(
               message, "Keyboard Interactive Authentication",
               "Enter the test password.", 1, prompts, echo) == SSH_OK
               ? 0
               : 1;
  }

  const int answer_count = ssh_userauth_kbdint_getnanswers(session);
  const char *answer =
      answer_count > 0 ? ssh_userauth_kbdint_getanswer(session, 0) : nullptr;
  state->keyboard_interactive_answer = answer == nullptr ? "" : answer;
  state->keyboard_interactive_accepted =
      state->options.auth_mode == ServerAuthMode::keyboard_interactive &&
      state->username == state->options.username &&
      state->keyboard_interactive_answer == state->options.password;
  if (state->keyboard_interactive_accepted) {
    return ssh_message_auth_reply_success(message, 0) == SSH_OK ? 0 : 1;
  }
  (void)ssh_message_auth_set_methods(message, SSH_AUTH_METHOD_INTERACTIVE);
  (void)ssh_message_reply_default(message);
  return 0;
}

static int on_channel_data(ssh_session, ssh_channel channel, void *data,
                           std::uint32_t size, int is_stderr,
                           void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  if (is_stderr != 0 || data == nullptr || size == 0) {
    return static_cast<int>(size);
  }
  state->payload.append(static_cast<const char *>(data), size);
  const int written = ssh_channel_write(channel, data, size);
  state->payload_echoed = written == static_cast<int>(size);
  if (state->payload_echoed) {
    (void)ssh_channel_send_eof(channel);
  }
  state->done = true;
  return static_cast<int>(size);
}

static void on_channel_close(ssh_session, ssh_channel, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  state->done = true;
}

static int on_pty_request(ssh_session, ssh_channel, const char *term,
                          int columns, int rows, int, int, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  state->pty_requested = true;
  state->terminal_type = term == nullptr ? "" : term;
  state->columns = columns;
  state->rows = rows;
  return 0;
}

static int on_shell_request(ssh_session, ssh_channel, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  state->shell_requested = true;
  return 0;
}

static int on_window_change(ssh_session, ssh_channel, int columns, int rows,
                            int, int, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  state->window_changed = true;
  state->resized_columns = columns;
  state->resized_rows = rows;
  return 0;
}

static ssh_channel on_channel_open(ssh_session session, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  state->channel = ssh_channel_new(session);
  if (state->channel == nullptr) {
    return nullptr;
  }
  state->channel_callbacks.userdata = state;
  state->channel_callbacks.channel_data_function = on_channel_data;
  state->channel_callbacks.channel_close_function = on_channel_close;
  state->channel_callbacks.channel_pty_request_function = on_pty_request;
  state->channel_callbacks.channel_shell_request_function = on_shell_request;
  state->channel_callbacks.channel_pty_window_change_function =
      on_window_change;
  ssh_callbacks_init(&state->channel_callbacks);
  if (ssh_set_channel_callbacks(state->channel,
                                &state->channel_callbacks) != SSH_OK) {
    ssh_channel_free(state->channel);
    state->channel = nullptr;
  }
  return state->channel;
}

static std::optional<int> bound_port(ssh_bind bind) {
  const socket_t fd = ssh_bind_get_fd(bind);
  if (fd < 0) {
    return std::nullopt;
  }
  sockaddr_in address{};
  socklen_t address_size = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address),
                    &address_size) != 0) {
    return std::nullopt;
  }
  return static_cast<int>(ntohs(address.sin_port));
}

static int validate_server_state(const ServerState &state) {
  if (state.options.auth_mode ==
      ServerAuthMode::keyboard_interactive) {
    if (!state.keyboard_interactive_requested) {
      return 30;
    }
    if (state.username != state.options.username) {
      return 31;
    }
    if (state.keyboard_interactive_answer != state.options.password) {
      return 32;
    }
    if (!state.keyboard_interactive_accepted) {
      return 33;
    }
  }
  if (!state.pty_requested ||
      state.terminal_type != state.options.terminal_type ||
      state.columns != state.options.columns ||
      state.rows != state.options.rows || !state.shell_requested ||
      !state.window_changed ||
      state.resized_columns != state.options.resized_columns ||
      state.resized_rows != state.options.resized_rows ||
      state.payload != state.options.payload || !state.payload_echoed) {
    return 18;
  }

  switch (state.options.auth_mode) {
  case ServerAuthMode::none:
    return state.none_requested &&
                   state.username == state.options.username
               ? 0
               : 19;
  case ServerAuthMode::password:
    return state.password_requested && state.password_accepted ? 0 : 20;
  case ServerAuthMode::public_key:
    return state.public_key_requested && state.public_key_accepted ? 0 : 21;
  case ServerAuthMode::keyboard_interactive:
    return 0;
  }
  return 22;
}

static int run_server_process(const ServerOptions &options, int port_fd) {
  ::alarm(8);
  ServerState state;
  state.options = options;
  if (options.auth_mode == ServerAuthMode::public_key &&
      ssh_pki_import_pubkey_file(options.authorized_key_path.c_str(),
                                 &state.authorized_key) != SSH_OK) {
    return 10;
  }

  ssh_bind bind = ssh_bind_new();
  if (bind == nullptr) {
    ssh_key_free(state.authorized_key);
    return 11;
  }
  const char *address = "127.0.0.1";
  int port = 0;
  if (ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDADDR, address) !=
          SSH_OK ||
      ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT, &port) !=
          SSH_OK ||
      ssh_bind_options_set(bind, SSH_BIND_OPTIONS_HOSTKEY,
                           options.host_key_path.c_str()) != SSH_OK ||
      ssh_bind_listen(bind) != SSH_OK) {
    ssh_bind_free(bind);
    ssh_key_free(state.authorized_key);
    return 12;
  }
  const std::optional<int> listening_port = bound_port(bind);
  if (!listening_port.has_value() ||
      !write_all_fd(port_fd, &*listening_port, sizeof(*listening_port))) {
    ssh_bind_free(bind);
    ssh_key_free(state.authorized_key);
    return 13;
  }
  (void)::close(port_fd);

  ssh_session session = ssh_new();
  if (session == nullptr || ssh_bind_accept(bind, session) != SSH_OK) {
    if (session != nullptr) {
      ssh_free(session);
    }
    ssh_bind_free(bind);
    ssh_key_free(state.authorized_key);
    return 14;
  }

  ssh_server_callbacks_struct server_callbacks{};
  server_callbacks.userdata = &state;
  server_callbacks.auth_none_function = on_none_auth;
  server_callbacks.auth_password_function = on_password_auth;
  server_callbacks.auth_pubkey_function = on_public_key_auth;
  server_callbacks.channel_open_request_session_function = on_channel_open;
  ssh_callbacks_init(&server_callbacks);
  if (ssh_set_server_callbacks(session, &server_callbacks) != SSH_OK) {
    ssh_disconnect(session);
    ssh_free(session);
    ssh_bind_free(bind);
    ssh_key_free(state.authorized_key);
    return 15;
  }

  int auth_methods = SSH_AUTH_METHOD_NONE;
  switch (options.auth_mode) {
  case ServerAuthMode::none:
    auth_methods = SSH_AUTH_METHOD_NONE;
    break;
  case ServerAuthMode::password:
    auth_methods = SSH_AUTH_METHOD_PASSWORD;
    break;
  case ServerAuthMode::public_key:
    auth_methods = SSH_AUTH_METHOD_PUBLICKEY;
    break;
  case ServerAuthMode::keyboard_interactive:
    auth_methods = SSH_AUTH_METHOD_INTERACTIVE;
    ssh_set_message_callback(session, on_keyboard_interactive_message, &state);
    break;
  }
  ssh_set_auth_methods(session, auth_methods);
  if (ssh_handle_key_exchange(session) != SSH_OK) {
    ssh_disconnect(session);
    ssh_free(session);
    ssh_bind_free(bind);
    ssh_key_free(state.authorized_key);
    return 16;
  }
  ssh_set_blocking(session, 0);

  ssh_event event = ssh_event_new();
  if (event == nullptr || ssh_event_add_session(event, session) != SSH_OK) {
    if (event != nullptr) {
      ssh_event_free(event);
    }
    ssh_disconnect(session);
    ssh_free(session);
    ssh_bind_free(bind);
    ssh_key_free(state.authorized_key);
    return 17;
  }
  while (!state.done) {
    if (ssh_event_dopoll(event, -1) == SSH_ERROR) {
      break;
    }
  }

  const int validation_result = validate_server_state(state);
  ssh_event_remove_session(event, session);
  ssh_event_free(event);
  if (state.channel != nullptr) {
    ssh_channel_free(state.channel);
    state.channel = nullptr;
  }
  ssh_disconnect(session);
  ssh_free(session);
  ssh_bind_free(bind);
  ssh_key_free(state.authorized_key);
  return validation_result;
}

static ChildServer start_server(const ServerOptions &options) {
  int port_pipe[2] = {-1, -1};
  if (::pipe(port_pipe) != 0) {
    throw std::runtime_error("failed to create SSH server port pipe");
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    (void)::close(port_pipe[0]);
    (void)::close(port_pipe[1]);
    throw std::runtime_error("failed to fork SSH test server");
  }
  if (pid == 0) {
    (void)::close(port_pipe[0]);
    const int result = run_server_process(options, port_pipe[1]);
    (void)::close(port_pipe[1]);
    ::_exit(result);
  }

  (void)::close(port_pipe[1]);
  int port = 0;
  const bool received = read_all_fd(port_pipe[0], &port, sizeof(port));
  (void)::close(port_pipe[0]);
  if (!received || port <= 0) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    throw std::runtime_error("SSH test server did not report a port");
  }
  return ChildServer{
      .pid = pid,
      .port = port,
  };
}

static int wait_for_server(ChildServer *server) {
  int status = 0;
  pid_t result = -1;
  do {
    result = ::waitpid(server->pid, &status, 0);
  } while (result < 0 && errno == EINTR);
  server->pid = -1;
  if (result < 0 || !WIFEXITED(status)) {
    return -1;
  }
  return WEXITSTATUS(status);
}

static gboolean cancel_client_timeout(gpointer data) {
  auto *timeout = static_cast<ClientTimeout *>(data);
  timeout->source_id = 0;
  if (timeout->cancellation_source != nullptr) {
    (void)timeout->cancellation_source->cancel();
  }
  return G_SOURCE_REMOVE;
}

static std::span<const unsigned char> bytes(const std::string &value) {
  return std::span<const unsigned char>(
      reinterpret_cast<const unsigned char *>(value.data()), value.size());
}

static void run_client_case(const ServerOptions &server_options,
                            const ClientCase &client_case) {
  ChildServer server = start_server(server_options);
  std::vector<elder_terms::SshUserPromptKind> prompts;
  std::vector<elder_terms::TerminalSessionConnectionPhase> phases;
  std::exception_ptr async_error;
  cardio::cancellation_source cancellation_source;
  ClientTimeout timeout{
      .cancellation_source = &cancellation_source,
      .source_id = 0,
  };
  timeout.source_id =
      g_timeout_add_seconds(5, cancel_client_timeout, &timeout);

  cardio::dispatcher_group_glib dispatcher_group;
  cardio::dispatcher_host_glib dispatcher(dispatcher_group);
  auto task = [&]() -> cardio::promise<void> {
    try {
      elder_terms::TerminalSessionCallbacks callbacks{
          .ended = {},
          .activity = {},
          .indicator_state = {},
          .connection_phase =
              [&phases](
                  elder_terms::TerminalSessionConnectionPhase phase) {
                phases.push_back(phase);
              },
          .output = {},
          .zmodem_auto_start = {},
          .ssh_prompt =
              [&prompts,
               &client_case](const elder_terms::SshUserPrompt &prompt,
                             cardio::cancellation cancellation)
              -> cardio::promise<elder_terms::SshUserPromptResponse> {
            cancellation.throw_if_cancellation_requested();
            prompts.push_back(prompt.kind);
            elder_terms::SshUserPromptResponse response{
                .accepted = true,
                .text = {},
            };
            if (prompt.kind !=
                elder_terms::SshUserPromptKind::host_key) {
              response.text = client_case.authentication_answer;
            }
            co_return response;
          },
      };
      elder_terms::SshConnectionSettings settings{
          .endpoint =
              {
                  .address = "127.0.0.1",
                  .port = server.port,
                  .username = server_options.username,
                  .identity_file = client_case.identity_file.string(),
              },
          .terminal_type = server_options.terminal_type,
      };
      auto connect_promise =
          elder_terms::SshChannelConnection::connect_async(
              settings, server_options.columns, server_options.rows,
              callbacks,
              elder_terms::SshChannelConnectionOptions{
                  .known_hosts_file =
                      client_case.known_hosts_file.string(),
              },
              cancellation_source.get_cancellation());
      std::unique_ptr<elder_terms::SshChannelConnection> connection =
          std::move(co_await connect_promise);
      co_await connection->resize_async(
          server_options.resized_columns, server_options.resized_rows,
          cancellation_source.get_cancellation());
      co_await connection->write_all_async(
          bytes(server_options.payload),
          cancellation_source.get_cancellation());

      std::string echoed;
      std::array<unsigned char, 128> buffer{};
      while (echoed.size() < server_options.payload.size()) {
        const std::size_t read_size = co_await connection->read_async(
            std::span<unsigned char>(buffer.data(), buffer.size()),
            cancellation_source.get_cancellation());
        if (read_size == 0) {
          break;
        }
        echoed.append(reinterpret_cast<const char *>(buffer.data()),
                      read_size);
      }
      expect_true(echoed == server_options.payload,
                  "SSH channel did not return the echoed payload");
      connection->close();
    } catch (...) {
      async_error = std::current_exception();
    }
    dispatcher_group.shutdown();
  }();

  dispatcher.park();
  task.unsafe_result();
  if (timeout.source_id != 0) {
    g_source_remove(timeout.source_id);
    timeout.source_id = 0;
  }
  const int server_result = wait_for_server(&server);
  if (async_error) {
    try {
      std::rethrow_exception(async_error);
    } catch (const std::exception &error) {
      throw std::runtime_error(
          std::string("SSH ") + auth_mode_name(client_case.auth_mode) +
          " client failed: " + error.what() +
          "; server result=" + std::to_string(server_result));
    }
  }
  expect_true(server_result == 0,
              "SSH test server validation failed with code " +
                  std::to_string(server_result));
  expect_true(prompts == client_case.expected_prompts,
              "SSH user prompt sequence did not match");
  expect_true(
      phases ==
          std::vector<elder_terms::TerminalSessionConnectionPhase>{
              elder_terms::TerminalSessionConnectionPhase::verifying_host,
              elder_terms::TerminalSessionConnectionPhase::authenticating,
              elder_terms::TerminalSessionConnectionPhase::opening_shell,
          },
      "SSH connection phases did not match");
}

static std::size_t line_count(const std::filesystem::path &path) {
  std::ifstream file(path);
  std::size_t count = 0;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty()) {
      ++count;
    }
  }
  return count;
}

static void test_supported_authentication_and_shell_channel() {
  const std::filesystem::path root = test_root_directory("authentication");
  std::filesystem::remove_all(root);
  TemporaryDirectoryCleanup cleanup{
      .path = root,
  };
  std::filesystem::create_directories(root / ".ssh");
  (void)::unsetenv("SSH_AUTH_SOCK");
  const std::filesystem::path known_hosts_file =
      root / ".ssh" / "known_hosts";

  const std::filesystem::path host_private_key =
      root / "ssh_host_ed25519_key";
  const std::filesystem::path host_public_key =
      root / "ssh_host_ed25519_key.pub";
  const std::filesystem::path plain_private_key =
      root / ".ssh" / "id_plain";
  const std::filesystem::path plain_public_key =
      root / ".ssh" / "id_plain.pub";
  const std::filesystem::path encrypted_private_key =
      root / ".ssh" / "id_encrypted";
  const std::filesystem::path encrypted_public_key =
      root / ".ssh" / "id_encrypted.pub";
  generate_key_pair(host_private_key, host_public_key, nullptr);
  generate_key_pair(plain_private_key, plain_public_key, nullptr);
  generate_key_pair(encrypted_private_key, encrypted_public_key,
                    "key-passphrase");

  ServerOptions options;
  options.auth_mode = ServerAuthMode::none;
  options.host_key_path = host_private_key;
  run_client_case(
      options,
      ClientCase{
          .auth_mode = ServerAuthMode::none,
          .identity_file = {},
          .known_hosts_file = known_hosts_file,
          .authentication_answer = {},
          .expected_prompts = {
              elder_terms::SshUserPromptKind::host_key,
          },
      });

  options.auth_mode = ServerAuthMode::password;
  run_client_case(
      options,
      ClientCase{
          .auth_mode = ServerAuthMode::password,
          .identity_file = {},
          .known_hosts_file = known_hosts_file,
          .authentication_answer = options.password,
          .expected_prompts = {
              elder_terms::SshUserPromptKind::host_key,
              elder_terms::SshUserPromptKind::password,
          },
      });

  options.auth_mode = ServerAuthMode::public_key;
  options.authorized_key_path = plain_public_key;
  run_client_case(
      options,
      ClientCase{
          .auth_mode = ServerAuthMode::public_key,
          .identity_file = plain_private_key,
          .known_hosts_file = known_hosts_file,
          .authentication_answer = {},
          .expected_prompts = {
              elder_terms::SshUserPromptKind::host_key,
          },
      });

  options.authorized_key_path = encrypted_public_key;
  run_client_case(
      options,
      ClientCase{
          .auth_mode = ServerAuthMode::public_key,
          .identity_file = encrypted_private_key,
          .known_hosts_file = known_hosts_file,
          .authentication_answer = "key-passphrase",
          .expected_prompts = {
              elder_terms::SshUserPromptKind::host_key,
              elder_terms::SshUserPromptKind::private_key_passphrase,
          },
      });

  options.auth_mode = ServerAuthMode::keyboard_interactive;
  options.authorized_key_path.clear();
  run_client_case(
      options,
      ClientCase{
          .auth_mode = ServerAuthMode::keyboard_interactive,
          .identity_file = {},
          .known_hosts_file = known_hosts_file,
          .authentication_answer = options.password,
          .expected_prompts = {
              elder_terms::SshUserPromptKind::host_key,
              elder_terms::SshUserPromptKind::keyboard_interactive,
          },
      });

  expect_true(line_count(known_hosts_file) == 5,
              "accepted SSH host keys were not saved in the isolated home");
}

} // namespace elder_terms_ssh_channel_connection_test

int main() {
  try {
    elder_terms_ssh_channel_connection_test::
        test_supported_authentication_and_shell_channel();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}

#include "../../src/terminal-sessions/ssh-session/ssh-channel-connection.h"
#include "../../src/sftp/sftp-client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
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
  std::filesystem::path sftp_root;
};

struct ServerState {
  ServerOptions options;
  ssh_key authorized_key = nullptr;
  ssh_channel channel = nullptr;
  ssh_channel sftp_channel = nullptr;
  ssh_channel_callbacks_struct channel_callbacks{};
  ssh_channel_callbacks_struct sftp_channel_callbacks{};
  ssh_event event = nullptr;
  pid_t sftp_server_pid = -1;
  int sftp_server_input_fd = -1;
  int sftp_server_output_fd = -1;
  bool sftp_requested = false;
  bool sftp_started = false;
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
  bool release_requested = false;
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
  int release_fd = -1;
};

struct ClientCase {
  ServerAuthMode auth_mode = ServerAuthMode::none;
  std::string setting_username = "test-user";
  std::string expected_initial_username = "test-user";
  std::string prompted_username = "test-user";
  std::filesystem::path identity_file;
  std::filesystem::path known_hosts_file;
  std::filesystem::path config_file;
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
  return static_cast<int>(size);
}

static void close_server_fd(int *fd) {
  if (*fd >= 0) {
    (void)::close(*fd);
    *fd = -1;
  }
}

static void stop_sftp_server(ServerState *state) {
  close_server_fd(&state->sftp_server_input_fd);
  if (state->event != nullptr &&
      state->sftp_server_output_fd >= 0) {
    ssh_event_remove_fd(state->event, state->sftp_server_output_fd);
  }
  close_server_fd(&state->sftp_server_output_fd);
  if (state->sftp_server_pid < 0) {
    return;
  }

  int status = 0;
  pid_t result = ::waitpid(state->sftp_server_pid, &status, WNOHANG);
  if (result == 0) {
    (void)::kill(state->sftp_server_pid, SIGTERM);
    do {
      result = ::waitpid(state->sftp_server_pid, &status, 0);
    } while (result < 0 && errno == EINTR);
  }
  state->sftp_server_pid = -1;
}

static int on_sftp_server_output(socket_t fd, int revents,
                                 void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  if ((revents & (POLLIN | POLLHUP | POLLERR)) == 0 ||
      state->sftp_channel == nullptr) {
    return 0;
  }

  std::array<unsigned char, 32768> buffer{};
  const ssize_t size = ::read(fd, buffer.data(), buffer.size());
  if (size == 0) {
    if (state->event != nullptr) {
      ssh_event_remove_fd(state->event, fd);
    }
    close_server_fd(&state->sftp_server_output_fd);
    (void)ssh_channel_send_eof(state->sftp_channel);
    return 0;
  }
  if (size < 0) {
    return errno == EINTR || errno == EAGAIN ? 0 : -1;
  }

  std::size_t offset = 0;
  while (offset < static_cast<std::size_t>(size)) {
    const int written = ssh_channel_write(
        state->sftp_channel, buffer.data() + offset,
        static_cast<std::uint32_t>(
            static_cast<std::size_t>(size) - offset));
    if (written <= 0) {
      return -1;
    }
    offset += static_cast<std::size_t>(written);
  }
  return 0;
}

static bool start_sftp_server(ServerState *state) {
  int input_pipe[2] = {-1, -1};
  int output_pipe[2] = {-1, -1};
  if (::pipe(input_pipe) != 0) {
    return false;
  }
  if (::pipe(output_pipe) != 0) {
    close_server_fd(&input_pipe[0]);
    close_server_fd(&input_pipe[1]);
    return false;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    close_server_fd(&input_pipe[0]);
    close_server_fd(&input_pipe[1]);
    close_server_fd(&output_pipe[0]);
    close_server_fd(&output_pipe[1]);
    return false;
  }
  if (pid == 0) {
    (void)::dup2(input_pipe[0], STDIN_FILENO);
    (void)::dup2(output_pipe[1], STDOUT_FILENO);
    close_server_fd(&input_pipe[0]);
    close_server_fd(&input_pipe[1]);
    close_server_fd(&output_pipe[0]);
    close_server_fd(&output_pipe[1]);
    ::execl("/usr/lib/openssh/sftp-server", "sftp-server", "-d",
            state->options.sftp_root.c_str(), nullptr);
    ::_exit(127);
  }

  close_server_fd(&input_pipe[0]);
  close_server_fd(&output_pipe[1]);
  state->sftp_server_pid = pid;
  state->sftp_server_input_fd = input_pipe[1];
  state->sftp_server_output_fd = output_pipe[0];
  if (state->event == nullptr ||
      ssh_event_add_fd(state->event, state->sftp_server_output_fd,
                       POLLIN | POLLHUP | POLLERR,
                       on_sftp_server_output, state) != SSH_OK) {
    stop_sftp_server(state);
    return false;
  }
  state->sftp_started = true;
  return true;
}

static int on_sftp_channel_data(ssh_session, ssh_channel, void *data,
                                std::uint32_t size, int is_stderr,
                                void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  if (is_stderr != 0 || data == nullptr || size == 0) {
    return static_cast<int>(size);
  }
  if (state->sftp_server_input_fd < 0 ||
      !write_all_fd(state->sftp_server_input_fd, data, size)) {
    return -1;
  }
  return static_cast<int>(size);
}

static void on_sftp_channel_eof(ssh_session, ssh_channel,
                                void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  close_server_fd(&state->sftp_server_input_fd);
}

static int on_sftp_subsystem_request(ssh_session, ssh_channel,
                                     const char *subsystem,
                                     void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  state->sftp_requested =
      subsystem != nullptr && std::string(subsystem) == "sftp";
  if (!state->sftp_requested || state->options.sftp_root.empty()) {
    return 1;
  }
  return start_sftp_server(state) ? 0 : 1;
}

static int on_release_requested(socket_t fd, int revents, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  if ((revents & POLLIN) == 0) {
    return 0;
  }
  unsigned char value = 0;
  const ssize_t read_size = ::read(fd, &value, sizeof(value));
  if (read_size == static_cast<ssize_t>(sizeof(value))) {
    state->release_requested = true;
  }
  return 0;
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
  ssh_channel channel = ssh_channel_new(session);
  if (channel == nullptr) {
    return nullptr;
  }
  if (state->channel != nullptr) {
    state->sftp_channel = channel;
    state->sftp_channel_callbacks.userdata = state;
    state->sftp_channel_callbacks.channel_data_function =
        on_sftp_channel_data;
    state->sftp_channel_callbacks.channel_eof_function =
        on_sftp_channel_eof;
    state->sftp_channel_callbacks.channel_close_function =
        on_sftp_channel_eof;
    state->sftp_channel_callbacks.channel_subsystem_request_function =
        on_sftp_subsystem_request;
    ssh_callbacks_init(&state->sftp_channel_callbacks);
    if (ssh_set_channel_callbacks(
            channel, &state->sftp_channel_callbacks) != SSH_OK) {
      ssh_channel_free(channel);
      state->sftp_channel = nullptr;
      return nullptr;
    }
    return channel;
  }

  state->channel = channel;
  state->channel_callbacks.userdata = state;
  state->channel_callbacks.channel_data_function = on_channel_data;
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
  if (!state.options.sftp_root.empty() &&
      (!state.sftp_requested || !state.sftp_started)) {
    return 34;
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

static int run_server_process(const ServerOptions &options, int port_fd,
                              int release_fd) {
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
  const char *server_banner = "SSH-2.0-OpenSSH_9.6";
  int port = 0;
  if (ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDADDR, address) !=
          SSH_OK ||
      ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT, &port) !=
          SSH_OK ||
      ssh_bind_options_set(bind, SSH_BIND_OPTIONS_HOSTKEY,
                           options.host_key_path.c_str()) != SSH_OK ||
      ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BANNER,
                           server_banner) != SSH_OK ||
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
  if (ssh_event_add_fd(event, release_fd, POLLIN, on_release_requested,
                       &state) != SSH_OK) {
    ssh_event_remove_session(event, session);
    ssh_event_free(event);
    ssh_disconnect(session);
    ssh_free(session);
    ssh_bind_free(bind);
    ssh_key_free(state.authorized_key);
    return 23;
  }
  state.event = event;
  while (!state.release_requested && ssh_is_connected(session) != 0) {
    if (ssh_event_dopoll(event, -1) == SSH_ERROR) {
      break;
    }
  }

  const int validation_result =
      state.release_requested ? validate_server_state(state) : 24;
  stop_sftp_server(&state);
  state.event = nullptr;
  ssh_event_remove_fd(event, release_fd);
  ssh_event_remove_session(event, session);
  ssh_event_free(event);
  if (state.channel != nullptr) {
    ssh_channel_free(state.channel);
    state.channel = nullptr;
  }
  if (state.sftp_channel != nullptr) {
    ssh_channel_free(state.sftp_channel);
    state.sftp_channel = nullptr;
  }
  ssh_disconnect(session);
  ssh_free(session);
  ssh_bind_free(bind);
  ssh_key_free(state.authorized_key);
  return validation_result;
}

static ChildServer start_server(const ServerOptions &options) {
  int port_pipe[2] = {-1, -1};
  int release_pipe[2] = {-1, -1};
  if (::pipe(port_pipe) != 0) {
    throw std::runtime_error("failed to create SSH server port pipe");
  }
  if (::pipe(release_pipe) != 0) {
    (void)::close(port_pipe[0]);
    (void)::close(port_pipe[1]);
    throw std::runtime_error("failed to create SSH server release pipe");
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    (void)::close(port_pipe[0]);
    (void)::close(port_pipe[1]);
    (void)::close(release_pipe[0]);
    (void)::close(release_pipe[1]);
    throw std::runtime_error("failed to fork SSH test server");
  }
  if (pid == 0) {
    (void)::close(port_pipe[0]);
    (void)::close(release_pipe[1]);
    const int result =
        run_server_process(options, port_pipe[1], release_pipe[0]);
    (void)::close(port_pipe[1]);
    (void)::close(release_pipe[0]);
    ::_exit(result);
  }

  (void)::close(port_pipe[1]);
  (void)::close(release_pipe[0]);
  int port = 0;
  const bool received = read_all_fd(port_pipe[0], &port, sizeof(port));
  (void)::close(port_pipe[0]);
  if (!received || port <= 0) {
    (void)::close(release_pipe[1]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    throw std::runtime_error("SSH test server did not report a port");
  }
  return ChildServer{
      .pid = pid,
      .port = port,
      .release_fd = release_pipe[1],
  };
}

static int wait_for_server(ChildServer *server) {
  if (server->release_fd >= 0) {
    (void)::close(server->release_fd);
    server->release_fd = -1;
  }
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

static std::span<const std::byte> byte_span(const std::string &value) {
  return std::span<const std::byte>(
      reinterpret_cast<const std::byte *>(value.data()), value.size());
}

static cardio::promise<void>
exercise_sftp_client_async(
    const std::shared_ptr<elder_terms::AuthenticatedSshTransport>
        &transport,
    cardio::cancellation cancellation) {
  std::shared_ptr<elder_terms::RemoteFileClient> client =
      co_await elder_terms::open_sftp_client_async(transport,
                                                   cancellation);
  expect_true(client != nullptr, "SFTP client was not created");
  const elder_terms::RemoteDirectorySnapshot snapshot =
      co_await client->load_directory_async(".", cancellation);
  expect_true(!snapshot.canonical_path.empty() &&
                  snapshot.canonical_path.front() == '/',
              "SFTP root was not canonicalized");

  const std::vector<elder_terms::RemoteFileAttributes> &entries =
      snapshot.entries;
  const auto server_file = std::find_if(
      entries.begin(), entries.end(),
      [](const elder_terms::RemoteFileAttributes &entry) {
        return entry.name == "server.txt" &&
               entry.type == elder_terms::RemoteFileType::regular;
      });
  const auto server_link = std::find_if(
      entries.begin(), entries.end(),
      [](const elder_terms::RemoteFileAttributes &entry) {
        return entry.name == "server-link" &&
               entry.type == elder_terms::RemoteFileType::symbolic_link;
      });
  expect_true(server_file != entries.end() &&
                  server_link != entries.end(),
              "SFTP directory listing lost file types");
  expect_true(
      co_await client->read_link_async("server-link", cancellation) ==
          "server.txt",
      "SFTP symbolic-link target did not match");

  std::unique_ptr<elder_terms::RemoteFileReader> reader =
      std::move(co_await client->open_read_async("server.txt",
                                                 cancellation));
  std::array<std::byte, 128> buffer{};
  const std::size_t read_size =
      co_await reader->read_async(buffer, cancellation);
  co_await reader->close_async(cancellation);
  reader.reset();
  expect_true(
      std::string(reinterpret_cast<const char *>(buffer.data()),
                  read_size) == "SFTP server payload",
      "SFTP file read did not return server content");

  const std::string uploaded_content = "SFTP uploaded payload";
  std::unique_ptr<elder_terms::RemoteFileWriter> writer =
      std::move(co_await client->open_write_async("uploaded.txt", 0600,
                                                  cancellation));
  co_await writer->write_all_async(byte_span(uploaded_content),
                                   cancellation);
  co_await writer->close_async(cancellation);
  writer.reset();
  co_await client->set_attributes_async(
      "uploaded.txt",
      elder_terms::RemoteFileAttributes{
          .name = {},
          .path = {},
          .type = elder_terms::RemoteFileType::other,
          .size = 0,
          .permissions = 0640,
          .access_time_unix_seconds = 1'700'002'000,
          .modification_time_unix_seconds = 1'700'002'123,
      },
      cancellation);
  const std::optional<elder_terms::RemoteFileAttributes> uploaded =
      co_await client->lstat_async("uploaded.txt", cancellation);
  expect_true(uploaded.has_value() &&
                  uploaded->type == elder_terms::RemoteFileType::regular &&
                  uploaded->size == uploaded_content.size() &&
                  uploaded->permissions.has_value() &&
                  (*uploaded->permissions & 0777U) == 0640U &&
                  uploaded->modification_time_unix_seconds ==
                      1'700'002'123,
              "SFTP write or metadata update did not persist");

  co_await client->rename_async("uploaded.txt", "renamed.txt",
                                cancellation);
  co_await client->make_symbolic_link_async(
      "renamed.txt", "created-link", cancellation);
  expect_true(
      co_await client->read_link_async("created-link", cancellation) ==
          "renamed.txt",
      "SFTP symbolic-link creation did not persist");
  co_await client->remove_file_async("created-link", cancellation);
  co_await client->remove_file_async("renamed.txt", cancellation);
  co_await client->make_directory_async("created-directory", 0750,
                                        cancellation);
  co_await client->remove_directory_async("created-directory",
                                          cancellation);
  expect_true(
      !(co_await client->lstat_async("missing.txt", cancellation))
           .has_value(),
      "SFTP lstat should distinguish a missing item");
  expect_true(client->try_begin_transfer() &&
                  !client->try_begin_transfer(),
              "SFTP client did not enforce one bulk transfer");
  client->end_transfer();
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
          .failure = {},
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
            if (prompt.kind == elder_terms::SshUserPromptKind::username) {
              expect_true(
                  prompt.initial_text ==
                      client_case.expected_initial_username,
                  "SSH username prompt initial value did not match");
              response.text = client_case.prompted_username;
            } else if (prompt.kind !=
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
                  .username = client_case.setting_username,
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
                  .config_file = client_case.config_file.string(),
              },
              cancellation_source.get_cancellation());
      std::unique_ptr<elder_terms::SshChannelConnection> connection =
          std::move(co_await connect_promise);
      co_await connection->resize_async(
          server_options.resized_columns, server_options.resized_rows,
          cancellation_source.get_cancellation());
      co_await connection->send_break_async(
          500, cancellation_source.get_cancellation());
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
      std::shared_ptr<elder_terms::AuthenticatedSshTransport> transport =
          connection->authenticated_transport();
      expect_true(transport != nullptr,
                  "SSH channel should expose its authenticated transport");
      expect_true(
          transport->endpoint_settings().username ==
              client_case.prompted_username,
          "SSH transport should expose the prompted username");
      connection->close();
      connection.reset();
      expect_true(
          co_await transport->is_connected_async(
              cancellation_source.get_cancellation()),
          "authenticated transport should outlive its shell channel");
      expect_true(
          transport.use_count() == 1,
          "authenticated transport retained unexpected owners: " +
              std::to_string(transport.use_count()));
      if (!server_options.sftp_root.empty()) {
        co_await exercise_sftp_client_async(
            transport, cancellation_source.get_cancellation());
        expect_true(
            transport.use_count() == 1,
            "SFTP client retained the authenticated transport after close");
      }
      const unsigned char release = 1;
      expect_true(write_all_fd(server.release_fd, &release, sizeof(release)),
                  "failed to release the SSH test server");
      (void)::close(server.release_fd);
      server.release_fd = -1;
      transport.reset();
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
  expect_true(::setenv("HOME", root.c_str(), 1) == 0,
              "failed to isolate the SSH client home directory");
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
          .config_file = {},
          .authentication_answer = {},
          .expected_prompts = {
              elder_terms::SshUserPromptKind::username,
              elder_terms::SshUserPromptKind::host_key,
          },
      });

  options.auth_mode = ServerAuthMode::password;
  options.sftp_root = root / "sftp-root";
  std::filesystem::create_directories(options.sftp_root);
  {
    std::ofstream file(options.sftp_root / "server.txt",
                       std::ios::binary);
    file << "SFTP server payload";
  }
  std::filesystem::create_symlink(
      "server.txt", options.sftp_root / "server-link");
  run_client_case(
      options,
      ClientCase{
          .auth_mode = ServerAuthMode::password,
          .identity_file = {},
          .known_hosts_file = known_hosts_file,
          .config_file = {},
          .authentication_answer = options.password,
          .expected_prompts = {
              elder_terms::SshUserPromptKind::username,
              elder_terms::SshUserPromptKind::host_key,
              elder_terms::SshUserPromptKind::password,
          },
      });

  options.auth_mode = ServerAuthMode::public_key;
  options.sftp_root.clear();
  options.authorized_key_path = plain_public_key;
  run_client_case(
      options,
      ClientCase{
          .auth_mode = ServerAuthMode::public_key,
          .identity_file = plain_private_key,
          .known_hosts_file = known_hosts_file,
          .config_file = {},
          .authentication_answer = {},
          .expected_prompts = {
              elder_terms::SshUserPromptKind::username,
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
          .config_file = {},
          .authentication_answer = "key-passphrase",
          .expected_prompts = {
              elder_terms::SshUserPromptKind::username,
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
          .config_file = {},
          .authentication_answer = options.password,
          .expected_prompts = {
              elder_terms::SshUserPromptKind::username,
              elder_terms::SshUserPromptKind::host_key,
              elder_terms::SshUserPromptKind::keyboard_interactive,
          },
      });

  options.auth_mode = ServerAuthMode::none;
  options.username = "selected-config-user";
  {
    std::ofstream config(root / ".ssh" / "config");
    config << "Host 127.0.0.1\n"
              "  User config-user\n";
  }
  run_client_case(
      options,
      ClientCase{
          .auth_mode = ServerAuthMode::none,
          .setting_username = {},
          .expected_initial_username = "config-user",
          .prompted_username = options.username,
          .identity_file = {},
          .known_hosts_file = known_hosts_file,
          .config_file = root / ".ssh" / "config",
          .authentication_answer = {},
          .expected_prompts = {
              elder_terms::SshUserPromptKind::username,
              elder_terms::SshUserPromptKind::host_key,
          },
      });

  std::filesystem::remove(root / ".ssh" / "config");
  const char *current_username = g_get_user_name();
  expect_true(current_username != nullptr && current_username[0] != '\0',
              "current operating-system username is unavailable");
  options.username = current_username;
  run_client_case(
      options,
      ClientCase{
          .auth_mode = ServerAuthMode::none,
          .setting_username = {},
          .expected_initial_username = current_username,
          .prompted_username = current_username,
          .identity_file = {},
          .known_hosts_file = known_hosts_file,
          .config_file = {},
          .authentication_answer = {},
          .expected_prompts = {
              elder_terms::SshUserPromptKind::username,
              elder_terms::SshUserPromptKind::host_key,
          },
      });

  expect_true(line_count(known_hosts_file) == 7,
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

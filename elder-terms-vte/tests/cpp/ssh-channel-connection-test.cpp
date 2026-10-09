#include "../../src/terminal-sessions/ssh-session/ssh-channel-connection.h"
#include "../../src/terminal-sessions/ssh-session/ssh-proxy.h"
#include "../../src/terminal-sessions/ssh-session/ssh-socks-proxy.h"
#include "../../src/terminal-sessions/telnet-session/telnet-protocol.h"
#include "../../src/file-transfer/file-hash.h"
#include "../../src/sftp/sftp-client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
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
#include <iterator>
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
  std::filesystem::path host_public_key_path;
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
  std::string expected_exec_command;
  bool abrupt_disconnect = false;
  std::string forward_host{};
  int forward_port = 23;
  bool reject_forward = false;
  int forward_target_port = 0;
  bool sftp_only = false;
  int window_gate_fd = -1;
  bool forward_open_marker = false;
};

struct ServerState {
  ServerOptions options;
  ssh_key authorized_key = nullptr;
  ssh_channel channel = nullptr;
  ssh_channel sftp_channel = nullptr;
  ssh_channel exec_channel = nullptr;
  ssh_channel_callbacks_struct channel_callbacks{};
  ssh_channel_callbacks_struct sftp_channel_callbacks{};
  ssh_channel_callbacks_struct exec_channel_callbacks{};
  ssh_event event = nullptr;
  pid_t sftp_server_pid = -1;
  int sftp_server_input_fd = -1;
  int sftp_server_output_fd = -1;
  int forward_fd = -1;
  bool sftp_requested = false;
  bool sftp_started = false;
  bool exec_requested = false;
  bool exec_response_sent = false;
  bool forward_requested = false;
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
  bool window_marker_pending = false;
  std::string username;
  std::string password;
  std::string keyboard_interactive_answer;
  std::string terminal_type;
  int columns = 0;
  int rows = 0;
  int resized_columns = 0;
  int resized_rows = 0;
  std::string payload;
  std::string exec_command;
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
  std::filesystem::path identity_file = {};
  std::filesystem::path known_hosts_file = {};
  std::filesystem::path config_file = {};
  std::filesystem::path conflicting_host_public_key = {};
  std::filesystem::path conflicting_known_hosts_file = {};
  std::string authentication_answer = {};
  std::string expected_host_key_command_marker = {};
  std::string expected_failure = {};
  bool request_host_key_reset = false;
  bool expected_host_key_reset_available = false;
  bool preserve_protected_host_markers = false;
  bool remote_disconnect = false;
  std::vector<elder_terms::SshUserPromptKind> expected_prompts = {};
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

// Arrange for a read to consume WINDOW_ADJUST before the blocked write returns
// to its caller. The pipe orders the peer; no scheduling delay is assumed.
static int window_gate_fd = -1;
static bool restored_window_observed = false;
static bool consume_forward_open_reply = false;
static bool forward_open_reply_observed = false;

extern "C" int __real_ssh_channel_open_forward(ssh_channel, const char *, int, const char *, int);
extern "C" int __wrap_ssh_channel_open_forward(ssh_channel channel, const char *host, int port,
                                               const char *source, int source_port) {
  const auto result = __real_ssh_channel_open_forward(channel, host, port, source, source_port);
  if (consume_forward_open_reply && !forward_open_reply_observed && result == SSH_AGAIN) {
    expect_true(ssh_channel_poll_timeout(channel, 30000, 0) == 1, "receive the channel-open marker");
    char marker = 0;
    expect_true(ssh_channel_read_nonblocking(channel, &marker, 1, 0) == 1 && marker == 'o',
                "consume the marker after channel-open confirmation");
    expect_true(ssh_channel_poll_timeout(channel, 0, 0) == 0, "drain readiness after the open marker");
    expect_true(ssh_channel_is_open(channel) != 0, "read must process channel-open confirmation");
    expect_true((ssh_get_poll_flags(ssh_channel_get_session(channel)) & SSH_WRITE_PENDING) == 0,
                "channel-open completion must not depend on socket writability");
    forward_open_reply_observed = true;
  }
  return result;
}

extern "C" int __real_ssh_channel_write(ssh_channel, const void *, std::uint32_t);
extern "C" int __wrap_ssh_channel_write(ssh_channel channel, const void *data, std::uint32_t size) {
  const auto result = __real_ssh_channel_write(channel, data, size);
  if (window_gate_fd >= 0 && !restored_window_observed && result == 0) {
    expect_true(ssh_channel_window_size(channel) == 0, "write must exhaust the remote window");
    const char release = 'w';
    expect_true(write_all_fd(window_gate_fd, &release, 1), "release the peer receive window");
    expect_true(ssh_channel_poll_timeout(channel, 30000, 1) == 1, "receive the window-update marker");
    char marker = 0;
    expect_true(ssh_channel_read_nonblocking(channel, &marker, 1, 1) == 1 && marker == 'w',
                "consume the marker after WINDOW_ADJUST");
    expect_true(ssh_channel_window_size(channel) > 0, "read must restore the remote send window");
    expect_true(ssh_channel_poll_timeout(channel, 0, 1) == 0, "drain readiness after the window marker");
    expect_true((ssh_get_poll_flags(ssh_channel_get_session(channel)) & SSH_WRITE_PENDING) == 0,
                "window recovery must not depend on socket writability");
    restored_window_observed = true;
  }
  return result;
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

static std::string
openssh_random_art(const std::filesystem::path &public_key_path) {
  const std::string public_key = public_key_path.string();
  gchar *standard_output = nullptr;
  gchar *standard_error = nullptr;
  gint wait_status = 0;
  GError *error = nullptr;
  gchar *arguments[] = {
      const_cast<gchar *>("/usr/bin/ssh-keygen"),
      const_cast<gchar *>("-l"),
      const_cast<gchar *>("-v"),
      const_cast<gchar *>("-E"),
      const_cast<gchar *>("sha256"),
      const_cast<gchar *>("-f"),
      const_cast<gchar *>(public_key.c_str()),
      nullptr,
  };
  const gboolean spawned = g_spawn_sync(
      nullptr, arguments, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr,
      &standard_output, &standard_error, &wait_status, &error);
  const std::string failure =
      error != nullptr && error->message != nullptr
          ? error->message
          : standard_error != nullptr ? standard_error : "unknown error";
  g_clear_error(&error);
  g_free(standard_error);
  if (spawned == FALSE ||
      g_spawn_check_wait_status(wait_status, &error) == FALSE) {
    const std::string status_failure =
        error != nullptr && error->message != nullptr ? error->message
                                                      : failure;
    g_clear_error(&error);
    g_free(standard_output);
    throw std::runtime_error("failed to obtain OpenSSH random art: " +
                             status_failure);
  }

  std::string output = standard_output == nullptr ? "" : standard_output;
  g_free(standard_output);
  const std::size_t art_start = output.find("\n+");
  expect_true(art_start != std::string::npos,
              "OpenSSH did not print host-key random art");
  output.erase(0, art_start + 1);
  while (!output.empty() &&
         (output.back() == '\n' || output.back() == '\r')) {
    output.pop_back();
  }
  return output;
}

static std::string read_text_file(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

static std::string public_key_material(const std::string &public_key) {
  const std::size_t key_data_start = public_key.find_first_of(" \t");
  expect_true(key_data_start != std::string::npos,
              "public key type was missing");
  const std::size_t key_data_end =
      public_key.find_first_of(" \t\r\n", key_data_start + 1);
  return public_key.substr(0, key_data_end);
}

static std::string known_hosts_target(const std::string &address, int port) {
  return port == 22 ? address
                    : "[" + address + "]:" + std::to_string(port);
}

static void install_conflicting_host_key(
    const std::filesystem::path &known_hosts_file,
    const std::filesystem::path &public_key_file,
    const std::string &address, int port) {
  const std::string public_key = read_text_file(public_key_file);
  expect_true(!public_key.empty(), "conflicting public key was empty");
  std::ofstream output(known_hosts_file, std::ios::app);
  output << known_hosts_target(address, port) << ' ' << public_key;
  expect_true(output.good(), "failed to install conflicting host key");
}

static void install_protected_host_markers(
    const std::filesystem::path &known_hosts_file,
    const std::filesystem::path &public_key_file,
    const std::string &address, int port) {
  const std::string public_key = read_text_file(public_key_file);
  std::ofstream output(known_hosts_file, std::ios::app);
  output << "@cert-authority " << known_hosts_target(address, port) << ' '
         << public_key;
  output << "@revoked " << known_hosts_target(address, port) << ' '
         << public_key;
  expect_true(output.good(), "failed to install protected host-key markers");
}

static std::string known_hosts_entries(
    const std::filesystem::path &known_hosts_file,
    const std::string &target) {
  const std::string file = known_hosts_file.string();
  gchar *standard_output = nullptr;
  gchar *standard_error = nullptr;
  gint wait_status = 0;
  GError *error = nullptr;
  gchar *arguments[] = {
      const_cast<gchar *>("/usr/bin/ssh-keygen"),
      const_cast<gchar *>("-F"),
      const_cast<gchar *>(target.c_str()),
      const_cast<gchar *>("-f"),
      const_cast<gchar *>(file.c_str()),
      nullptr,
  };
  const gboolean spawned = g_spawn_sync(
      nullptr, arguments, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr,
      &standard_output, &standard_error, &wait_status, &error);
  const std::string failure =
      error != nullptr && error->message != nullptr
          ? error->message
          : standard_error != nullptr ? standard_error : "unknown error";
  g_clear_error(&error);
  g_free(standard_error);
  if (spawned == FALSE ||
      g_spawn_check_wait_status(wait_status, &error) == FALSE) {
    const std::string status_failure =
        error != nullptr && error->message != nullptr ? error->message
                                                      : failure;
    g_clear_error(&error);
    g_free(standard_output);
    throw std::runtime_error("failed to find OpenSSH known_hosts entry: " +
                             status_failure);
  }
  const std::string output =
      standard_output == nullptr ? std::string() : standard_output;
  g_free(standard_output);
  return output;
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
  if (state->options.window_gate_fd >= 0) {
    char release = 0;
    if (!read_all_fd(state->options.window_gate_fd, &release, 1) || release != 'w') return -1;
    (void)::close(state->options.window_gate_fd);
    state->options.window_gate_fd = -1;
    state->window_marker_pending = true;
  }
  state->payload.append(static_cast<const char *>(data), size);
  const int written = ssh_channel_write(channel, data, size);
  state->payload_echoed = written == static_cast<int>(size);
  if (state->payload_echoed && !state->options.abrupt_disconnect &&
      (state->options.forward_host.empty() || state->payload.size() >= state->options.payload.size())) {
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

static int on_exec_request(ssh_session, ssh_channel channel,
                           const char *command, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  state->exec_requested = true;
  state->exec_command = command == nullptr ? "" : command;
  if (state->options.expected_exec_command.empty() ||
      state->exec_command != state->options.expected_exec_command) {
    return 1;
  }

  const std::string output =
      "38adb9eb75e100197e43a3626662315a  -\n"
      "543d35939ab278c16ab479a9e39a1580ebc49413  -\n"
      "7588cf1ef3681d5379f11b15bd4bf803d9a7fd22dc77cd6c15242c5265d196ff  -\n";
  const int written = ssh_channel_write(
      channel, output.data(), static_cast<std::uint32_t>(output.size()));
  state->exec_response_sent =
      written == static_cast<int>(output.size()) &&
      ssh_channel_request_send_exit_status(channel, 0) == SSH_OK &&
      ssh_channel_send_eof(channel) == SSH_OK;
  return state->exec_response_sent ? 0 : 1;
}

static ssh_channel on_channel_open(ssh_session session, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  ssh_channel channel = ssh_channel_new(session);
  if (channel == nullptr) {
    return nullptr;
  }
  if (state->channel != nullptr && state->sftp_channel != nullptr) {
    state->exec_channel = channel;
    state->exec_channel_callbacks.userdata = state;
    state->exec_channel_callbacks.channel_exec_request_function =
        on_exec_request;
    ssh_callbacks_init(&state->exec_channel_callbacks);
    if (ssh_set_channel_callbacks(
            channel, &state->exec_channel_callbacks) != SSH_OK) {
      ssh_channel_free(channel);
      state->exec_channel = nullptr;
      return nullptr;
    }
    return channel;
  }
  if (state->channel != nullptr || state->options.sftp_only) {
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

static int on_forward_peer_data(socket_t fd, int, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  std::array<unsigned char, 32768> buffer{};
  const auto count = ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
  if (count > 0) {
    std::size_t offset = 0;
    while (offset < static_cast<std::size_t>(count)) {
      const auto sent = ssh_channel_write(state->channel, buffer.data() + offset, count - offset);
      if (sent <= 0) return -1;
      offset += sent;
    }
  } else if (!count) {
    (void)ssh_channel_send_eof(state->channel);
    ssh_event_remove_fd(state->event, fd);
  }
  return 0;
}

static int on_forward_channel_data(ssh_session, ssh_channel, void *data,
    std::uint32_t size, int, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  state->payload.append(static_cast<const char *>(data), size);
  return write_all_fd(state->forward_fd, data, size) ? static_cast<int>(size) : -1;
}

static void on_forward_channel_eof(ssh_session, ssh_channel, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  (void)::shutdown(state->forward_fd, SHUT_WR);
}

static int on_forward_message(ssh_session session, ssh_message message, void *userdata) {
  auto *state = static_cast<ServerState *>(userdata);
  if (ssh_message_type(message) != SSH_REQUEST_CHANNEL_OPEN ||
      ssh_message_subtype(message) != SSH_CHANNEL_DIRECT_TCPIP) return 1;
  const auto *destination = ssh_message_channel_request_open_destination(message);
  state->forward_requested = destination && state->options.forward_host == destination &&
      ssh_message_channel_request_open_destination_port(message) == state->options.forward_port;
  if (!state->forward_requested || state->options.reject_forward) return 1;
  state->channel = ssh_message_channel_request_open_reply_accept(message);
  if (!state->channel) return 1;
  state->channel_callbacks.userdata = state;
  state->channel_callbacks.channel_data_function = on_channel_data;
  if (state->options.forward_target_port) {
    state->forward_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(state->options.forward_target_port);
    if (state->forward_fd < 0 || ::connect(state->forward_fd,
          reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) return 1;
    state->channel_callbacks.channel_data_function = on_forward_channel_data;
    state->channel_callbacks.channel_eof_function = on_forward_channel_eof;
    if (ssh_event_add_fd(state->event, state->forward_fd, POLLIN, on_forward_peer_data, state) != SSH_OK) return 1;
  }
  ssh_callbacks_init(&state->channel_callbacks);
  (void)session;
  if (ssh_set_channel_callbacks(state->channel, &state->channel_callbacks) != SSH_OK) return 1;
  if (state->options.forward_open_marker) {
    const char marker = 'o';
    if (ssh_channel_write(state->channel, &marker, 1) != 1) return 1;
  }
  return 0;
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
  if (state.options.sftp_only)
    return state.sftp_started && state.sftp_requested && !state.shell_requested &&
        state.username == state.options.username ? 0 : 39;
  if (state.options.forward_target_port)
    return state.forward_requested && !state.payload.empty() &&
        (state.options.auth_mode != ServerAuthMode::public_key || state.public_key_accepted) ? 0 : 38;
  if (state.options.reject_forward)
    return state.forward_requested && state.payload.empty() ? 0 : 37;
  if (!state.options.forward_host.empty())
    return state.forward_requested && state.payload == state.options.payload && state.payload_echoed ? 0 : 36;
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
  if (!state.options.expected_exec_command.empty() &&
      (!state.exec_requested || !state.exec_response_sent ||
       state.exec_command != state.options.expected_exec_command)) {
    return 35;
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
  if (!options.forward_host.empty()) ssh_set_message_callback(session, on_forward_message, &state);
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
    if (state.window_marker_pending) {
      // libssh grows the receive window after the data callback returns.
      // Send a marker after that packet so the client can observe its receipt.
      state.window_marker_pending = false;
      const char marker = 'w';
      if (ssh_channel_write_stderr(state.channel, &marker, 1) != 1) break;
    }
  }

  const int validation_result =
      (state.release_requested || !options.forward_host.empty()) ? validate_server_state(state) : 24;
  if (options.abrupt_disconnect) {
    (void)::shutdown(ssh_get_fd(session), SHUT_RDWR);
  }
  stop_sftp_server(&state);
  if (state.forward_fd >= 0) {
    ssh_event_remove_fd(event, state.forward_fd);
    close_server_fd(&state.forward_fd);
  }
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
  if (state.exec_channel != nullptr) {
    ssh_channel_free(state.exec_channel);
    state.exec_channel = nullptr;
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

static std::string prompt_sequence(const std::vector<elder_terms::SshUserPromptKind> &prompts) {
  std::string result;
  for (const auto prompt : prompts) {
    if (!result.empty()) result += ',';
    result += std::to_string(static_cast<int>(prompt));
  }
  return result;
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
    std::shared_ptr<elder_terms::RemoteFileClient> client,
    cardio::cancellation cancellation) {
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
      std::move(co_await client->open_write_async("uploaded.txt", uploaded_content.size(), 0600,
                                                  cancellation));
  co_await writer->write_all_async(byte_span(uploaded_content),
                                   cancellation);
  co_await writer->close_async(cancellation);
  writer.reset();
  auto updating = client->set_attributes_async(
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
  co_await updating;
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

static int run_client_case(const ServerOptions &server_options,
                           const ClientCase &client_case) {
  auto effective_options = server_options;
  effective_options.abrupt_disconnect = client_case.remote_disconnect;
  ChildServer server = start_server(effective_options);
  std::cout << "SSH case " << auth_mode_name(client_case.auth_mode) << " port=" << server.port << std::endl;
  if (!client_case.conflicting_host_public_key.empty()) {
    const std::filesystem::path target_file =
        client_case.conflicting_known_hosts_file.empty()
            ? client_case.known_hosts_file
            : client_case.conflicting_known_hosts_file;
    install_conflicting_host_key(target_file,
                                 client_case.conflicting_host_public_key,
                                 "127.0.0.1", server.port);
    if (client_case.preserve_protected_host_markers) {
      install_protected_host_markers(
          target_file, client_case.conflicting_host_public_key,
          "127.0.0.1", server.port);
    }
  }
  const std::string expected_random_art =
      openssh_random_art(server_options.host_public_key_path);
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
  auto task_body = [&]() -> cardio::promise<void> {
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
               &server,
               &client_case,
               &expected_random_art](const elder_terms::SshUserPrompt &prompt,
                                     cardio::cancellation cancellation)
              -> cardio::promise<elder_terms::SshUserPromptResponse> {
            cancellation.throw_if_cancellation_requested();
            prompts.push_back(prompt.kind);
            elder_terms::SshUserPromptResponse response{
                .accepted = true,
                .text = {},
                .reset_host_key = false,
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
            } else {
              expect_true(prompt.monospace_message == expected_random_art,
                          "SSH host-key prompt random art did not match "
                          "OpenSSH\nExpected:\n" +
                              expected_random_art + "\nActual:\n" +
                              prompt.monospace_message);
              expect_true(
                  prompt.host_key_reset_available ==
                      client_case.expected_host_key_reset_available,
                  "SSH host-key reset availability did not match");
              if (!client_case.expected_host_key_command_marker.empty()) {
                const std::filesystem::path expected_known_hosts_file =
                    client_case.conflicting_known_hosts_file.empty()
                        ? client_case.known_hosts_file
                        : client_case.conflicting_known_hosts_file;
                expect_true(
                    prompt.message.find(
                        client_case.expected_host_key_command_marker) !=
                        std::string::npos &&
                        prompt.message.find(known_hosts_target(
                            "127.0.0.1", server.port)) !=
                            std::string::npos &&
                        prompt.message.find(
                            expected_known_hosts_file.string()) !=
                            std::string::npos,
                    "SSH host-key reset command was not displayed");
              }
              if (client_case.request_host_key_reset) {
                response.accepted = false;
                response.reset_host_key = true;
              }
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
      if (client_case.remote_disconnect) {
        const unsigned char release = 1;
        expect_true(write_all_fd(server.release_fd, &release, sizeof(release)),
                    "failed to release the SSH test server");
        (void)::close(server.release_fd);
        server.release_fd = -1;
        expect_true(co_await connection->read_async(buffer,
                        cancellation_source.get_cancellation()) == 0,
                    "Remote SSH disconnect must be reported as EOF");
      }
      connection->close();
      connection.reset();
      if (!client_case.remote_disconnect) {
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
              co_await elder_terms::open_sftp_client_async(transport, cancellation_source.get_cancellation()),
              cancellation_source.get_cancellation());
          expect_true(
              transport.use_count() == 1,
              "SFTP client retained the authenticated transport after close");
        }
        if (!server_options.expected_exec_command.empty()) {
          const elder_terms::FileHashes hashes =
              co_await elder_terms::calculate_ssh_file_hashes_async(
                  transport, "/server's payload.txt",
                  cancellation_source.get_cancellation());
          expect_true(
              hashes.md5 == "38adb9eb75e100197e43a3626662315a" &&
                  hashes.sha1 ==
                      "543d35939ab278c16ab479a9e39a1580ebc49413" &&
                  hashes.sha256 ==
                      "7588cf1ef3681d5379f11b15bd4bf803d9a7fd22dc77cd6c15242c5265d196ff",
              "SSH file hash values did not match the server response");
        }
        const unsigned char release = 1;
        expect_true(write_all_fd(server.release_fd, &release, sizeof(release)),
                    "failed to release the SSH test server");
        (void)::close(server.release_fd);
        server.release_fd = -1;
      }
      transport.reset();
    } catch (...) {
      async_error = std::current_exception();
    }
    dispatcher_group.shutdown();
  };
  auto task = task_body();

  dispatcher.park();
  task.unsafe_result();
  if (timeout.source_id != 0) {
    g_source_remove(timeout.source_id);
    timeout.source_id = 0;
  }
  const int server_result = wait_for_server(&server);
  if (!client_case.expected_failure.empty()) {
    expect_true(async_error != nullptr,
                "SSH connection unexpectedly succeeded");
    try {
      std::rethrow_exception(async_error);
    } catch (const std::exception &error) {
      expect_true(
          std::string(error.what()).find(client_case.expected_failure) !=
              std::string::npos,
          "SSH failure did not describe the rejected host key: " +
              std::string(error.what()));
    }
    expect_true(server_result != 0,
                "SSH server unexpectedly authenticated a rejected host key");
    expect_true(prompts == client_case.expected_prompts,
                "SSH rejected host-key prompt sequence did not match");
    expect_true(
        phases ==
            std::vector<elder_terms::TerminalSessionConnectionPhase>{
                elder_terms::TerminalSessionConnectionPhase::verifying_host,
            },
        "SSH rejected host-key phases did not match");
    return server.port;
  }
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
              "SSH user prompt sequence did not match: mode=" +
                  std::string(auth_mode_name(client_case.auth_mode)) +
                  " port=" + std::to_string(server.port) +
                  " expected=[" + prompt_sequence(client_case.expected_prompts) +
                  "] actual=[" + prompt_sequence(prompts) + "]");
  expect_true(
      phases ==
          std::vector<elder_terms::TerminalSessionConnectionPhase>{
              elder_terms::TerminalSessionConnectionPhase::verifying_host,
              elder_terms::TerminalSessionConnectionPhase::authenticating,
              elder_terms::TerminalSessionConnectionPhase::opening_shell,
          },
      "SSH connection phases did not match");
  return server.port;
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

static std::size_t open_socket_count() {
  std::size_t count = 0;
  for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
    std::error_code error;
    const auto target = std::filesystem::read_symlink(entry.path(), error).string();
    if (!error && target.starts_with("socket:")) ++count;
  }
  return count;
}

static void test_forward_readiness(const ServerOptions &options, const std::filesystem::path &known_hosts,
                                    bool test_open_reply) {
  int gate[2] = {-1, -1};
  if (!test_open_reply) expect_true(::pipe(gate) == 0, "create the receive-window gate");
  auto server_options = options;
  server_options.forward_host = "window.proxy-test.invalid";
  server_options.window_gate_fd = gate[0];
  server_options.forward_open_marker = test_open_reply;
  server_options.payload.resize(65537);
  for (std::size_t index = 0; index < server_options.payload.size(); ++index)
    server_options.payload[index] = static_cast<char>(index % 256);
  auto server = start_server(server_options);
  if (gate[0] >= 0) ::close(gate[0]);
  window_gate_fd = gate[1];
  restored_window_observed = false;
  consume_forward_open_reply = test_open_reply;
  forward_open_reply_observed = false;
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  std::exception_ptr failure;
  auto body = [&]() -> cardio::promise<void> {
    try {
      elder_terms::TerminalSessionCallbacks callbacks{};
      callbacks.ssh_prompt = [](const auto &prompt, cardio::cancellation) -> cardio::promise<elder_terms::SshUserPromptResponse> {
        co_return elder_terms::SshUserPromptResponse{.accepted = true, .text = prompt.initial_text, .reset_host_key = false};
      };
      const elder_terms::SshEndpointSettings endpoint{
          .address = "127.0.0.1", .port = server.port,
          .username = options.username, .identity_file = {}};
      auto connecting = elder_terms::AuthenticatedSshTransport::connect_async(endpoint, callbacks,
          {.known_hosts_file = known_hosts.string(), .config_file = {}}, {});
      auto transport = co_await connecting;
      auto opening = elder_terms::SshChannelConnection::open_forward_async(
          transport, server_options.forward_host, server_options.forward_port, {});
      auto channel = std::move(co_await opening);
      co_await channel->write_all_async(bytes(server_options.payload), {});
      expect_true(test_open_reply ? forward_open_reply_observed : restored_window_observed,
                  "exercise readiness consumed by a read before the caller waits");
      std::string received;
      std::array<unsigned char, 16384> buffer{};
      for (;;) {
        const auto count = co_await channel->read_async(buffer, {});
        if (!count) break;
        received.append(reinterpret_cast<const char *>(buffer.data()), count);
      }
      expect_true(received == server_options.payload, "forwarding must resume without losing bytes after buffered SSH replies");
    } catch (...) { failure = std::current_exception(); }
    group.shutdown();
  };
  auto task = body();
  dispatcher.park();
  task.unsafe_result();
  window_gate_fd = -1;
  consume_forward_open_reply = false;
  if (gate[1] >= 0) ::close(gate[1]);
  const auto result = wait_for_server(&server);
  if (failure) std::rethrow_exception(failure);
  expect_true(result == 0, "server must receive the complete payload after readiness recovery");
}

static void test_proxy_forwarding(const ServerOptions &options, const std::filesystem::path &known_hosts, bool socks) {
  auto gateway_options = options;
  gateway_options.forward_host = "internal.proxy-test.invalid";
  elder_terms::TelnetProtocol protocol("xterm");
  gateway_options.payload.clear();
  for (const auto &request : protocol.encode_enable_binary())
    gateway_options.payload.append(reinterpret_cast<const char *>(request.data()), request.size());
  gateway_options.payload += options.payload;
  auto server = start_server(gateway_options);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  std::exception_ptr failure;
  auto body = [&]() -> cardio::promise<void> {
    try {
      const auto initial_sockets = open_socket_count();
      elder_terms::SshProxySettings proxy;
      proxy.enabled = true;
      proxy.endpoint = {.address = "127.0.0.1", .port = server.port,
                        .username = options.username, .identity_file = {}};
      elder_terms::TerminalSessionCallbacks callbacks{};
      callbacks.ssh_prompt = [](const auto &prompt, cardio::cancellation) -> cardio::promise<elder_terms::SshUserPromptResponse> {
        expect_true(prompt.title.find("SSH proxy") != std::string::npos, "gateway prompts must identify SSH proxy");
        co_return elder_terms::SshUserPromptResponse{.accepted = true, .text = prompt.initial_text, .reset_host_key = false};
      };
      std::shared_ptr<elder_terms::SshProxyConnection> connection;
      std::shared_ptr<elder_terms::SshSocksProxy> adapter;
      cardio::io_uring io(64);
      int fd = -1;
      std::string socket_path;
      if (socks) {
        auto opening = elder_terms::open_ssh_socks_proxy_async(proxy, gateway_options.forward_host,
            callbacks, {.known_hosts_file = known_hosts.string(), .config_file = {}}, {});
        adapter = std::move(co_await opening);
        const auto url = elder_terms::ssh_socks_proxy_url(adapter);
        expect_true(url.starts_with("socks5h://localhost/"), "SOCKS endpoint must use a private Unix socket");
        socket_path = url.substr(std::string("socks5h://localhost").size());
        struct stat info{};
        expect_true(::stat(std::filesystem::path(socket_path).parent_path().c_str(), &info) == 0 &&
            (info.st_mode & 0777) == 0700, "SOCKS directory must be private to this user");
        fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::copy(socket_path.begin(), socket_path.end(), address.sun_path);
        expect_true(::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "connect private SOCKS endpoint");
        std::string greeting("\x05\x01\x00", 3);
        co_await cardio::io_urings::write(io, fd, byte_span(greeting));
        std::array<std::byte, 2> method{};
        expect_true(co_await cardio::io_urings::read(io, fd, std::span(method)) == 2 && method[1] == std::byte{0}, "SOCKS method negotiation");
        std::string request("\x05\x01\x00\x03", 4);
        request.push_back(static_cast<char>(gateway_options.forward_host.size()));
        request += gateway_options.forward_host;
        request.append("\x00\x17", 2);
        // Send individual bytes to exercise fragmented SOCKS negotiation.
        for (const auto &byte : request) {
          const auto one = std::span<const std::byte>(reinterpret_cast<const std::byte *>(&byte), 1);
          co_await cardio::io_urings::write(io, fd, one);
        }
        std::array<std::byte, 10> response{};
        std::size_t received = 0;
        while (received < response.size()) received += co_await cardio::io_urings::read(io, fd, std::span(response).subspan(received));
        expect_true(response[1] == std::byte{0}, "SOCKS CONNECT must reach the gateway");
      } else {
        auto connecting = elder_terms::connect_ssh_proxy_async(proxy, gateway_options.forward_host, 23,
            callbacks, {.known_hosts_file = known_hosts.string(), .config_file = {}}, {});
        connection = std::move(co_await connecting);
        fd = elder_terms::ssh_proxy_connection_fd(connection);
      }
      const auto data = byte_span(gateway_options.payload);
      std::size_t sent = 0;
      while (sent < data.size()) sent += co_await cardio::io_urings::write(io, fd, data.subspan(sent));
      std::string echoed;
      std::array<std::byte, 128> buffer{};
      for (;;) {
        const auto count = co_await cardio::io_urings::read(io, fd, std::span<std::byte>(buffer));
        if (!count) break;
        echoed.append(reinterpret_cast<const char *>(buffer.data()), count);
      }
      expect_true(echoed == gateway_options.payload, "gateway must transport payload and remote EOF");
      const auto parsed = protocol.receive(std::span<const unsigned char>(
          reinterpret_cast<const unsigned char *>(echoed.data()), echoed.size()));
      expect_true(protocol.is_binary_enabled() && std::string(parsed.terminal_data.begin(), parsed.terminal_data.end()) == options.payload,
                  "TELNET negotiation and terminal data must survive forwarding");
      co_await elder_terms::stop_ssh_proxy_connection_async(connection);
      connection.reset();
      if (adapter) {
        ::close(fd);
        co_await elder_terms::stop_ssh_socks_proxy_async(adapter);
        adapter.reset();
        expect_true(!std::filesystem::exists(socket_path), "stopping SOCKS must remove the private endpoint");
      }
      expect_true(open_socket_count() == initial_sockets, "stopping proxy must release gateway and bridge sockets");
    } catch (...) { failure = std::current_exception(); }
    group.shutdown();
  };
  auto task = body();
  dispatcher.park();
  expect_true(task.is_ready(), "proxy task must complete");
  const auto result = wait_for_server(&server);
  if (failure) std::rethrow_exception(failure);
  expect_true(result == 0, "gateway must receive the original host and port");
}

static void test_proxy_rejections(const ServerOptions &options, const std::filesystem::path &known_hosts) {
  auto gateway_options = options;
  gateway_options.forward_host = "127.0.0.1";
  gateway_options.reject_forward = true;
  auto server = start_server(gateway_options);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  std::exception_ptr failure;
  auto body = [&]() -> cardio::promise<void> {
    try {
      elder_terms::SshProxySettings proxy;
      proxy.enabled = true;
      proxy.endpoint = {.address = "127.0.0.1", .port = server.port,
                        .username = options.username, .identity_file = {}};
      elder_terms::TerminalSessionCallbacks callbacks{};
      bool prompted = false;
      bool accept = false;
      callbacks.ssh_prompt = [&](const auto &prompt, cardio::cancellation) -> cardio::promise<elder_terms::SshUserPromptResponse> {
        prompted = true;
        co_return elder_terms::SshUserPromptResponse{.accepted = accept, .text = prompt.initial_text};
      };
      elder_terms::AuthenticatedSshTransportOptions overrides{.known_hosts_file = known_hosts.string(), .config_file = {}};
      proxy.validation_errors = {"invalid proxy setting"};
      bool rejected = false;
      try { (void)co_await elder_terms::connect_ssh_proxy_async(proxy, "127.0.0.1", 23, callbacks, overrides, {}); }
      catch (const std::invalid_argument &) { rejected = true; }
      expect_true(rejected && !prompted, "invalid proxy must fail before authentication or connecting");
      proxy.validation_errors.clear();
      cardio::cancellation_source canceled;
      canceled.cancel();
      rejected = false;
      try { (void)co_await elder_terms::connect_ssh_proxy_async(proxy, "127.0.0.1", 23, callbacks, overrides, canceled.get_cancellation()); }
      catch (const cardio::canceled_exception &) { rejected = true; }
      expect_true(rejected && !prompted, "canceled connection must not fall back to direct TCP");
      rejected = false;
      try { (void)co_await elder_terms::connect_ssh_proxy_async(proxy, "127.0.0.1", 23, callbacks, overrides, {}); }
      catch (const std::exception &) { rejected = true; }
      expect_true(rejected && prompted, "declined authentication must reject the connection");
      accept = true;
      rejected = false;
      std::string rejection;
      try { (void)co_await elder_terms::connect_ssh_proxy_async(proxy, "127.0.0.1", 23, callbacks, overrides, {}); }
      catch (const std::exception &error) { rejection = error.what(); rejected = rejection.find("SSH proxy forwarding failed") != std::string::npos; }
      expect_true(rejected, "forwarding refusal must remain a proxy failure: " + rejection);
    } catch (...) { failure = std::current_exception(); }
    group.shutdown();
  };
  auto task = body();
  dispatcher.park();
  expect_true(task.is_ready(), "proxy rejection task completed");
  const auto result = wait_for_server(&server);
  if (failure) std::rethrow_exception(failure);
  expect_true(result == 0, "gateway must observe the rejected forwarding request");
}

enum class ProxySftpMode { none, standalone, shell_first, sftp_first };

static void test_proxy_ssh(ServerOptions target_options, ServerOptions gateway_options,
    const std::filesystem::path &target_identity, const std::filesystem::path &gateway_identity,
    const std::filesystem::path &known_hosts, bool changed_keys, ProxySftpMode sftp_mode) {
  const auto initial_sockets = open_socket_count();
  target_options.sftp_only = sftp_mode == ProxySftpMode::standalone;
  auto target = start_server(target_options);
  gateway_options.forward_host = "target.proxy-test.invalid";
  gateway_options.forward_port = target.port;
  gateway_options.forward_target_port = target.port;
  gateway_options.username = "gateway-user";
  auto gateway = start_server(gateway_options);
  if (changed_keys) {
    install_conflicting_host_key(known_hosts, target_options.host_public_key_path, "127.0.0.1", gateway.port);
    install_conflicting_host_key(known_hosts, gateway_options.host_public_key_path, gateway_options.forward_host, target.port);
  }
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  std::exception_ptr failure;
  unsigned int gateway_keys = 0;
  unsigned int target_keys = 0;
  auto body = [&]() -> cardio::promise<void> {
    try {
      elder_terms::TerminalSessionCallbacks callbacks{};
      callbacks.ssh_prompt = [&](const auto &prompt, cardio::cancellation) -> cardio::promise<elder_terms::SshUserPromptResponse> {
        const bool gateway_prompt = prompt.title.find("SSH proxy") != std::string::npos;
        std::string text;
        if (prompt.kind == elder_terms::SshUserPromptKind::username) {
          text = gateway_prompt ? gateway_options.username : target_options.username;
          expect_true(prompt.initial_text == text, "gateway and destination users must remain distinct");
        } else if (prompt.kind == elder_terms::SshUserPromptKind::host_key) {
          if (gateway_prompt) ++gateway_keys; else ++target_keys;
          expect_true(prompt.message.find(gateway_prompt ? "127.0.0.1" : gateway_options.forward_host) != std::string::npos,
                      "host-key prompts must identify the original endpoint");
          expect_true(prompt.host_key_reset_available == changed_keys,
                      "changed gateway and target host keys must be checked independently");
        } else text = gateway_prompt ? "key-passphrase" : target_options.password;
        const bool reset_key = changed_keys && prompt.kind == elder_terms::SshUserPromptKind::host_key;
        co_return elder_terms::SshUserPromptResponse{.accepted = !reset_key, .text = text, .reset_host_key = reset_key};
      };
      elder_terms::SshConnectionSettings settings{
          .endpoint = {.address = gateway_options.forward_host, .port = target.port,
              .username = target_options.username, .identity_file = target_identity.string()},
          .terminal_type = target_options.terminal_type};
      elder_terms::SshChannelConnectionOptions overrides{
          .known_hosts_file = known_hosts.string(), .config_file = {},
          .proxy = {.enabled = true, .endpoint = {.address = "127.0.0.1", .port = gateway.port,
              .username = gateway_options.username, .identity_file = gateway_identity.string()}}};
      std::unique_ptr<elder_terms::SshChannelConnection> connection;
      std::shared_ptr<elder_terms::AuthenticatedSshTransport> transport;
      if (sftp_mode == ProxySftpMode::standalone) {
        auto opening = elder_terms::AuthenticatedSshTransport::connect_async(settings.endpoint, callbacks, overrides, {});
        transport = std::move(co_await opening);
      } else {
        auto opening = elder_terms::SshChannelConnection::connect_async(settings,
            target_options.columns, target_options.rows, callbacks, overrides, {});
        connection = std::move(co_await opening);
        transport = connection->authenticated_transport();
      }
      if (sftp_mode == ProxySftpMode::sftp_first) {
        auto client = co_await elder_terms::open_sftp_client_async(transport, {});
        co_await exercise_sftp_client_async(std::move(client), {});
      }
      if (connection) {
        co_await connection->resize_async(target_options.resized_columns, target_options.resized_rows, {});
        co_await connection->send_break_async(500, {});
        co_await connection->write_all_async(bytes(target_options.payload), {});
        std::string echoed;
        std::array<unsigned char, 128> buffer{};
        while (echoed.size() < target_options.payload.size()) {
          const auto count = co_await connection->read_async(buffer, {});
          if (!count) break;
          echoed.append(reinterpret_cast<const char *>(buffer.data()), count);
        }
        expect_true(echoed == target_options.payload, "SSH shell must operate on the final endpoint");
      }
      std::shared_ptr<elder_terms::RemoteFileClient> sftp_client;
      if (sftp_mode == ProxySftpMode::standalone || sftp_mode == ProxySftpMode::shell_first) {
        sftp_client = co_await elder_terms::open_sftp_client_async(transport, {});
        if (connection) connection->close();
        connection.reset();
        transport.reset();
        // The SFTP client now owns the final transport and the whole route.
        co_await exercise_sftp_client_async(sftp_client, {});
      }
      const unsigned char release = 1;
      expect_true(write_all_fd(target.release_fd, &release, 1), "release final server");
      if (connection) connection->close();
      connection.reset();
      transport.reset();
      sftp_client.reset();
    } catch (...) { failure = std::current_exception(); }
    group.shutdown();
  };
  auto task = body();
  dispatcher.park();
  expect_true(task.is_ready(), "nested SSH task completed");
  const auto target_result = wait_for_server(&target);
  const auto gateway_result = wait_for_server(&gateway);
  if (failure) std::rethrow_exception(failure);
  expect_true(target_result == 0 && gateway_result == 0, "both SSH servers must validate their distinct sessions");
  expect_true(open_socket_count() == initial_sockets, "final SSH/SFTP owner must release every routed socket");
  expect_true(gateway_keys == 1 && target_keys == 1, "both SSH host keys must be verified");
  expect_true(!known_hosts_entries(known_hosts, known_hosts_target(gateway_options.forward_host, target.port)).empty(),
              "destination host key must be stored under its original name and port");
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
  const std::filesystem::path changed_host_private_key =
      root / "ssh_host_changed_ed25519_key";
  const std::filesystem::path changed_host_public_key =
      root / "ssh_host_changed_ed25519_key.pub";
  const std::filesystem::path plain_private_key =
      root / ".ssh" / "id_plain";
  const std::filesystem::path plain_public_key =
      root / ".ssh" / "id_plain.pub";
  const std::filesystem::path encrypted_private_key =
      root / ".ssh" / "id_encrypted";
  const std::filesystem::path encrypted_public_key =
      root / ".ssh" / "id_encrypted.pub";
  // This public test-only key has a fingerprint walk that visits an ordinary
  // cell 15 times. OpenSSH must show its maximum density, never another S.
  {
    std::ofstream key_file(host_private_key);
    key_file << R"KEY(-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZW
QyNTUxOQAAACCYJQ25JId2m4zDE80rGUuf1+O2/zMcuQRzx7dlpuy0bwAAAKDl8GIt5fBi
LQAAAAtzc2gtZWQyNTUxOQAAACCYJQ25JId2m4zDE80rGUuf1+O2/zMcuQRzx7dlpuy0bw
AAAEA8cCQePgwL2LLorJKJb/mbOaBviLYfCkaS2lc+lgrnvZglDbkkh3abjMMTzSsZS5/X
47b/Mxy5BHPHt2Wm7LRvAAAAGHJhbmRvbS1hcnQtYm91bmRhcnktdGVzdAECAwQF
-----END OPENSSH PRIVATE KEY-----
)KEY";
    expect_true(key_file.good(), "failed to write the random-art regression key");
  }
  {
    std::ofstream key_file(host_public_key);
    key_file << "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJglDbkkh3abjMMTzSsZS5/X47b/Mxy5BHPHt2Wm7LRv random-art-boundary-test\n";
    expect_true(key_file.good(), "failed to write the random-art regression public key");
  }
  expect_true(::chmod(host_private_key.c_str(), S_IRUSR | S_IWUSR) == 0,
              "failed to protect the random-art regression key");
  generate_key_pair(changed_host_private_key, changed_host_public_key,
                    nullptr);
  generate_key_pair(plain_private_key, plain_public_key, nullptr);
  generate_key_pair(encrypted_private_key, encrypted_public_key,
                    "key-passphrase");

  ServerOptions options;
  options.auth_mode = ServerAuthMode::none;
  options.host_key_path = host_private_key;
  options.host_public_key_path = host_public_key;
  test_forward_readiness(options, root / ".ssh" / "proxy_known_hosts", true);
  test_forward_readiness(options, root / ".ssh" / "proxy_known_hosts", false);
  test_proxy_forwarding(options, root / ".ssh" / "proxy_known_hosts", false);
  test_proxy_forwarding(options, root / ".ssh" / "proxy_known_hosts", true);
  test_proxy_rejections(options, root / ".ssh" / "proxy_known_hosts");
  run_client_case(
      options,
      ClientCase{
          .auth_mode = ServerAuthMode::none,
          .identity_file = {},
          .known_hosts_file = known_hosts_file,
          .config_file = {},
          .authentication_answer = {},
          .remote_disconnect = true,
          .expected_prompts = {
              elder_terms::SshUserPromptKind::username,
              elder_terms::SshUserPromptKind::host_key,
          },
      });

  options.auth_mode = ServerAuthMode::password;
  options.sftp_root = root / "sftp-root";
  options.expected_exec_command =
      "LC_ALL=C md5sum < '/server'\"'\"'s payload.txt' && "
      "LC_ALL=C sha1sum < '/server'\"'\"'s payload.txt' && "
      "LC_ALL=C sha256sum < '/server'\"'\"'s payload.txt'";
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
  options.expected_exec_command.clear();
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

  auto gateway_options = options;
  gateway_options.host_key_path = changed_host_private_key;
  gateway_options.host_public_key_path = changed_host_public_key;
  gateway_options.authorized_key_path = encrypted_public_key;
  test_proxy_ssh(options, gateway_options, plain_private_key, encrypted_private_key,
                 root / ".ssh" / "nested_known_hosts", false, ProxySftpMode::none);
  test_proxy_ssh(options, gateway_options, plain_private_key, encrypted_private_key,
                 root / ".ssh" / "nested_known_hosts", true, ProxySftpMode::none);

  auto sftp_options = options;
  sftp_options.sftp_root = root / "sftp-root";
  for (const auto mode : {ProxySftpMode::standalone, ProxySftpMode::shell_first, ProxySftpMode::sftp_first})
    test_proxy_ssh(sftp_options, gateway_options, plain_private_key, encrypted_private_key,
                   root / ".ssh" / "sftp_proxy_known_hosts", false, mode);

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

  options.auth_mode = ServerAuthMode::none;
  options.username = "test-user";
  options.host_key_path = changed_host_private_key;
  options.host_public_key_path = changed_host_public_key;
  const int changed_host_port = run_client_case(
      options,
      ClientCase{
          .auth_mode = ServerAuthMode::none,
          .identity_file = {},
          .known_hosts_file = known_hosts_file,
          .config_file = {},
          .conflicting_host_public_key = host_public_key,
          .authentication_answer = {},
          .expected_host_key_command_marker = "ssh-keygen -R",
          .request_host_key_reset = true,
          .expected_host_key_reset_available = true,
          .preserve_protected_host_markers = true,
          .expected_prompts = {
              elder_terms::SshUserPromptKind::username,
              elder_terms::SshUserPromptKind::host_key,
          },
      });
  const std::string changed_target =
      known_hosts_target("127.0.0.1", changed_host_port);
  const std::string changed_entry =
      known_hosts_entries(known_hosts_file, changed_target);
  const std::string changed_public_key_text =
      read_text_file(changed_host_public_key);
  expect_true(
      changed_entry.find(public_key_material(changed_public_key_text)) !=
          std::string::npos,
              "reset SSH host key was not replaced with the current key");
  const std::filesystem::path known_hosts_backup =
      known_hosts_file.string() + ".old";
  const std::string backed_up_entry =
      known_hosts_entries(known_hosts_backup, changed_target);
  const std::string original_public_key_text =
      read_text_file(host_public_key);
  expect_true(
      backed_up_entry.find(public_key_material(original_public_key_text)) !=
          std::string::npos,
      "ssh-keygen did not preserve the replaced known_hosts entry");
  const std::string reset_known_hosts_text = read_text_file(known_hosts_file);
  expect_true(
      reset_known_hosts_text.find("@cert-authority " + changed_target + " ") !=
              std::string::npos &&
          reset_known_hosts_text.find("@revoked " + changed_target + " ") !=
              std::string::npos,
      "ssh-keygen did not preserve protected known_hosts marker entries");

  const std::filesystem::path global_known_hosts_file =
      root / "ssh_global_known_hosts";
  const std::filesystem::path global_user_known_hosts_file =
      root / ".ssh" / "global-test-known_hosts";
  const std::filesystem::path global_config_file =
      root / ".ssh" / "global-config";
  {
    std::ofstream config(global_config_file);
    config << "Host 127.0.0.1\n"
              "  GlobalKnownHostsFile "
           << global_known_hosts_file.string() << "\n";
  }
  run_client_case(
      options,
      ClientCase{
          .auth_mode = ServerAuthMode::none,
          .identity_file = {},
          .known_hosts_file = global_user_known_hosts_file,
          .config_file = global_config_file,
          .conflicting_host_public_key = host_public_key,
          .conflicting_known_hosts_file = global_known_hosts_file,
          .authentication_answer = {},
          .expected_host_key_command_marker = "sudo ssh-keygen -R",
          .expected_failure = "SSH host key was not reset",
          .request_host_key_reset = false,
          .expected_host_key_reset_available = false,
          .expected_prompts = {
              elder_terms::SshUserPromptKind::username,
              elder_terms::SshUserPromptKind::host_key,
          },
      });
  expect_true(!std::filesystem::exists(
                  global_known_hosts_file.string() + ".old"),
              "global SSH known_hosts was modified automatically");

  expect_true(line_count(known_hosts_file) == 10,
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

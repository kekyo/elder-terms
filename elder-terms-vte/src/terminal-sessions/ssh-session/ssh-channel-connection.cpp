#include "ssh-channel-connection.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <glib.h>
#include <libssh/callbacks.h>
#include <libssh/libssh.h>

#include "../tcp-connector.h"

namespace elder_terms {

static std::runtime_error ssh_failure(ssh_session session,
                                      const std::string &operation) {
  const char *detail = session == nullptr ? nullptr : ssh_get_error(session);
  if (detail == nullptr || detail[0] == '\0') {
    return std::runtime_error(operation);
  }
  return std::runtime_error(operation + ": " + detail);
}

static cardio::fd_event ssh_ready_events(ssh_session session,
                                         cardio::fd_event fallback) {
  const int flags = ssh_get_poll_flags(session);
  cardio::fd_event events = cardio::fd_event::none;
  if ((flags & SSH_READ_PENDING) != 0) {
    events |= cardio::fd_event::read;
  }
  if ((flags & SSH_WRITE_PENDING) != 0) {
    events |= cardio::fd_event::write;
  }
  if (events == cardio::fd_event::none) {
    events = fallback;
  }
  return events | cardio::fd_event::error | cardio::fd_event::hangup;
}

static cardio::promise<void>
await_ssh_ready_async(ssh_session session, cardio::fd_event fallback,
                      cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  const int fd = ssh_get_fd(session);
  if (fd < 0) {
    throw ssh_failure(session, "SSH session has no socket");
  }
  (void)co_await cardio::from_fd(fd, ssh_ready_events(session, fallback),
                                 std::move(cancellation));
}

static bool ssh_result_is_again(ssh_session session, int result) {
  return result == SSH_AGAIN ||
         (result == SSH_ERROR && ssh_get_error_code(session) == SSH_AGAIN);
}

static int open_ssh_event_fd() {
  const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "eventfd failed");
  }
  return fd;
}

static int open_ssh_epoll_fd() {
  const int fd = ::epoll_create1(EPOLL_CLOEXEC);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "epoll_create1 failed");
  }
  return fd;
}

static void close_ssh_fd(int *fd) {
  if (*fd >= 0) {
    (void)::close(*fd);
    *fd = -1;
  }
}

static void notify_ssh_event_fd(int fd) {
  if (fd < 0) {
    return;
  }
  while (::eventfd_write(fd, 1) < 0 && errno == EINTR) {
  }
}

static void drain_ssh_event_fd(int fd) {
  if (fd < 0) {
    return;
  }
  eventfd_t value = 0;
  while (::eventfd_read(fd, &value) < 0 && errno == EINTR) {
  }
  while (::eventfd_read(fd, &value) == 0) {
  }
}

template <typename Operation>
static cardio::promise<void>
await_ssh_ok_async(ssh_session session, Operation operation,
                   const std::string &description,
                   cardio::cancellation cancellation) {
  for (;;) {
    cancellation.throw_if_cancellation_requested();
    const int result = operation();
    if (result == SSH_OK) {
      co_return;
    }
    if (!ssh_result_is_again(session, result)) {
      throw ssh_failure(session, description);
    }
    co_await await_ssh_ready_async(
        session, cardio::fd_event::read | cardio::fd_event::write,
        cancellation);
  }
}

template <typename Operation>
static cardio::promise<int>
await_ssh_auth_async(ssh_session session, Operation operation,
                     cardio::cancellation cancellation) {
  for (;;) {
    cancellation.throw_if_cancellation_requested();
    const int result = operation();
    if (result != SSH_AUTH_AGAIN) {
      co_return result;
    }
    co_await await_ssh_ready_async(
        session, cardio::fd_event::read | cardio::fd_event::write,
        cancellation);
  }
}

static cardio::promise<void>
flush_ssh_async(ssh_session session, int read_wakeup_fd,
                cardio::cancellation cancellation) {
  for (;;) {
    cancellation.throw_if_cancellation_requested();
    const int result = ssh_blocking_flush(session, 0);
    // A nonblocking libssh write operation can also consume input packets and
    // leave channel data buffered without the socket remaining readable.
    notify_ssh_event_fd(read_wakeup_fd);
    if (result == SSH_OK) {
      co_return;
    }
    if (!ssh_result_is_again(session, result)) {
      throw ssh_failure(session, "Failed to flush SSH output");
    }
    co_await await_ssh_ready_async(session, cardio::fd_event::write,
                                   cancellation);
  }
}

static int reject_libssh_terminal_prompt(const char *, char *, std::size_t,
                                         int, int, void *) {
  // libssh's synchronous default asker must never read from a terminal or
  // create UI. Interactive answers are collected asynchronously by GTK.
  return -1;
}

static int clamped_pty_dimension(glong value) {
  if (value <= 0) {
    return 1;
  }
  if (value > std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(value);
}

static std::string expanded_identity_path(const std::string &path) {
  if (path.rfind("~/", 0) != 0) {
    return path;
  }
  const char *home = g_get_home_dir();
  if (home == nullptr || home[0] == '\0') {
    return path;
  }
  return std::string(home) + path.substr(1);
}

static std::string effective_username(ssh_session session) {
  char *value = nullptr;
  if (ssh_options_get(session, SSH_OPTIONS_USER, &value) != SSH_OK ||
      value == nullptr) {
    return {};
  }
  std::string username(value);
  ssh_string_free_char(value);
  return username;
}

static cardio::promise<std::optional<SshUserPromptResponse>>
request_ssh_prompt_async(const TerminalSessionCallbacks &callbacks,
                         const SshUserPrompt &prompt,
                         cardio::cancellation cancellation) {
  if (!callbacks.ssh_prompt) {
    co_return std::nullopt;
  }
  SshUserPromptResponse response =
      co_await callbacks.ssh_prompt(prompt, cancellation);
  if (!response.accepted) {
    co_return std::nullopt;
  }
  co_return response;
}

static std::optional<SshUserPrompt>
host_key_prompt(ssh_session session, const SshConnectionSettings &settings,
                enum ssh_known_hosts_e status) {
  ssh_key server_key = nullptr;
  if (ssh_get_server_publickey(session, &server_key) != SSH_OK) {
    return std::nullopt;
  }

  const char *key_type_name =
      ssh_key_type_to_char(ssh_key_type(server_key));
  const std::string key_type =
      key_type_name == nullptr ? "unknown" : key_type_name;
  unsigned char *hash = nullptr;
  std::size_t hash_size = 0;
  std::string fingerprint;
  if (ssh_get_publickey_hash(server_key, SSH_PUBLICKEY_HASH_SHA256, &hash,
                             &hash_size) == SSH_OK) {
    char *text =
        ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hash_size);
    if (text != nullptr) {
      fingerprint = text;
      ssh_string_free_char(text);
    }
    ssh_clean_pubkey_hash(&hash);
  }
  ssh_key_free(server_key);
  if (fingerprint.empty()) {
    return std::nullopt;
  }

  std::string message =
      "Unknown SSH host key for " + settings.address + ":" +
      std::to_string(settings.port) + "\nKey type: " + key_type +
      "\nFingerprint: " + fingerprint;
  if (status == SSH_KNOWN_HOSTS_NOT_FOUND) {
    message += "\nThe known_hosts file does not exist and will be created.";
  }
  message += "\nAccept and save this host key?";
  return SshUserPrompt{
      .kind = SshUserPromptKind::host_key,
      .title = "SSH Host Key",
      .message = std::move(message),
      .input_required = false,
      .echo = false,
  };
}

static cardio::promise<void>
verify_host_key_async(ssh_session session,
                      const SshConnectionSettings &settings,
                      const TerminalSessionCallbacks &callbacks,
                      cardio::cancellation cancellation) {
  const enum ssh_known_hosts_e status =
      ssh_session_is_known_server(session);
  if (status == SSH_KNOWN_HOSTS_OK) {
    co_return;
  }
  if (status == SSH_KNOWN_HOSTS_CHANGED) {
    throw std::runtime_error(
        "SSH host key changed; connection rejected");
  }
  if (status == SSH_KNOWN_HOSTS_OTHER) {
    throw std::runtime_error(
        "SSH host key type differs from known_hosts; connection rejected");
  }
  if (status == SSH_KNOWN_HOSTS_ERROR) {
    throw ssh_failure(session, "Failed to check SSH host key");
  }

  const std::optional<SshUserPrompt> prompt =
      host_key_prompt(session, settings, status);
  if (!prompt.has_value() ||
      !(co_await request_ssh_prompt_async(callbacks, *prompt, cancellation))
           .has_value()) {
    throw std::runtime_error("SSH host key was not accepted");
  }
  if (ssh_session_update_known_hosts(session) != SSH_OK) {
    throw ssh_failure(session, "Failed to save SSH host key");
  }
}

static cardio::promise<int>
authenticate_private_key_async(ssh_session session,
                               const SshConnectionSettings &settings,
                               const TerminalSessionCallbacks &callbacks,
                               cardio::cancellation cancellation) {
  if (settings.identity_file.empty()) {
    co_return SSH_AUTH_DENIED;
  }

  const std::string path = expanded_identity_path(settings.identity_file);
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(path, filesystem_error)) {
    co_return SSH_AUTH_DENIED;
  }

  ssh_key private_key = nullptr;
  int imported = ssh_pki_import_privkey_file(
      path.c_str(), nullptr, nullptr, nullptr, &private_key);
  constexpr unsigned int maximum_passphrase_attempts = 3;
  for (unsigned int attempt = 1;
       imported != SSH_OK && attempt <= maximum_passphrase_attempts;
       ++attempt) {
    const std::optional<SshUserPromptResponse> response =
        co_await request_ssh_prompt_async(
            callbacks,
            {
                .kind = SshUserPromptKind::private_key_passphrase,
                .title = "SSH Key Passphrase",
                .message =
                    "Passphrase for " + settings.identity_file +
                    " (attempt " + std::to_string(attempt) + " of " +
                    std::to_string(maximum_passphrase_attempts) + "):",
                .input_required = true,
                .echo = false,
            },
            cancellation);
    if (!response.has_value()) {
      co_return SSH_AUTH_DENIED;
    }
    imported = ssh_pki_import_privkey_file(
        path.c_str(), response->text.c_str(), nullptr, nullptr, &private_key);
  }
  if (imported != SSH_OK || private_key == nullptr) {
    co_return SSH_AUTH_DENIED;
  }

  const int result = co_await await_ssh_auth_async(
      session,
      [session, private_key]() {
        return ssh_userauth_publickey(session, nullptr, private_key);
      },
      cancellation);
  ssh_key_free(private_key);
  co_return result;
}

static cardio::promise<bool>
authenticate_password_async(ssh_session session,
                            const SshConnectionSettings &settings,
                            const TerminalSessionCallbacks &callbacks,
                            cardio::cancellation cancellation) {
  constexpr unsigned int maximum_attempts = 3;
  const std::string username = effective_username(session);
  for (unsigned int attempt = 1; attempt <= maximum_attempts; ++attempt) {
    std::string target = settings.address;
    if (!username.empty()) {
      target = username + "@" + target;
    }
    const std::optional<SshUserPromptResponse> response =
        co_await request_ssh_prompt_async(
            callbacks,
            {
                .kind = SshUserPromptKind::password,
                .title = "SSH Authentication",
                .message =
                    "Password for " + target + " (attempt " +
                    std::to_string(attempt) + " of " +
                    std::to_string(maximum_attempts) + "):",
                .input_required = true,
                .echo = false,
            },
            cancellation);
    if (!response.has_value()) {
      co_return false;
    }
    const int result = co_await await_ssh_auth_async(
        session,
        [session, &response]() {
          return ssh_userauth_password(session, nullptr,
                                       response->text.c_str());
        },
        cancellation);
    if (result == SSH_AUTH_SUCCESS) {
      co_return true;
    }
    if (result == SSH_AUTH_ERROR) {
      throw ssh_failure(session, "SSH password authentication failed");
    }
  }
  co_return false;
}

static std::string keyboard_interactive_message(const char *name,
                                                const char *instruction,
                                                const char *prompt) {
  std::string message;
  if (name != nullptr && name[0] != '\0') {
    message += name;
  }
  if (instruction != nullptr && instruction[0] != '\0') {
    if (!message.empty()) {
      message += "\n";
    }
    message += instruction;
  }
  if (prompt != nullptr && prompt[0] != '\0') {
    if (!message.empty()) {
      message += "\n";
    }
    message += prompt;
  }
  return message.empty() ? "SSH authentication response:" : message;
}

static cardio::promise<bool>
authenticate_keyboard_interactive_async(
    ssh_session session, const TerminalSessionCallbacks &callbacks,
    cardio::cancellation cancellation) {
  constexpr unsigned int maximum_attempts = 3;
  for (unsigned int attempt = 1; attempt <= maximum_attempts; ++attempt) {
    int result = co_await await_ssh_auth_async(
        session,
        [session]() {
          return ssh_userauth_kbdint(session, nullptr, nullptr);
        },
        cancellation);
    while (result == SSH_AUTH_INFO) {
      const char *name = ssh_userauth_kbdint_getname(session);
      const char *instruction =
          ssh_userauth_kbdint_getinstruction(session);
      const int prompt_count = ssh_userauth_kbdint_getnprompts(session);
      if (prompt_count < 0) {
        throw ssh_failure(session,
                          "Failed to read SSH authentication questions");
      }
      for (int index = 0; index < prompt_count; ++index) {
        char echo = 0;
        const char *prompt = ssh_userauth_kbdint_getprompt(
            session, static_cast<unsigned int>(index), &echo);
        const std::optional<SshUserPromptResponse> response =
            co_await request_ssh_prompt_async(
                callbacks,
                {
                    .kind = SshUserPromptKind::keyboard_interactive,
                    .title = "SSH Authentication",
                    .message = keyboard_interactive_message(
                        name, instruction, prompt),
                    .input_required = true,
                    .echo = echo != 0,
                },
                cancellation);
        if (!response.has_value() ||
            ssh_userauth_kbdint_setanswer(
                session, static_cast<unsigned int>(index),
                response->text.c_str()) != SSH_OK) {
          co_return false;
        }
      }
      result = co_await await_ssh_auth_async(
          session,
          [session]() {
            return ssh_userauth_kbdint(session, nullptr, nullptr);
          },
          cancellation);
    }
    if (result == SSH_AUTH_SUCCESS) {
      co_return true;
    }
    if (result == SSH_AUTH_ERROR) {
      throw ssh_failure(
          session, "SSH keyboard-interactive authentication failed");
    }
  }
  co_return false;
}

static cardio::promise<void>
authenticate_session_async(ssh_session session,
                           const SshConnectionSettings &settings,
                           const TerminalSessionCallbacks &callbacks,
                           cardio::cancellation cancellation) {
  int result = co_await await_ssh_auth_async(
      session,
      [session]() { return ssh_userauth_none(session, nullptr); },
      cancellation);
  if (result == SSH_AUTH_SUCCESS) {
    co_return;
  }
  if (result == SSH_AUTH_ERROR) {
    throw ssh_failure(session, "SSH authentication negotiation failed");
  }

  int methods = ssh_userauth_list(session, nullptr);
  if (methods < 0) {
    throw ssh_failure(session, "Failed to list SSH authentication methods");
  }

  if ((methods & SSH_AUTH_METHOD_PUBLICKEY) != 0 &&
      !settings.identity_file.empty()) {
    result = co_await authenticate_private_key_async(
        session, settings, callbacks, cancellation);
    if (result == SSH_AUTH_SUCCESS) {
      co_return;
    }
    if (result == SSH_AUTH_ERROR) {
      throw ssh_failure(session, "SSH public-key authentication failed");
    }
  }

  if ((methods & SSH_AUTH_METHOD_PUBLICKEY) != 0) {
    result = co_await await_ssh_auth_async(
        session,
        [session]() {
          // publickey_auto covers SSH Agent and default identity files. A
          // non-null passphrase and the rejecting session callback prevent
          // libssh from invoking its terminal-based default asker.
          return ssh_userauth_publickey_auto(session, nullptr, "");
        },
        cancellation);
    if (result == SSH_AUTH_SUCCESS) {
      co_return;
    }
  }

  methods = ssh_userauth_list(session, nullptr);
  if (methods < 0) {
    throw ssh_failure(session, "Failed to refresh SSH authentication methods");
  }
  if ((methods & SSH_AUTH_METHOD_PASSWORD) != 0 &&
      co_await authenticate_password_async(session, settings, callbacks,
                                           cancellation)) {
    co_return;
  }
  if ((methods & SSH_AUTH_METHOD_INTERACTIVE) != 0 &&
      co_await authenticate_keyboard_interactive_async(
          session, callbacks, cancellation)) {
    co_return;
  }
  throw std::runtime_error(
      "SSH server did not accept an available authentication method");
}

struct SshChannelConnection::Impl {
  ssh_session session = nullptr;
  ssh_channel channel = nullptr;
  ssh_callbacks_struct session_callbacks = {};
  int read_socket_fd = -1;
  int read_wakeup_fd = -1;
  int read_wait_fd = -1;
  bool closed = false;

  ~Impl() {
    close();
    close_ssh_fd(&read_wait_fd);
    close_ssh_fd(&read_wakeup_fd);
  }

  void initialize_read_waiter() {
    read_socket_fd = ssh_get_fd(session);
    if (read_socket_fd < 0) {
      throw ssh_failure(session, "SSH session has no socket");
    }

    read_wakeup_fd = open_ssh_event_fd();
    try {
      read_wait_fd = open_ssh_epoll_fd();

      epoll_event wakeup_event = {};
      wakeup_event.events = EPOLLIN;
      wakeup_event.data.fd = read_wakeup_fd;
      if (::epoll_ctl(read_wait_fd, EPOLL_CTL_ADD, read_wakeup_fd,
                      &wakeup_event) < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "epoll_ctl failed for SSH wakeup");
      }

      epoll_event socket_event = {};
      socket_event.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
      socket_event.data.fd = read_socket_fd;
      if (::epoll_ctl(read_wait_fd, EPOLL_CTL_ADD, read_socket_fd,
                      &socket_event) < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "epoll_ctl failed for SSH socket");
      }
    } catch (...) {
      close_ssh_fd(&read_wait_fd);
      close_ssh_fd(&read_wakeup_fd);
      throw;
    }
  }

  void notify_read_retry() const {
    notify_ssh_event_fd(read_wakeup_fd);
  }

  cardio::promise<void>
  await_read_retry_async(cardio::cancellation cancellation) {
    cancellation.throw_if_cancellation_requested();
    if (closed || session == nullptr || read_socket_fd < 0 ||
        read_wait_fd < 0) {
      co_return;
    }

    const int flags = ssh_get_poll_flags(session);
    std::uint32_t socket_events = EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    if ((flags & SSH_READ_PENDING) != 0) {
      socket_events |= EPOLLIN;
    }
    if ((flags & SSH_WRITE_PENDING) != 0) {
      socket_events |= EPOLLOUT;
    }
    if ((socket_events & (EPOLLIN | EPOLLOUT)) == 0) {
      socket_events |= EPOLLIN;
    }

    epoll_event socket_event = {};
    socket_event.events = socket_events;
    socket_event.data.fd = read_socket_fd;
    if (::epoll_ctl(read_wait_fd, EPOLL_CTL_MOD, read_socket_fd,
                    &socket_event) < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "epoll_ctl failed for SSH socket");
    }

    (void)co_await cardio::from_fd(
        read_wait_fd,
        cardio::fd_event::read | cardio::fd_event::error |
            cardio::fd_event::hangup,
        std::move(cancellation));

    std::array<epoll_event, 2> events = {};
    int event_count = -1;
    do {
      event_count = ::epoll_wait(read_wait_fd, events.data(),
                                 static_cast<int>(events.size()), 0);
    } while (event_count < 0 && errno == EINTR);
    if (event_count < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "epoll_wait failed for SSH input");
    }
    drain_ssh_event_fd(read_wakeup_fd);
  }

  void close() {
    if (closed) {
      return;
    }
    closed = true;
    notify_read_retry();
    if (channel != nullptr) {
      if (ssh_channel_is_open(channel) != 0) {
        (void)ssh_channel_send_eof(channel);
        (void)ssh_channel_close(channel);
      }
      ssh_channel_free(channel);
      channel = nullptr;
    }
    if (session != nullptr) {
      ssh_disconnect(session);
      ssh_free(session);
      session = nullptr;
    }
  }
};

SshChannelConnection::SshChannelConnection(std::unique_ptr<Impl> impl)
    : impl(std::move(impl)) {
}

SshChannelConnection::~SshChannelConnection() = default;

cardio::promise<std::unique_ptr<SshChannelConnection>>
SshChannelConnection::connect_async(
    const SshConnectionSettings &settings, glong columns, glong rows,
    const TerminalSessionCallbacks &callbacks,
    SshChannelConnectionOptions options,
    cardio::cancellation cancellation) {
  cardio::io_uring io(64);
  int socket_fd = co_await connect_tcp_socket_async(
      io, settings.address, static_cast<std::uint16_t>(settings.port),
      cancellation);

  auto connection_impl = std::make_unique<Impl>();
  connection_impl->session = ssh_new();
  if (connection_impl->session == nullptr) {
    (void)::close(socket_fd);
    throw std::runtime_error("Failed to allocate SSH session");
  }
  ssh_session session = connection_impl->session;
  try {
    if (ssh_options_set(session, SSH_OPTIONS_HOST,
                        settings.address.c_str()) != SSH_OK) {
      throw ssh_failure(session, "Failed to set SSH host");
    }
    if (ssh_options_parse_config(session, nullptr) != SSH_OK) {
      throw ssh_failure(session, "Failed to parse SSH configuration");
    }
    const unsigned int port = static_cast<unsigned int>(settings.port);
    if (ssh_options_set(session, SSH_OPTIONS_HOST,
                        settings.address.c_str()) != SSH_OK ||
        ssh_options_set(session, SSH_OPTIONS_PORT, &port) != SSH_OK ||
        (!settings.username.empty() &&
         ssh_options_set(session, SSH_OPTIONS_USER,
                         settings.username.c_str()) != SSH_OK)) {
      throw ssh_failure(session, "Failed to configure SSH endpoint");
    }
    if (!options.known_hosts_file.empty() &&
        ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS,
                        options.known_hosts_file.c_str()) != SSH_OK) {
      throw ssh_failure(session, "Failed to configure SSH known_hosts");
    }
    if (ssh_options_set(session, SSH_OPTIONS_FD, &socket_fd) != SSH_OK) {
      throw ssh_failure(session, "Failed to attach SSH socket");
    }
    socket_fd = -1;

    connection_impl->session_callbacks.userdata = nullptr;
    connection_impl->session_callbacks.auth_function =
        reject_libssh_terminal_prompt;
    ssh_callbacks_init(&connection_impl->session_callbacks);
    if (ssh_set_callbacks(session,
                          &connection_impl->session_callbacks) != SSH_OK) {
      throw ssh_failure(session, "Failed to configure SSH callbacks");
    }
    ssh_set_blocking(session, 0);
    co_await await_ssh_ok_async(
        session, [session]() { return ssh_connect(session); },
        "Failed to establish SSH transport", cancellation);

    if (callbacks.connection_phase) {
      callbacks.connection_phase(
          TerminalSessionConnectionPhase::verifying_host);
    }
    co_await verify_host_key_async(session, settings, callbacks,
                                   cancellation);

    if (callbacks.connection_phase) {
      callbacks.connection_phase(
          TerminalSessionConnectionPhase::authenticating);
    }
    co_await authenticate_session_async(session, settings, callbacks,
                                        cancellation);

    if (callbacks.connection_phase) {
      callbacks.connection_phase(
          TerminalSessionConnectionPhase::opening_shell);
    }
    connection_impl->channel = ssh_channel_new(session);
    if (connection_impl->channel == nullptr) {
      throw ssh_failure(session, "Failed to allocate SSH channel");
    }
    ssh_channel channel = connection_impl->channel;
    co_await await_ssh_ok_async(
        session, [channel]() { return ssh_channel_open_session(channel); },
        "Failed to open SSH session channel", cancellation);
    co_await await_ssh_ok_async(
        session,
        [channel, &settings, columns, rows]() {
          return ssh_channel_request_pty_size(
              channel, settings.terminal_type.c_str(),
              clamped_pty_dimension(columns),
              clamped_pty_dimension(rows));
        },
        "Failed to request SSH PTY", cancellation);
    co_await await_ssh_ok_async(
        session, [channel]() { return ssh_channel_request_shell(channel); },
        "Failed to request SSH shell", cancellation);
    co_await flush_ssh_async(session, -1, cancellation);
    connection_impl->initialize_read_waiter();
  } catch (...) {
    if (socket_fd >= 0) {
      (void)::close(socket_fd);
    }
    throw;
  }

  co_return std::unique_ptr<SshChannelConnection>(
      new SshChannelConnection(std::move(connection_impl)));
}

cardio::promise<std::size_t>
SshChannelConnection::read_async(std::span<unsigned char> buffer,
                                 cardio::cancellation cancellation) {
  if (impl == nullptr || impl->closed || impl->session == nullptr ||
      impl->channel == nullptr) {
    throw std::runtime_error("SSH channel is closed");
  }
  if (buffer.empty()) {
    co_return 0;
  }

  const std::uint32_t capacity = static_cast<std::uint32_t>(
      std::min<std::size_t>(buffer.size(),
                            std::numeric_limits<std::uint32_t>::max()));
  for (;;) {
    cancellation.throw_if_cancellation_requested();
    if (impl->closed || impl->session == nullptr ||
        impl->channel == nullptr) {
      co_return 0;
    }
    /*
     * Check the PTY's normal data stream last. A libssh read for one stream
     * can consume socket packets for the other stream; checking stdout last
     * ensures transfer protocol replies buffered by the stderr check are
     * observed before waiting again.
     */
    for (int is_stderr : {1, 0}) {
      const int read_size = ssh_channel_read_nonblocking(
          impl->channel, buffer.data(), capacity, is_stderr);
      if (read_size > 0) {
        co_return static_cast<std::size_t>(read_size);
      }
      if (read_size == SSH_ERROR &&
          !ssh_result_is_again(impl->session, read_size)) {
        throw ssh_failure(impl->session, "Failed to read SSH channel");
      }
    }
    if (ssh_channel_is_eof(impl->channel) != 0 ||
        ssh_channel_is_closed(impl->channel) != 0 ||
        ssh_channel_is_open(impl->channel) == 0) {
      co_return 0;
    }
    co_await impl->await_read_retry_async(cancellation);
  }
}

cardio::promise<void> SshChannelConnection::write_all_async(
    std::span<const unsigned char> bytes,
    cardio::cancellation cancellation) {
  if (impl == nullptr || impl->closed || impl->session == nullptr ||
      impl->channel == nullptr) {
    throw std::runtime_error("SSH channel is closed");
  }

  std::size_t offset = 0;
  while (offset < bytes.size()) {
    cancellation.throw_if_cancellation_requested();
    const std::uint32_t chunk_size = static_cast<std::uint32_t>(
        std::min<std::size_t>(
            bytes.size() - offset,
            std::numeric_limits<std::uint32_t>::max()));
    const int written = ssh_channel_write(
        impl->channel, bytes.data() + offset, chunk_size);
    impl->notify_read_retry();
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (!ssh_result_is_again(impl->session, written) && written != 0) {
      throw ssh_failure(impl->session, "Failed to write SSH channel");
    }
    co_await await_ssh_ready_async(impl->session, cardio::fd_event::write,
                                   cancellation);
  }
  co_await flush_ssh_async(impl->session, impl->read_wakeup_fd,
                           cancellation);
}

cardio::promise<void>
SshChannelConnection::resize_async(glong columns, glong rows,
                                   cardio::cancellation cancellation) {
  if (impl == nullptr || impl->closed || impl->session == nullptr ||
      impl->channel == nullptr) {
    throw std::runtime_error("SSH channel is closed");
  }
  co_await await_ssh_ok_async(
      impl->session,
      [this, columns, rows]() {
        const int result = ssh_channel_change_pty_size(
            impl->channel, clamped_pty_dimension(columns),
            clamped_pty_dimension(rows));
        impl->notify_read_retry();
        return result;
      },
      "Failed to resize SSH PTY", cancellation);
  co_await flush_ssh_async(impl->session, impl->read_wakeup_fd,
                           cancellation);
}

void SshChannelConnection::close() {
  if (impl != nullptr) {
    impl->close();
  }
}

} // namespace elder_terms

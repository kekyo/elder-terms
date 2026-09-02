#include "ssh-channel-connection.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include <glib.h>
#include <libssh/callbacks.h>
#include <libssh/libssh.h>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

#include "../tcp-connector.h"

namespace elder_terms {

static std::string format_translated_string(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  gchar *formatted = g_strdup_vprintf(format, arguments);
  va_end(arguments);
  const std::string result = formatted == nullptr ? std::string() : formatted;
  g_free(formatted);
  return result;
}

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
    throw ssh_failure(session, _("SSH session has no socket"));
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

static int open_ssh_blocking_event_fd() {
  const int fd = ::eventfd(0, EFD_CLOEXEC);
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

static cardio::promise<std::string>
request_ssh_username_async(ssh_session session,
                           const SshEndpointSettings &settings,
                           const TerminalSessionCallbacks &callbacks,
                           cardio::cancellation cancellation) {
  std::string username = effective_username(session);
  if (username.empty()) {
    const char *current_username = g_get_user_name();
    if (current_username != nullptr) {
      username = current_username;
    }
  }

  bool username_missing = false;
  while (true) {
    std::string message = format_translated_string(
        _("User name for %s:"), settings.address.c_str());
    if (username_missing) {
      message += "\n\n";
      message += _("User name must not be empty.");
    }
    const std::optional<SshUserPromptResponse> response =
        co_await request_ssh_prompt_async(
            callbacks,
            {
                .kind = SshUserPromptKind::username,
                .title = _("SSH Authentication"),
                .message = std::move(message),
                .initial_text = username,
                .input_required = true,
                .echo = true,
            },
            cancellation);
    if (!response.has_value()) {
      throw std::runtime_error(_("SSH user name was not provided"));
    }
    username = response->text;
    if (username.find_first_not_of(" \t\r\n") != std::string::npos) {
      co_return username;
    }
    username_missing = true;
  }
}

static std::optional<SshUserPrompt>
host_key_prompt(ssh_session session, const SshEndpointSettings &settings,
                enum ssh_known_hosts_e status) {
  ssh_key server_key = nullptr;
  if (ssh_get_server_publickey(session, &server_key) != SSH_OK) {
    return std::nullopt;
  }

  const char *key_type_name =
      ssh_key_type_to_char(ssh_key_type(server_key));
  const std::string key_type =
      key_type_name == nullptr ? _("unknown") : key_type_name;
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

  std::string message = format_translated_string(
      _("Unknown SSH host key for %s:%u\nKey type: %s\nFingerprint: %s"),
      settings.address.c_str(), static_cast<unsigned int>(settings.port),
      key_type.c_str(),
      fingerprint.c_str());
  if (status == SSH_KNOWN_HOSTS_NOT_FOUND) {
    message += "\n";
    message += _("The known_hosts file does not exist and will be created.");
  }
  message += "\n";
  message += _("Accept and save this host key?");
  return SshUserPrompt{
      .kind = SshUserPromptKind::host_key,
      .title = _("SSH Host Key"),
      .message = std::move(message),
      .initial_text = {},
      .input_required = false,
      .echo = false,
  };
}

static cardio::promise<void>
verify_host_key_async(ssh_session session,
                      const SshEndpointSettings &settings,
                      const TerminalSessionCallbacks &callbacks,
                      cardio::cancellation cancellation) {
  const enum ssh_known_hosts_e status =
      ssh_session_is_known_server(session);
  if (status == SSH_KNOWN_HOSTS_OK) {
    co_return;
  }
  if (status == SSH_KNOWN_HOSTS_CHANGED) {
    throw std::runtime_error(
        _("SSH host key changed; connection rejected"));
  }
  if (status == SSH_KNOWN_HOSTS_OTHER) {
    throw std::runtime_error(
        _("SSH host key type differs from known_hosts; connection rejected"));
  }
  if (status == SSH_KNOWN_HOSTS_ERROR) {
    throw ssh_failure(session, _("Failed to check SSH host key"));
  }

  const std::optional<SshUserPrompt> prompt =
      host_key_prompt(session, settings, status);
  if (!prompt.has_value() ||
      !(co_await request_ssh_prompt_async(callbacks, *prompt, cancellation))
           .has_value()) {
    throw std::runtime_error(_("SSH host key was not accepted"));
  }
  if (ssh_session_update_known_hosts(session) != SSH_OK) {
    throw ssh_failure(session, _("Failed to save SSH host key"));
  }
}

static cardio::promise<int>
authenticate_private_key_async(ssh_session session,
                               const SshEndpointSettings &settings,
                               const TerminalSessionCallbacks &callbacks,
                               cardio::cancellation cancellation) {
  if (settings.identity_file.empty()) {
    co_return SSH_AUTH_DENIED;
  }

  const std::string path =
      expanded_identity_path(settings.identity_file);
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
                .title = _("SSH Key Passphrase"),
                .message = format_translated_string(
                    _("Passphrase for %s (attempt %u of %u):"),
                    settings.identity_file.c_str(), attempt,
                    maximum_passphrase_attempts),
                .initial_text = {},
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
                            const SshEndpointSettings &settings,
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
                .title = _("SSH Authentication"),
                .message = format_translated_string(
                    _("Password for %s (attempt %u of %u):"), target.c_str(),
                    attempt, maximum_attempts),
                .initial_text = {},
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
      throw ssh_failure(session, _("SSH password authentication failed"));
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
  return message.empty() ? _("SSH authentication response:") : message;
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
                    .title = _("SSH Authentication"),
                    .message = keyboard_interactive_message(
                        name, instruction, prompt),
                    .initial_text = {},
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
          session, _("SSH keyboard-interactive authentication failed"));
    }
  }
  co_return false;
}

static cardio::promise<void>
authenticate_session_async(ssh_session session,
                           const SshEndpointSettings &settings,
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
    throw ssh_failure(session, _("SSH authentication negotiation failed"));
  }

  int methods = ssh_userauth_list(session, nullptr);
  if (methods < 0) {
    throw ssh_failure(session, _("Failed to list SSH authentication methods"));
  }

  if ((methods & SSH_AUTH_METHOD_PUBLICKEY) != 0 &&
      !settings.identity_file.empty()) {
    result = co_await authenticate_private_key_async(
        session, settings, callbacks, cancellation);
    if (result == SSH_AUTH_SUCCESS) {
      co_return;
    }
    if (result == SSH_AUTH_ERROR) {
      throw ssh_failure(session, _("SSH public-key authentication failed"));
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
    throw ssh_failure(
        session, _("Failed to refresh SSH authentication methods"));
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
      _("SSH server did not accept an available authentication method"));
}

template <typename T> struct SshWorkerCompletion {
  int event_fd = -1;
  std::optional<T> value;
  std::exception_ptr error;

  SshWorkerCompletion() : event_fd(open_ssh_event_fd()) {
  }

  ~SshWorkerCompletion() {
    close_ssh_fd(&event_fd);
  }
};

struct SshWorkerResult {
  int value = SSH_ERROR;
  int error_code = SSH_ERROR;
  int poll_flags = 0;
  std::string error;
};

struct SshChannelReadResult {
  int size = SSH_AGAIN;
  int poll_flags = 0;
  bool eof = false;
};

static bool ssh_worker_result_is_again(const SshWorkerResult &result) {
  return result.value == SSH_AGAIN ||
         (result.value == SSH_ERROR && result.error_code == SSH_AGAIN);
}

struct AuthenticatedSshTransport::Impl {
  SshEndpointSettings endpoint;
  ssh_session session = nullptr;
  ssh_callbacks_struct session_callbacks = {};
  int socket_fd = -1;
  int wakeup_fd = -1;
  int wait_fd = -1;
  int command_fd = -1;
  std::mutex command_mutex;
  std::deque<std::function<void()>> commands;
  std::vector<ssh_channel> channels;
  std::thread worker;
  std::atomic_bool sftp_transfer_active = false;
  bool stopping = false;

  ~Impl() {
    stop_worker();
    close_ssh_fd(&command_fd);
    close_ssh_fd(&wait_fd);
    close_ssh_fd(&wakeup_fd);
  }

  void initialize_waiter() {
    socket_fd = ssh_get_fd(session);
    if (socket_fd < 0) {
      throw ssh_failure(session, _("SSH session has no socket"));
    }

    wakeup_fd = open_ssh_event_fd();
    try {
      wait_fd = open_ssh_epoll_fd();

      epoll_event wakeup_event = {};
      wakeup_event.events = EPOLLIN;
      wakeup_event.data.fd = wakeup_fd;
      if (::epoll_ctl(wait_fd, EPOLL_CTL_ADD, wakeup_fd,
                      &wakeup_event) < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "epoll_ctl failed for SSH wakeup");
      }

      epoll_event socket_event = {};
      socket_event.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
      socket_event.data.fd = socket_fd;
      if (::epoll_ctl(wait_fd, EPOLL_CTL_ADD, socket_fd,
                      &socket_event) < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "epoll_ctl failed for SSH socket");
      }
    } catch (...) {
      close_ssh_fd(&wait_fd);
      close_ssh_fd(&wakeup_fd);
      throw;
    }
  }

  void start_worker() {
    command_fd = open_ssh_blocking_event_fd();
    worker = std::thread([this]() { worker_loop(); });
  }

  void worker_loop() {
    for (;;) {
      eventfd_t value = 0;
      while (::eventfd_read(command_fd, &value) < 0 && errno == EINTR) {
      }

      std::deque<std::function<void()>> pending;
      bool should_stop = false;
      {
        const std::lock_guard<std::mutex> lock(command_mutex);
        pending.swap(commands);
        should_stop = stopping && pending.empty();
      }
      if (should_stop) {
        break;
      }
      for (std::function<void()> &command : pending) {
        command();
      }
      {
        const std::lock_guard<std::mutex> lock(command_mutex);
        should_stop = stopping && commands.empty();
      }
      if (should_stop) {
        break;
      }
    }
    close_session_on_worker();
  }

  bool enqueue(std::function<void()> command) {
    {
      const std::lock_guard<std::mutex> lock(command_mutex);
      if (stopping) {
        return false;
      }
      commands.push_back(std::move(command));
    }
    notify_ssh_event_fd(command_fd);
    return true;
  }

  template <typename T, typename Operation>
  cardio::promise<T> execute_async(Operation operation,
                                   bool notify_channel_activity,
                                   cardio::cancellation cancellation) {
    auto completion = std::make_shared<SshWorkerCompletion<T>>();
    const bool queued = enqueue(
        [this, completion, operation = std::move(operation),
         notify_channel_activity]() mutable {
          try {
            completion->value.emplace(operation());
          } catch (...) {
            completion->error = std::current_exception();
          }
          if (notify_channel_activity) {
            notify_ssh_event_fd(wakeup_fd);
          }
          notify_ssh_event_fd(completion->event_fd);
        });
    if (!queued) {
      throw std::runtime_error(_("SSH transport is closed"));
    }

    (void)co_await cardio::from_fd(
        completion->event_fd,
        cardio::fd_event::read | cardio::fd_event::error |
            cardio::fd_event::hangup,
        std::move(cancellation));
    drain_ssh_event_fd(completion->event_fd);
    if (completion->error) {
      std::rethrow_exception(completion->error);
    }
    co_return std::move(completion->value.value());
  }

  template <typename Operation>
  cardio::promise<SshWorkerResult>
  execute_ssh_async(Operation operation, bool notify_channel_activity,
                    cardio::cancellation cancellation) {
    co_return co_await execute_async<SshWorkerResult>(
        [this, operation = std::move(operation)]() mutable {
          const int value = operation();
          const char *detail = ssh_get_error(session);
          return SshWorkerResult{
              .value = value,
              .error_code = ssh_get_error_code(session),
              .poll_flags = ssh_get_poll_flags(session),
              .error =
                  detail == nullptr ? std::string() : std::string(detail),
          };
        },
        notify_channel_activity, std::move(cancellation));
  }

  cardio::promise<void>
  await_ready_async(int poll_flags, cardio::fd_event fallback,
                    cardio::cancellation cancellation) {
    cancellation.throw_if_cancellation_requested();
    if (socket_fd < 0 || wait_fd < 0) {
      co_return;
    }

    std::uint32_t socket_events = EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    if ((poll_flags & SSH_READ_PENDING) != 0) {
      socket_events |= EPOLLIN;
    }
    if ((poll_flags & SSH_WRITE_PENDING) != 0) {
      socket_events |= EPOLLOUT;
    }
    if ((socket_events & (EPOLLIN | EPOLLOUT)) == 0) {
      if ((fallback & cardio::fd_event::write) != cardio::fd_event::none) {
        socket_events |= EPOLLOUT;
      } else {
        socket_events |= EPOLLIN;
      }
    }

    epoll_event socket_event = {};
    socket_event.events = socket_events;
    socket_event.data.fd = socket_fd;
    if (::epoll_ctl(wait_fd, EPOLL_CTL_MOD, socket_fd,
                    &socket_event) < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "epoll_ctl failed for SSH socket");
    }

    drain_ssh_event_fd(wakeup_fd);
    (void)co_await cardio::from_fd(
        wait_fd,
        cardio::fd_event::read | cardio::fd_event::error |
            cardio::fd_event::hangup,
        std::move(cancellation));

    std::array<epoll_event, 2> events = {};
    int event_count = -1;
    do {
      event_count = ::epoll_wait(wait_fd, events.data(),
                                 static_cast<int>(events.size()), 0);
    } while (event_count < 0 && errno == EINTR);
    if (event_count < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "epoll_wait failed for SSH transport");
    }
    drain_ssh_event_fd(wakeup_fd);
  }

  void close_channel(ssh_channel channel) {
    if (channel == nullptr) {
      return;
    }
    (void)enqueue([this, channel]() {
      if (ssh_channel_is_open(channel) != 0) {
        (void)ssh_channel_send_eof(channel);
        (void)ssh_channel_close(channel);
      }
      ssh_channel_free(channel);
      const auto iterator =
          std::find(channels.begin(), channels.end(), channel);
      if (iterator != channels.end()) {
        channels.erase(iterator);
      }
      notify_ssh_event_fd(wakeup_fd);
    });
  }

  void close_session_on_worker() {
    for (ssh_channel channel : channels) {
      if (channel != nullptr) {
        if (ssh_channel_is_open(channel) != 0) {
          (void)ssh_channel_send_eof(channel);
          (void)ssh_channel_close(channel);
        }
        ssh_channel_free(channel);
      }
    }
    channels.clear();
    if (session != nullptr) {
      ssh_disconnect(session);
      ssh_free(session);
      session = nullptr;
    }
  }

  void stop_worker() {
    if (!worker.joinable()) {
      close_session_on_worker();
      return;
    }
    {
      const std::lock_guard<std::mutex> lock(command_mutex);
      stopping = true;
    }
    notify_ssh_event_fd(command_fd);
    worker.join();
  }
};

static std::runtime_error
ssh_worker_failure(const SshWorkerResult &result,
                   const std::string &operation) {
  return result.error.empty()
             ? std::runtime_error(operation)
             : std::runtime_error(operation + ": " + result.error);
}

template <typename Operation>
cardio::promise<void> await_transport_ok_async(
    const std::shared_ptr<AuthenticatedSshTransport> &transport,
    Operation operation, const std::string &description,
    bool notify_channel_activity, cardio::cancellation cancellation) {
  for (;;) {
    cancellation.throw_if_cancellation_requested();
    const SshWorkerResult result =
        co_await transport->impl->execute_ssh_async(
            operation, notify_channel_activity, cancellation);
    if (result.value == SSH_OK) {
      co_return;
    }
    if (!ssh_worker_result_is_again(result)) {
      throw ssh_worker_failure(result, description);
    }
    co_await transport->impl->await_ready_async(
        result.poll_flags,
        cardio::fd_event::read | cardio::fd_event::write,
        cancellation);
  }
}

cardio::promise<void> flush_transport_async(
    const std::shared_ptr<AuthenticatedSshTransport> &transport,
    cardio::cancellation cancellation) {
  co_await await_transport_ok_async(
      transport,
      [transport]() {
        return ssh_blocking_flush(transport->impl->session, 0);
      },
      "Failed to flush SSH output", true, std::move(cancellation));
}

AuthenticatedSshTransport::AuthenticatedSshTransport(
    std::unique_ptr<Impl> impl)
    : impl(std::move(impl)) {
}

AuthenticatedSshTransport::~AuthenticatedSshTransport() = default;

cardio::promise<std::shared_ptr<AuthenticatedSshTransport>>
AuthenticatedSshTransport::connect_async(
    const SshEndpointSettings &settings,
    const TerminalSessionCallbacks &callbacks,
    AuthenticatedSshTransportOptions options,
    cardio::cancellation cancellation) {
  cardio::io_uring io(64);
  int socket_fd = -1;
  auto transport_impl = std::make_unique<Impl>();
  transport_impl->endpoint = settings;
  transport_impl->session = ssh_new();
  if (transport_impl->session == nullptr) {
    throw std::runtime_error(_("Failed to allocate SSH session"));
  }
  ssh_session session = transport_impl->session;
  try {
    if (ssh_options_set(session, SSH_OPTIONS_HOST,
                        settings.address.c_str()) != SSH_OK) {
      throw ssh_failure(session, _("Failed to set SSH host"));
    }
    const char *config_file =
        options.config_file.empty() ? nullptr : options.config_file.c_str();
    if (ssh_options_parse_config(session, config_file) != SSH_OK) {
      throw ssh_failure(session, _("Failed to parse SSH configuration"));
    }
    const unsigned int port = static_cast<unsigned int>(settings.port);
    if (ssh_options_set(session, SSH_OPTIONS_HOST,
                        settings.address.c_str()) != SSH_OK ||
        ssh_options_set(session, SSH_OPTIONS_PORT, &port) != SSH_OK ||
        (!settings.username.empty() &&
         ssh_options_set(session, SSH_OPTIONS_USER,
                         settings.username.c_str()) != SSH_OK)) {
      throw ssh_failure(session, _("Failed to configure SSH endpoint"));
    }
    const std::string username = co_await request_ssh_username_async(
        session, settings, callbacks, cancellation);
    if (ssh_options_set(session, SSH_OPTIONS_USER,
                        username.c_str()) != SSH_OK) {
      throw ssh_failure(session, _("Failed to configure SSH user name"));
    }
    transport_impl->endpoint.username = username;
    if (!options.known_hosts_file.empty() &&
        ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS,
                        options.known_hosts_file.c_str()) != SSH_OK) {
      throw ssh_failure(session, _("Failed to configure SSH known_hosts"));
    }
    socket_fd = co_await connect_tcp_socket_async(
        io, settings.address, static_cast<std::uint16_t>(settings.port),
        cancellation);
    if (ssh_options_set(session, SSH_OPTIONS_FD, &socket_fd) != SSH_OK) {
      throw ssh_failure(session, _("Failed to attach SSH socket"));
    }
    socket_fd = -1;

    transport_impl->session_callbacks.userdata = nullptr;
    transport_impl->session_callbacks.auth_function =
        reject_libssh_terminal_prompt;
    ssh_callbacks_init(&transport_impl->session_callbacks);
    if (ssh_set_callbacks(session,
                          &transport_impl->session_callbacks) != SSH_OK) {
      throw ssh_failure(session, _("Failed to configure SSH callbacks"));
    }
    ssh_set_blocking(session, 0);
    co_await await_ssh_ok_async(
        session, [session]() { return ssh_connect(session); },
        "Failed to establish SSH transport", cancellation);

    if (callbacks.connection_phase) {
      callbacks.connection_phase(
          TerminalSessionConnectionPhase::verifying_host);
    }
    co_await verify_host_key_async(session, transport_impl->endpoint,
                                   callbacks, cancellation);

    if (callbacks.connection_phase) {
      callbacks.connection_phase(
          TerminalSessionConnectionPhase::authenticating);
    }
    co_await authenticate_session_async(
        session, transport_impl->endpoint, callbacks, cancellation);

    transport_impl->initialize_waiter();
    transport_impl->start_worker();
  } catch (...) {
    if (socket_fd >= 0) {
      (void)::close(socket_fd);
    }
    throw;
  }

  co_return std::shared_ptr<AuthenticatedSshTransport>(
      new AuthenticatedSshTransport(std::move(transport_impl)));
}

cardio::promise<bool> AuthenticatedSshTransport::is_connected_async(
    cardio::cancellation cancellation) {
  const std::shared_ptr<AuthenticatedSshTransport> owner =
      shared_from_this();
  (void)owner;
  co_return co_await impl->execute_async<bool>(
      [this]() {
        return impl->session != nullptr &&
               ssh_is_connected(impl->session) != 0;
      },
      false, std::move(cancellation));
}

const SshEndpointSettings &
AuthenticatedSshTransport::endpoint_settings() const noexcept {
  return impl->endpoint;
}

cardio::promise<void>
AuthenticatedSshTransport::execute_serialized_async(
    std::function<void(ssh_session)> operation,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  const std::shared_ptr<AuthenticatedSshTransport> owner =
      shared_from_this();
  (void)co_await impl->execute_async<bool>(
      [owner, operation = std::move(operation)]() {
        if (owner->impl->session == nullptr) {
          throw std::runtime_error(_("SSH transport is closed"));
        }
        operation(owner->impl->session);
        return true;
      },
      true, {});
  cancellation.throw_if_cancellation_requested();
}

bool AuthenticatedSshTransport::enqueue_serialized(
    std::function<void(ssh_session)> operation) noexcept {
  if (impl == nullptr) {
    return false;
  }
  try {
    return impl->enqueue(
        [this, operation = std::move(operation)]() {
          try {
            if (impl->session != nullptr) {
              operation(impl->session);
            }
          } catch (...) {
          }
          notify_ssh_event_fd(impl->wakeup_fd);
        });
  } catch (...) {
    return false;
  }
}

bool AuthenticatedSshTransport::try_begin_sftp_transfer() noexcept {
  bool expected = false;
  return impl != nullptr &&
         impl->sftp_transfer_active.compare_exchange_strong(
             expected, true, std::memory_order_acq_rel);
}

void AuthenticatedSshTransport::end_sftp_transfer() noexcept {
  if (impl != nullptr) {
    impl->sftp_transfer_active.store(false, std::memory_order_release);
  }
}

struct SshChannelConnection::Impl {
  std::shared_ptr<AuthenticatedSshTransport> transport;
  ssh_channel channel = nullptr;
  bool closed = false;

  ~Impl() {
    close();
  }

  void close() {
    if (closed) {
      return;
    }
    closed = true;
    if (transport != nullptr && transport->impl != nullptr) {
      transport->impl->close_channel(channel);
    }
    channel = nullptr;
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
  std::shared_ptr<AuthenticatedSshTransport> transport =
      co_await AuthenticatedSshTransport::connect_async(
          settings.endpoint, callbacks, std::move(options), cancellation);
  std::unique_ptr<SshChannelConnection> connection;
  {
    auto open_promise = open_async(
        std::move(transport), settings.terminal_type, columns, rows, callbacks,
        std::move(cancellation));
    connection = std::move(co_await open_promise);
  }
  co_return std::move(connection);
}

cardio::promise<std::unique_ptr<SshChannelConnection>>
SshChannelConnection::open_async(
    std::shared_ptr<AuthenticatedSshTransport> transport,
    std::string terminal_type, glong columns, glong rows,
    const TerminalSessionCallbacks &callbacks,
    cardio::cancellation cancellation) {
  if (transport == nullptr || transport->impl == nullptr) {
    throw std::invalid_argument(_("SSH transport is required"));
  }
  if (callbacks.connection_phase) {
    callbacks.connection_phase(
        TerminalSessionConnectionPhase::opening_shell);
  }

  auto connection_impl = std::make_unique<Impl>();
  connection_impl->transport = transport;
  connection_impl->channel =
      co_await transport->impl->execute_async<ssh_channel>(
          [transport]() {
            ssh_channel channel = ssh_channel_new(transport->impl->session);
            if (channel == nullptr) {
              throw ssh_failure(transport->impl->session,
                                "Failed to allocate SSH channel");
            }
            transport->impl->channels.push_back(channel);
            return channel;
          },
          false, cancellation);
  const ssh_channel channel = connection_impl->channel;

  try {
    co_await await_transport_ok_async(
        transport, [channel]() { return ssh_channel_open_session(channel); },
        "Failed to open SSH session channel", false, cancellation);
    co_await await_transport_ok_async(
        transport,
        [channel, terminal_type, columns, rows]() {
          return ssh_channel_request_pty_size(
              channel, terminal_type.c_str(),
              clamped_pty_dimension(columns),
              clamped_pty_dimension(rows));
        },
        "Failed to request SSH PTY", false, cancellation);
    co_await await_transport_ok_async(
        transport, [channel]() { return ssh_channel_request_shell(channel); },
        "Failed to request SSH shell", false, cancellation);
    co_await flush_transport_async(transport, cancellation);
  } catch (...) {
    connection_impl->close();
    throw;
  }

  co_return std::unique_ptr<SshChannelConnection>(
      new SshChannelConnection(std::move(connection_impl)));
}

cardio::promise<std::size_t>
SshChannelConnection::read_async(std::span<unsigned char> buffer,
                                 cardio::cancellation cancellation) {
  if (impl == nullptr || impl->closed || impl->transport == nullptr ||
      impl->channel == nullptr) {
    throw std::runtime_error(_("SSH channel is closed"));
  }
  if (buffer.empty()) {
    co_return 0;
  }

  const std::shared_ptr<AuthenticatedSshTransport> transport =
      impl->transport;
  const ssh_channel channel = impl->channel;
  const std::uint32_t capacity = static_cast<std::uint32_t>(
      std::min<std::size_t>(buffer.size(),
                            std::numeric_limits<std::uint32_t>::max()));
  for (;;) {
    cancellation.throw_if_cancellation_requested();
    if (impl->closed) {
      co_return 0;
    }
    const SshChannelReadResult result =
        co_await transport->impl->execute_async<SshChannelReadResult>(
            [transport, channel, buffer, capacity]() {
              /*
               * Check the PTY's normal data stream last. A libssh read for one
               * stream can consume packets for the other stream; checking
               * stdout last observes data buffered by the stderr check.
               */
              for (int is_stderr : {1, 0}) {
                const int read_size = ssh_channel_read_nonblocking(
                    channel, buffer.data(), capacity, is_stderr);
                if (read_size > 0) {
                  return SshChannelReadResult{
                      .size = read_size,
                      .poll_flags =
                          ssh_get_poll_flags(transport->impl->session),
                      .eof = false,
                  };
                }
                if (read_size == SSH_ERROR &&
                    !ssh_result_is_again(transport->impl->session,
                                         read_size)) {
                  throw ssh_failure(transport->impl->session,
                                    "Failed to read SSH channel");
                }
              }
              return SshChannelReadResult{
                  .size = SSH_AGAIN,
                  .poll_flags =
                      ssh_get_poll_flags(transport->impl->session),
                  .eof = ssh_channel_is_eof(channel) != 0 ||
                         ssh_channel_is_closed(channel) != 0 ||
                         ssh_channel_is_open(channel) == 0,
              };
            },
            false, cancellation);
    if (result.size > 0) {
      co_return static_cast<std::size_t>(result.size);
    }
    if (result.eof) {
      co_return 0;
    }
    co_await transport->impl->await_ready_async(
        result.poll_flags, cardio::fd_event::read, cancellation);
  }
}

cardio::promise<void> SshChannelConnection::write_all_async(
    std::span<const unsigned char> bytes,
    cardio::cancellation cancellation) {
  if (impl == nullptr || impl->closed || impl->transport == nullptr ||
      impl->channel == nullptr) {
    throw std::runtime_error(_("SSH channel is closed"));
  }

  const std::shared_ptr<AuthenticatedSshTransport> transport =
      impl->transport;
  const ssh_channel channel = impl->channel;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    cancellation.throw_if_cancellation_requested();
    const std::uint32_t chunk_size = static_cast<std::uint32_t>(
        std::min<std::size_t>(
            bytes.size() - offset,
            std::numeric_limits<std::uint32_t>::max()));
    const SshWorkerResult result =
        co_await transport->impl->execute_ssh_async(
            [channel, bytes, offset, chunk_size]() {
              return ssh_channel_write(channel, bytes.data() + offset,
                                       chunk_size);
            },
            true, cancellation);
    if (result.value > 0) {
      offset += static_cast<std::size_t>(result.value);
      continue;
    }
    if (!ssh_worker_result_is_again(result) && result.value != 0) {
      throw ssh_worker_failure(result, "Failed to write SSH channel");
    }
    co_await transport->impl->await_ready_async(
        result.poll_flags, cardio::fd_event::write, cancellation);
  }
  co_await flush_transport_async(transport, std::move(cancellation));
}

cardio::promise<void>
SshChannelConnection::resize_async(glong columns, glong rows,
                                   cardio::cancellation cancellation) {
  if (impl == nullptr || impl->closed || impl->transport == nullptr ||
      impl->channel == nullptr) {
    throw std::runtime_error(_("SSH channel is closed"));
  }
  const std::shared_ptr<AuthenticatedSshTransport> transport =
      impl->transport;
  const ssh_channel channel = impl->channel;
  co_await await_transport_ok_async(
      transport,
      [channel, columns, rows]() {
        return ssh_channel_change_pty_size(
            channel, clamped_pty_dimension(columns),
            clamped_pty_dimension(rows));
      },
      "Failed to resize SSH PTY", true, cancellation);
  co_await flush_transport_async(transport, std::move(cancellation));
}

cardio::promise<void> SshChannelConnection::send_break_async(
    std::uint32_t duration_ms, cardio::cancellation cancellation) {
  if (impl == nullptr || impl->closed || impl->transport == nullptr ||
      impl->channel == nullptr) {
    throw std::runtime_error(_("SSH channel is closed"));
  }
  const std::shared_ptr<AuthenticatedSshTransport> transport =
      impl->transport;
  const ssh_channel channel = impl->channel;
  co_await await_transport_ok_async(
      transport,
      [channel, duration_ms]() {
        return ssh_channel_request_send_break(channel, duration_ms);
      },
      "Failed to send SSH BREAK", true, cancellation);
  co_await flush_transport_async(transport, std::move(cancellation));
}

std::shared_ptr<AuthenticatedSshTransport>
SshChannelConnection::authenticated_transport() const {
  return impl == nullptr ? nullptr : impl->transport;
}

void SshChannelConnection::close() {
  if (impl != nullptr) {
    impl->close();
  }
}

} // namespace elder_terms

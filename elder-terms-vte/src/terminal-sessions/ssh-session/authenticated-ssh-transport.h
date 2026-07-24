#pragma once

#include <functional>
#include <memory>
#include <string>

#include <cardio.h>
#include <libssh/libssh.h>

#include <elder-terms/settings.h>

#include "../../terminal-session-callbacks.h"

namespace elder_terms {

class SshChannelConnection;
class LibsshSftpClient;

/**
 * Connection-layer options that do not belong to a saved SSH profile.
 */
struct AuthenticatedSshTransportOptions {
  /**
   * Explicit known_hosts file, or an empty string to use libssh defaults.
   */
  std::string known_hosts_file;
};

/**
 * Authenticated SSH session whose libssh calls are serialized on one worker.
 */
class AuthenticatedSshTransport
    : public std::enable_shared_from_this<AuthenticatedSshTransport> {
private:
  friend class SshChannelConnection;
  friend class LibsshSftpClient;
  template <typename Operation>
  friend cardio::promise<void> await_transport_ok_async(
      const std::shared_ptr<AuthenticatedSshTransport> &transport,
      Operation operation, const std::string &description,
      bool notify_channel_activity, cardio::cancellation cancellation);
  friend cardio::promise<void> flush_transport_async(
      const std::shared_ptr<AuthenticatedSshTransport> &transport,
      cardio::cancellation cancellation);

  struct Impl;
  std::unique_ptr<Impl> impl;

  explicit AuthenticatedSshTransport(std::unique_ptr<Impl> impl);

  cardio::promise<void> execute_serialized_async(
      std::function<void(ssh_session)> operation,
      cardio::cancellation cancellation);
  bool enqueue_serialized(
      std::function<void(ssh_session)> operation) noexcept;
  bool try_begin_sftp_transfer() noexcept;
  void end_sftp_transfer() noexcept;

public:
  /**
   * Disconnects the SSH session after its last shared owner releases it.
   */
  ~AuthenticatedSshTransport();

  AuthenticatedSshTransport(const AuthenticatedSshTransport &) = delete;
  AuthenticatedSshTransport &
  operator=(const AuthenticatedSshTransport &) = delete;

  /**
   * Connects and authenticates one reusable SSH transport.
   *
   * @param settings Effective SSH endpoint and login settings.
   * @param callbacks Session callbacks used for host-key and authentication
   * prompts.
   * @param options Connection-layer path overrides.
   * @param cancellation Connection cancellation signal.
   * @returns Authenticated shared transport.
   */
  static cardio::promise<std::shared_ptr<AuthenticatedSshTransport>>
  connect_async(const SshEndpointSettings &settings,
                const TerminalSessionCallbacks &callbacks,
                AuthenticatedSshTransportOptions options,
                cardio::cancellation cancellation);

  /**
   * Checks whether the authenticated SSH session remains connected.
   *
   * @param cancellation Operation cancellation signal.
   * @returns True while libssh reports an active transport.
   */
  cardio::promise<bool>
  is_connected_async(cardio::cancellation cancellation);

  /**
   * Returns the immutable endpoint used to authenticate this transport.
   *
   * @returns Effective SSH endpoint settings.
   */
  const SshEndpointSettings &endpoint_settings() const noexcept;
};

} // namespace elder_terms

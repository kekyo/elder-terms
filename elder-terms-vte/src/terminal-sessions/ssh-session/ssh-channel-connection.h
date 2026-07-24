#pragma once

#include <memory>
#include <span>
#include <string>

#include <cardio.h>

#include <elder-terms/settings.h>

#include "../../terminal-session-callbacks.h"
#include "authenticated-ssh-transport.h"

namespace elder_terms {

/**
 * Connection-layer options that do not belong to a saved SSH profile.
 */
using SshChannelConnectionOptions = AuthenticatedSshTransportOptions;

/**
 * Direct nonblocking libssh connection containing one interactive shell
 * channel.
 */
class SshChannelConnection {
private:
  struct Impl;
  std::unique_ptr<Impl> impl;

  explicit SshChannelConnection(std::unique_ptr<Impl> impl);

public:
  /**
   * Releases the SSH channel, session, and owned TCP socket.
   */
  ~SshChannelConnection();

  SshChannelConnection(const SshChannelConnection &) = delete;
  SshChannelConnection &operator=(const SshChannelConnection &) = delete;

  /**
   * Opens a cardio-connected TCP socket, completes SSH authentication, and
   * requests an interactive remote shell.
   *
   * @param settings Effective SSH endpoint and terminal settings.
   * @param columns Initial remote PTY width.
   * @param rows Initial remote PTY height.
   * @param callbacks Session callbacks, including the overlay prompt callback.
   * @param options Connection-layer path overrides.
   * @param cancellation Session cancellation signal.
   * @returns Connected shell channel.
   */
  static cardio::promise<std::unique_ptr<SshChannelConnection>> connect_async(
      const SshConnectionSettings &settings, glong columns, glong rows,
      const TerminalSessionCallbacks &callbacks,
      SshChannelConnectionOptions options,
      cardio::cancellation cancellation);

  /**
   * Opens an interactive shell on an existing authenticated transport.
   *
   * @param transport Shared authenticated SSH transport.
   * @param terminal_type Terminal type sent with the remote PTY request.
   * @param columns Initial remote PTY width.
   * @param rows Initial remote PTY height.
   * @param callbacks Session callbacks used for connection phase reporting.
   * @param cancellation Operation cancellation signal.
   * @returns Connected shell channel.
   */
  static cardio::promise<std::unique_ptr<SshChannelConnection>> open_async(
      std::shared_ptr<AuthenticatedSshTransport> transport,
      std::string terminal_type, glong columns, glong rows,
      const TerminalSessionCallbacks &callbacks,
      cardio::cancellation cancellation);

  /**
   * Reads available stdout or stderr bytes from the remote channel.
   *
   * @param buffer Destination buffer.
   * @param cancellation Operation cancellation signal.
   * @returns Number of bytes read, or zero after remote EOF.
   */
  cardio::promise<std::size_t>
  read_async(std::span<unsigned char> buffer,
             cardio::cancellation cancellation);

  /**
   * Writes an entire payload to the remote channel.
   *
   * @param bytes Payload to write.
   * @param cancellation Operation cancellation signal.
   */
  cardio::promise<void>
  write_all_async(std::span<const unsigned char> bytes,
                  cardio::cancellation cancellation);

  /**
   * Changes the remote PTY character dimensions.
   *
   * @param columns New width.
   * @param rows New height.
   * @param cancellation Operation cancellation signal.
   */
  cardio::promise<void> resize_async(glong columns, glong rows,
                                     cardio::cancellation cancellation);

  /**
   * Returns the authenticated transport shared by this shell channel.
   *
   * @returns Shared authenticated SSH transport.
   */
  std::shared_ptr<AuthenticatedSshTransport>
  authenticated_transport() const;

  /**
   * Closes the channel and underlying libssh session.
   */
  void close();
};

} // namespace elder_terms

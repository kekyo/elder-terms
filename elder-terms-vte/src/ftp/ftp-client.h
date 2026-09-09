#pragma once

#include <memory>
#include <string>

#include <cardio.h>
#include <elder-terms/settings/ftp-settings.h>

#include "../file-transfer/remote-file-client.h"

namespace elder_terms {

/** Options used to open one unencrypted FTP session. */
struct FtpClientOpenOptions {
  /** Stored endpoint, identity, and data-connection settings. */
  FtpConnectionSettings connection;
  /** Runtime-only password, or empty when the server accepts one. */
  std::string password;
};

/**
 * Opens and authenticates one unencrypted FTP file service session.
 *
 * @param options Endpoint and runtime authentication values.
 * @param cancellation Operation cancellation signal.
 * @returns Initialized remote file client with serialized logical operations.
 *
 * @remarks FTPS is intentionally unsupported. The username must be explicit;
 * anonymous login uses the literal username `anonymous` supplied by the user.
 */
cardio::promise<std::shared_ptr<RemoteFileClient>>
open_ftp_client_async(FtpClientOpenOptions options,
                      cardio::cancellation cancellation);

/**
 * Stops a libcurl FTP client and waits for its worker and pending operations.
 *
 * @param client Client returned by open_ftp_client_async().
 * @returns Completion after the worker and pending operations have ended.
 * @remarks Keep the caller dispatcher alive until this completes. Repeated
 * calls are permitted; new file operations fail after stopping begins.
 * @throws std::invalid_argument if client is not a libcurl FTP client.
 */
cardio::promise<void>
stop_ftp_client_async(std::shared_ptr<RemoteFileClient> client);

} // namespace elder_terms

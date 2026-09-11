#pragma once

#include <functional>
#include <memory>
#include <string>

#include <cardio.h>
#include <elder-terms/settings/ftp-settings.h>

#include "../file-transfer/remote-file-client.h"

namespace elder_terms {

/** TLS-protected FTP channel presenting a certificate. */
enum class FtpTlsChannel {
  /** Authentication and file-operation commands. */
  control,
  /** Directory contents and file bytes. */
  data,
};

/** Immutable certificate failure copied from the TLS worker. */
struct FtpCertificateFailure {
  /** Requested server name or numeric address. */
  std::string address;
  /** Control connection port. */
  gint64 port;
  /** Channel whose handshake was rejected. */
  FtpTlsChannel channel;
  /** TLS backend validation identifier, independent of its display string. */
  int validation_code;
  /** Failing chain depth; zero is the leaf. */
  int depth;
  /** Human-readable failure reason. */
  std::string reason;
  /** Leaf subject, or empty when unavailable. */
  std::string subject;
  /** Leaf issuer, or empty when unavailable. */
  std::string issuer;
  /** Leaf validity start, or empty when unavailable. */
  std::string not_before;
  /** Leaf validity end, or empty when unavailable. */
  std::string not_after;
  /** SHA-256 fingerprint identifying the presented leaf certificate. */
  std::string sha256;
  /** SHA-256 of the chain certificate associated with the failure. */
  std::string failed_certificate_sha256;
};

/** Options used to open one FTP or FTPS session. */
struct FtpClientOpenOptions {
  /** Stored endpoint, identity, and data-connection settings. */
  FtpConnectionSettings connection;
  /** Runtime-only password, or empty when the server accepts one. */
  std::string password;
  /** Called on the caller dispatcher after a failed handshake ends; true approves only this failure. */
  std::function<cardio::promise<bool>(const FtpCertificateFailure &, cardio::cancellation)> confirm_certificate{};
};

/**
 * Opens and authenticates one FTP or FTPS file service session.
 *
 * @param options Endpoint and runtime authentication values.
 * @param cancellation Operation cancellation signal.
 * @returns Initialized remote file client with serialized logical operations.
 *
 * @remarks The username must be explicit;
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

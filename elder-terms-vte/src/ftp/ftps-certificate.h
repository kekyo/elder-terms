#pragma once

#include <array>
#include <exception>
#include <optional>
#include <set>
#include <tuple>

#include <curl/curl.h>

#include "ftp-client.h"

namespace elder_terms {

/** Most recent certificate and explicitly accepted errors for one channel. */
struct FtpCertificateIdentity {
  /** SHA-256 of the leaf certificate last observed on this channel. */
  std::string fingerprint;
  /** Backend error, chain depth, and fingerprint of the failing certificate. */
  std::set<std::tuple<int, int, std::string>> errors;
};

/** Certificate state owned exclusively by one curl worker. */
struct FtpCertificatePolicy {
  /** Requested endpoint shown in confirmation requests. */
  std::string address;
  /** Normalized A-label hostname or numeric address used for verification. */
  std::string hostname;
  /** Control port; data sockets belong to this immutable endpoint. */
  gint64 port;
  /** Most recently observed control and data identities. */
  std::array<FtpCertificateIdentity, 2> identities;
  /** Channel currently performing a TLS handshake. */
  FtpTlsChannel channel = FtpTlsChannel::control;
  /** Unapproved verification failure copied before ending the handshake. */
  std::optional<FtpCertificateFailure> failure;
  /** Exceptions captured at the C callback boundary. */
  std::exception_ptr callback_failure;
};

/**
 * Creates a session-local policy and verifies the linked OpenSSL backend.
 * @param connection Immutable endpoint and TLS settings.
 * @returns Policy with no certificate exceptions.
 */
std::shared_ptr<FtpCertificatePolicy> create_ftp_certificate_policy(
    const FtpConnectionSettings &connection);

/**
 * Installs peer verification on an OpenSSL context supplied by libcurl.
 * @param easy libcurl handle, unused by the callback.
 * @param context Borrowed SSL_CTX, never retained.
 * @param data Worker-owned FtpCertificatePolicy.
 * @returns libcurl status; C++ exceptions never cross this boundary.
 */
CURLcode configure_ftp_certificate_context(CURL *easy, void *context, void *data) noexcept;

/**
 * Applies one explicit confirmation on the owning worker.
 * @param policy Policy for the same logical connection.
 * @param failure Exact immutable failure shown to the user.
 */
void approve_ftp_certificate_failure(FtpCertificatePolicy &policy,
                                    const FtpCertificateFailure &failure);

} // namespace elder_terms

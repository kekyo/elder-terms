#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <curl/curl.h>
#include "certificate-failure.h"

namespace elder_terms {

/** Last observed certificate and explicitly accepted errors for one identity. */
struct TlsCertificateIdentity {
  /** SHA-256 of the last observed leaf certificate. */
  std::string fingerprint;
  /** Backend error, chain depth, and fingerprint of the failing certificate. */
  std::set<std::tuple<int, int, std::string>> errors;
};

/** Certificate state owned by one logical session and execution context. */
struct TlsCertificatePolicy {
  /** Requested endpoint shown in confirmation requests. */
  std::string address;
  /** Normalized A-label hostname or numeric address used for verification. */
  std::string hostname;
  /** Port of the immutable logical endpoint. */
  std::int64_t port;
  /** Most recently observed identities; FTP uses two and HTTPS uses one. */
  std::vector<TlsCertificateIdentity> identities;
  /** Identity currently performing a TLS handshake. */
  std::size_t identity_slot = 0;
  /** Unapproved verification failure copied before ending the handshake. */
  std::optional<TlsCertificateFailure> failure;
  /** Exceptions captured at the C callback boundary. */
  std::exception_ptr callback_failure;
};

/**
 * Creates a session-local policy and verifies the linked OpenSSL backend.
 * @param address Immutable endpoint name or numeric address.
 * @param port Port identifying the logical endpoint.
 * @param scheme URL scheme used by this service.
 * @param identity_count Number of identities: one for HTTPS, two for FTPS.
 * @returns Policy with no certificate exceptions.
 * @throws std::invalid_argument for an unsupported identity count or endpoint.
 */
std::shared_ptr<TlsCertificatePolicy> create_tls_certificate_policy(
    std::string address, std::int64_t port, const std::string &scheme,
    std::size_t identity_count);

/**
 * Installs peer verification on an OpenSSL context supplied by libcurl.
 * @param easy libcurl handle, unused by the callback.
 * @param context Borrowed SSL_CTX, never retained.
 * @param data Session-owned TlsCertificatePolicy.
 * @returns libcurl status; C++ exceptions never cross this boundary.
 */
CURLcode configure_tls_certificate_context(CURL *easy, void *context, void *data) noexcept;

/**
 * Applies one explicit confirmation in the owning execution context.
 * @param policy Policy for the same logical connection.
 * @param failure Exact immutable failure shown to the user.
 */
void approve_tls_certificate_failure(TlsCertificatePolicy &policy,
                                    const TlsCertificateFailure &failure);

} // namespace elder_terms

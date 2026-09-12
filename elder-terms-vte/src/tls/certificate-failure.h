#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace elder_terms {

/** Immutable certificate failure copied before a TLS handshake ends. */
struct TlsCertificateFailure {
  /** Requested server name or numeric address. */
  std::string address;
  /** Port of the immutable logical endpoint. */
  std::int64_t port;
  /** Index of the connection identity whose handshake was rejected. */
  std::size_t identity_slot;
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

} // namespace elder_terms

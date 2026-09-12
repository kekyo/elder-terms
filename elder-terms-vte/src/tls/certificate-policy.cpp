#include "certificate-policy.h"

#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>

namespace elder_terms {

static int policy_index() {
  static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  if (index < 0) throw std::runtime_error("Cannot allocate TLS certificate context");
  return index;
}

static std::string fingerprint(X509 *certificate) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
  unsigned length = 0;
  if (!certificate || X509_digest(certificate, EVP_sha256(), bytes.data(), &length) != 1 || length != 32)
    throw std::runtime_error("Cannot read TLS certificate fingerprint");
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string text;
  for (unsigned index = 0; index < length; ++index) {
    if (index) text += ':';
    text += hex[bytes[index] >> 4];
    text += hex[bytes[index] & 15];
  }
  return text;
}

static std::string bio_text(BIO *bio) {
  char *data = nullptr;
  const long length = BIO_get_mem_data(bio, &data);
  return length > 0 && data ? std::string(data, std::min<long>(length, 8192)) : std::string();
}

static std::string certificate_name(const X509_NAME *name) {
  const auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), BIO_free);
  if (!bio) throw std::bad_alloc();
  if (!name || X509_NAME_print_ex(bio.get(), name, 0, XN_FLAG_RFC2253) < 0) return {};
  return bio_text(bio.get());
}

static std::string certificate_time(const ASN1_TIME *time) {
  const auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), BIO_free);
  if (!bio) throw std::bad_alloc();
  if (!time || ASN1_TIME_print(bio.get(), time) != 1) return {};
  return bio_text(bio.get());
}

static bool can_confirm(int error) {
  // Cryptographic policy errors (weak keys/signatures), malformed certificates,
  // and internal verification failures are not relaxed by identity approval.
  switch (error) {
  case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
  case X509_V_ERR_CERT_NOT_YET_VALID:
  case X509_V_ERR_CERT_HAS_EXPIRED:
  case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
  case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
  case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
  case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
  case X509_V_ERR_CERT_UNTRUSTED:
  case X509_V_ERR_CERT_REJECTED:
  case X509_V_ERR_HOSTNAME_MISMATCH:
  case X509_V_ERR_IP_ADDRESS_MISMATCH:
    return true;
  default:
    return false;
  }
}

static std::string normalized_certificate_name(std::string value) {
  if (value.ends_with('.')) value.pop_back();
  // libcurl accepts only wildcards below at least two domain labels.
  if (value.find('*') != std::string::npos &&
      std::count(value.begin(), value.end(), '.') < 2) return {};
  return value;
}

static int hostname_error(X509 *certificate, const std::string &hostname) {
  std::array<unsigned char, 16> address{};
  const auto length = ::inet_pton(AF_INET, hostname.c_str(), address.data()) == 1 ? 4 :
      ::inet_pton(AF_INET6, hostname.c_str(), address.data()) == 1 ? 16 : 0;
  const auto mismatch = length ? X509_V_ERR_IP_ADDRESS_MISMATCH : X509_V_ERR_HOSTNAME_MISMATCH;
  if (hostname.empty() || hostname.front() == '.') return mismatch;

  // This private copy is used only for OpenSSL's name matcher. The original
  // certificate remains unchanged for chain validation and approval identity.
  // Normalize curl's SAN/CN precedence and trailing-dot/wildcard rules first.
  const auto copy = std::unique_ptr<X509, decltype(&X509_free)>(X509_dup(certificate), X509_free);
  if (!copy) throw std::bad_alloc();
  int critical = -1;
  const auto alternatives = std::unique_ptr<GENERAL_NAMES, decltype(&GENERAL_NAMES_free)>(
      static_cast<GENERAL_NAMES *>(X509_get_ext_d2i(copy.get(), NID_subject_alt_name, &critical, nullptr)),
      GENERAL_NAMES_free);
  if (!alternatives && critical != -1)
    throw std::runtime_error("Cannot decode TLS certificate alternative names");
  bool has_address_name = false;
  if (alternatives) {
    for (int index = sk_GENERAL_NAME_num(alternatives.get()) - 1; index >= 0; --index) {
      auto *name = sk_GENERAL_NAME_value(alternatives.get(), index);
      if (name->type == GEN_IPADD) has_address_name = true;
      if (name->type != GEN_DNS) continue;
      has_address_name = true;
      const auto *data = ASN1_STRING_get0_data(name->d.dNSName);
      const auto size = ASN1_STRING_length(name->d.dNSName);
      if (size < 0 || (!data && size)) throw std::runtime_error("Invalid TLS certificate name");
      const auto value = normalized_certificate_name(size ?
          std::string(reinterpret_cast<const char *>(data), size) : std::string());
      if (value.empty() || value.find('\0') != std::string::npos) {
        GENERAL_NAME_free(sk_GENERAL_NAME_delete(alternatives.get(), index));
      } else if (ASN1_STRING_set(name->d.dNSName, value.data(), static_cast<int>(value.size())) != 1) {
        throw std::bad_alloc();
      }
    }
    if (X509_add1_ext_i2d(copy.get(), NID_subject_alt_name, alternatives.get(), critical,
                         X509V3_ADD_REPLACE_EXISTING) != 1)
      throw std::runtime_error("Cannot normalize TLS certificate alternative names");
  }

  if (!has_address_name) {
    auto *subject = X509_get_subject_name(copy.get());
    int last = -1;
    for (;;) {
      const auto next = X509_NAME_get_index_by_NID(subject, NID_commonName, last);
      if (next < 0) break;
      last = next;
    }
    if (last < 0) return mismatch;
    auto *entry = X509_NAME_get_entry(subject, last);
    unsigned char *converted = nullptr;
    const auto size = ASN1_STRING_to_UTF8(&converted, X509_NAME_ENTRY_get_data(entry));
    const auto release = [](unsigned char *value) { OPENSSL_free(value); };
    const auto owned = std::unique_ptr<unsigned char, decltype(release)>(converted, release);
    if (size < 0) throw std::runtime_error("Cannot decode TLS certificate common name");
    const auto value = normalized_certificate_name(size ?
        std::string(reinterpret_cast<const char *>(converted), size) : std::string());
    if (value.empty() || value.find('\0') != std::string::npos) return mismatch;
    // Retain only the last CN, as libcurl does when DNS/IP SANs are absent.
    for (;;) {
      const auto index = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
      if (index < 0) break;
      auto *removed = X509_NAME_delete_entry(subject, index);
      if (!removed) throw std::runtime_error("Cannot normalize TLS certificate subject");
      X509_NAME_ENTRY_free(removed);
    }
    if (X509_NAME_add_entry_by_NID(subject, NID_commonName, MBSTRING_UTF8,
        reinterpret_cast<const unsigned char *>(value.data()), static_cast<int>(value.size()), -1, 0) != 1)
      throw std::runtime_error("Cannot normalize TLS certificate common name");
  }
  const auto flags = X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS |
      (has_address_name ? X509_CHECK_FLAG_NEVER_CHECK_SUBJECT : 0) |
      (length ? X509_CHECK_FLAG_NO_WILDCARDS : 0);
  // Preserve libcurl's IP/DNS classification before normalizing a DNS dot.
  // A numeric name whose dot remains after URL parsing must not become an IP SAN target.
  const auto comparison = !length && hostname.ends_with('.')
      ? hostname.substr(0, hostname.size() - 1) : hostname;
  const auto matched = length && has_address_name
      ? X509_check_ip(copy.get(), address.data(), length, 0)
      : X509_check_host(copy.get(), comparison.c_str(), comparison.size(), flags, nullptr);
  if (matched < 0) throw std::runtime_error("Cannot verify TLS certificate hostname");
  return matched == 1 ? X509_V_OK : mismatch;
}

static bool accept_error(TlsCertificatePolicy &policy, X509_STORE_CTX *context,
                         X509 *leaf, int error, int depth) {
  auto &identity = policy.identities.at(policy.identity_slot);
  const auto leaf_fingerprint = fingerprint(leaf);
  if (identity.fingerprint != leaf_fingerprint) {
    auto errors = decltype(identity.errors){};
    if (identity.fingerprint.empty()) {
      const auto previous = std::find_if(policy.identities.begin(), policy.identities.end(),
          [&leaf_fingerprint](const auto &other) { return other.fingerprint == leaf_fingerprint; });
      if (previous != policy.identities.end()) errors = previous->errors;
    }
    identity = {.fingerprint = leaf_fingerprint, .errors = std::move(errors)};
  }
  if (error == X509_V_OK) return true;
  if (!can_confirm(error)) return false;
  auto *failed_certificate = X509_STORE_CTX_get_current_cert(context);
  const auto failed_fingerprint = fingerprint(depth == 0 ? leaf : failed_certificate);
  if (identity.errors.contains({error, depth, failed_fingerprint})) return true;
  policy.failure = TlsCertificateFailure{
      .address = policy.address, .port = policy.port, .identity_slot = policy.identity_slot,
      .validation_code = error, .depth = depth,
      .reason = X509_verify_cert_error_string(error),
      .subject = certificate_name(X509_get_subject_name(leaf)),
      .issuer = certificate_name(X509_get_issuer_name(leaf)),
      .not_before = certificate_time(X509_get0_notBefore(leaf)),
      .not_after = certificate_time(X509_get0_notAfter(leaf)),
      .sha256 = leaf_fingerprint, .failed_certificate_sha256 = failed_fingerprint};
  return false;
}

static int verify_certificate(int verified, X509_STORE_CTX *context) noexcept {
  TlsCertificatePolicy *policy = nullptr;
  try {
    auto *ssl = static_cast<SSL *>(X509_STORE_CTX_get_ex_data(context, SSL_get_ex_data_X509_STORE_CTX_idx()));
    if (!ssl) return 0;
    policy = static_cast<TlsCertificatePolicy *>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), policy_index()));
    if (!policy) return 0;
    auto *leaf = X509_STORE_CTX_get0_cert(context);
    if (!leaf) return 0;
    const auto depth = X509_STORE_CTX_get_error_depth(context);
    if (!verified) {
      const auto error = X509_STORE_CTX_get_error(context);
      if (!accept_error(*policy, context, leaf, error, depth)) return 0;
      // libcurl also examines SSL_get_verify_result after the handshake.
      // Clear only the particular failure already approved for this identity.
      X509_STORE_CTX_set_error(context, X509_V_OK);
    }
    if (depth == 0) {
      const auto error = hostname_error(leaf, policy->hostname);
      if (!accept_error(*policy, context, leaf, error, 0)) {
        X509_STORE_CTX_set_error(context, error);
        return 0;
      }
    }
    return 1;
  } catch (...) {
    if (policy) policy->callback_failure = std::current_exception();
    X509_STORE_CTX_set_error(context, X509_V_ERR_APPLICATION_VERIFICATION);
    return 0;
  }
}

std::shared_ptr<TlsCertificatePolicy> create_tls_certificate_policy(
    std::string address, std::int64_t port, const std::string &scheme,
    std::size_t identity_count) {
  if (identity_count != 1 && identity_count != 2)
    throw std::invalid_argument("Unsupported TLS certificate identity count");
  const auto *runtime = curl_version_info(CURLVERSION_NOW);
  const std::string_view linked(OpenSSL_version(OPENSSL_VERSION));
  const auto begin = linked.find(' ');
  const auto end = linked.find(' ', begin + 1);
  const auto expected = "OpenSSL/" + std::string(linked.substr(begin + 1, end - begin - 1));
  if (!runtime->ssl_version || runtime->ssl_version != expected)
    throw std::runtime_error("TLS certificate confirmation requires the same OpenSSL version as libcurl");
  const auto url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>(curl_url(), curl_url_cleanup);
  if (!url) throw std::bad_alloc();
  auto host = address;
  if (host.find(':') != std::string::npos && !host.starts_with('[')) host = '[' + host + ']';
  if (curl_url_set(url.get(), CURLUPART_HOST, host.c_str(), 0) != CURLUE_OK)
    throw std::invalid_argument("Invalid TLS certificate hostname");
  // Transfers parse the complete URL, which canonicalizes short/hexadecimal
  // IPv4 forms. Setting only CURLUPART_HOST does not perform that step.
  if (curl_url_set(url.get(), CURLUPART_SCHEME, scheme.c_str(), 0) != CURLUE_OK)
    throw std::invalid_argument("Cannot normalize TLS certificate hostname");
  char *endpoint = nullptr;
  const auto endpoint_code = curl_url_get(url.get(), CURLUPART_URL, &endpoint, 0);
  const auto owned_endpoint = std::unique_ptr<char, decltype(&curl_free)>(endpoint, curl_free);
  if (endpoint_code != CURLUE_OK ||
      curl_url_set(url.get(), CURLUPART_URL, endpoint, 0) != CURLUE_OK)
    throw std::invalid_argument("Cannot normalize TLS certificate hostname");
  char *text = nullptr;
  const auto code = curl_url_get(url.get(), CURLUPART_HOST, &text, CURLU_PUNYCODE);
  const auto owned = std::unique_ptr<char, decltype(&curl_free)>(text, curl_free);
  if (code != CURLUE_OK) throw std::invalid_argument("Cannot normalize TLS certificate hostname");
  host = text;
  if (host.starts_with('[') && host.ends_with(']')) host = host.substr(1, host.size() - 2);
  auto result = std::make_shared<TlsCertificatePolicy>();
  result->address = std::move(address);
  result->hostname = std::move(host);
  result->port = port;
  result->identities.resize(identity_count);
  return result;
}

CURLcode configure_tls_certificate_context(CURL *, void *context, void *data) noexcept {
  auto *policy = static_cast<TlsCertificatePolicy *>(data);
  try {
    auto *ssl_context = static_cast<SSL_CTX *>(context);
    if (SSL_CTX_set_ex_data(ssl_context, policy_index(), policy) != 1)
      throw std::runtime_error("Cannot configure TLS certificate verification");
    SSL_CTX_set_verify(ssl_context, SSL_VERIFY_PEER, verify_certificate);
    return CURLE_OK;
  } catch (...) {
    policy->callback_failure = std::current_exception();
    return CURLE_ABORTED_BY_CALLBACK;
  }
}

void approve_tls_certificate_failure(TlsCertificatePolicy &policy,
                                    const TlsCertificateFailure &failure) {
  if (failure.address != policy.address || failure.port != policy.port ||
      failure.identity_slot >= policy.identities.size() || !can_confirm(failure.validation_code)) throw std::invalid_argument("Invalid TLS certificate approval");
  if (policy.identities[failure.identity_slot].fingerprint != failure.sha256)
    throw std::invalid_argument("TLS certificate changed before approval");
  for (auto &identity : policy.identities) {
    if (identity.fingerprint == failure.sha256)
      identity.errors.emplace(failure.validation_code, failure.depth, failure.failed_certificate_sha256);
  }
}

} // namespace elder_terms

#include "../../src/ftp/ftps-certificate.h"

#include <elder-terms/localization.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

// A memory BIO pair performs real TLS handshakes without timers, sockets, or
// workers. Each iteration delivers the preceding peer's handshake messages.
static bool handshake(elder_terms::FtpCertificatePolicy &policy,
                      const std::string &certificate, const std::string &key,
                      bool trusted, elder_terms::FtpTlsChannel channel) {
  policy.failure.reset();
  policy.callback_failure = nullptr;
  policy.channel = channel;
  const auto client_context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
  const auto server_context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
  expect(client_context && server_context, "Cannot allocate test TLS contexts");
  expect(SSL_CTX_use_certificate_chain_file(server_context.get(), certificate.c_str()) == 1 &&
         SSL_CTX_use_PrivateKey_file(server_context.get(), key.c_str(), SSL_FILETYPE_PEM) == 1,
         "Cannot load test TLS identity");
  if (trusted) expect(SSL_CTX_load_verify_locations(client_context.get(), certificate.c_str(), nullptr) == 1, "Cannot trust test certificate");
  expect(elder_terms::configure_ftp_certificate_context(nullptr, client_context.get(), &policy) == CURLE_OK,
         "Cannot configure certificate policy");
  const auto client = std::unique_ptr<SSL, decltype(&SSL_free)>(SSL_new(client_context.get()), SSL_free);
  const auto server = std::unique_ptr<SSL, decltype(&SSL_free)>(SSL_new(server_context.get()), SSL_free);
  expect(client && server, "Cannot allocate test TLS connections");
  BIO *client_bio = nullptr;
  BIO *server_bio = nullptr;
  expect(BIO_new_bio_pair(&client_bio, 0, &server_bio, 0) == 1, "Cannot allocate TLS transport");
  auto owned_client_bio = std::unique_ptr<BIO, decltype(&BIO_free)>(client_bio, BIO_free);
  auto owned_server_bio = std::unique_ptr<BIO, decltype(&BIO_free)>(server_bio, BIO_free);
  expect(BIO_up_ref(client_bio) == 1, "Cannot share client BIO");
  SSL_set0_rbio(client.get(), owned_client_bio.release());
  SSL_set0_wbio(client.get(), client_bio);
  expect(BIO_up_ref(server_bio) == 1, "Cannot share server BIO");
  SSL_set0_rbio(server.get(), owned_server_bio.release());
  SSL_set0_wbio(server.get(), server_bio);
  SSL_set_connect_state(client.get());
  SSL_set_accept_state(server.get());
  for (unsigned round = 0; round < 100; ++round) {
    for (auto *peer : {client.get(), server.get()}) {
      if (SSL_is_init_finished(peer)) continue;
      ERR_clear_error();
      const auto result = SSL_do_handshake(peer);
      if (result == 1) continue;
      const auto error = SSL_get_error(peer, result);
      if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
        if (policy.callback_failure) std::rethrow_exception(policy.callback_failure);
        expect(policy.failure.has_value(), "Unexpected non-certificate TLS failure");
        return false;
      }
    }
    if (SSL_is_init_finished(client.get()) && SSL_is_init_finished(server.get())) {
      expect(SSL_get_verify_result(client.get()) == X509_V_OK, "Approved verification errors must be cleared");
      return true;
    }
  }
  throw std::runtime_error("TLS fixture made no handshake progress");
}

int main(int argc, char **argv) {
  try {
    expect(argc == 6, "Expected hostname, certificate, key, trust, result or alternate certificate");
    // Match application startup: libcurl's IDN conversion uses the process locale.
    const auto localization = elder_terms::initialize_localization(elder_terms::ApplicationUiLanguage::system);
    expect(localization.requested_language_applied, "Cannot initialize the test locale");
    expect(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "Cannot initialize curl");
    const auto connection = elder_terms::FtpConnectionSettings{
        .address = argv[1], .port = 21, .username = "alice",
        .data_connection_mode = elder_terms::FtpDataConnectionMode::passive,
        .local_directory = {}, .remote_directory = {}, .tls_mode = elder_terms::FtpTlsMode::explicit_tls};
    auto policy = elder_terms::create_ftp_certificate_policy(connection);
    const std::string result = argv[5];
    const bool trusted = std::string(argv[4]) == "trusted";
    using elder_terms::FtpTlsChannel;
    const bool accepted = handshake(*policy, argv[2], argv[3], trusted, FtpTlsChannel::control);
    if (result == "success" || result == "failure") {
      expect(accepted == (result == "success"), "Unexpected hostname verification result");
    } else {
      expect(!accepted, "Untrusted certificate must require approval");
      const auto first = *policy->failure;
      elder_terms::approve_ftp_certificate_failure(*policy, first);
      expect(handshake(*policy, argv[2], argv[3], false, FtpTlsChannel::control), "Same control certificate must retain its approval");
      expect(handshake(*policy, argv[2], argv[3], false, FtpTlsChannel::data), "Same data certificate may use the control approval");
      expect(!handshake(*policy, result, argv[3], false, FtpTlsChannel::data), "Changed data certificate must prompt");
      expect(policy->failure->sha256 != first.sha256, "Changed certificate must have a distinct identity");
      elder_terms::approve_ftp_certificate_failure(*policy, *policy->failure);
      expect(handshake(*policy, result, argv[3], false, FtpTlsChannel::data), "Approved replacement must work");
      expect(!handshake(*policy, argv[2], argv[3], false, FtpTlsChannel::data), "Returning to an older certificate must prompt again");
      auto another = elder_terms::create_ftp_certificate_policy(connection);
      expect(!handshake(*another, argv[2], argv[3], false, FtpTlsChannel::control), "A new session must not inherit approval");
    }
    std::cout << "FTPS certificate policy PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FTPS certificate policy FAIL: " << error.what() << '\n';
    return 1;
  }
}

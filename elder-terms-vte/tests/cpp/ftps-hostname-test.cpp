#include "../../src/ftp/ftps-certificate.h"

#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

static void require(CURLcode code) {
  if (code != CURLE_OK) throw std::runtime_error(curl_easy_strerror(code));
}

static std::size_t discard_listing(char *, std::size_t size,
                                   std::size_t count, void *) {
  return size * count;
}

static void verify(int argc, char **argv) {
  if (argc != 7) throw std::invalid_argument("Expected mode, hostname, port, CA, policy and result");
  const bool implicit = std::string(argv[1]) == "implicit";
  const bool prompt = std::string(argv[5]) == "prompt";
  const std::string expectation(argv[6]);
  const bool expected = expectation == "success";
  const auto connection = elder_terms::FtpConnectionSettings{
      .address = argv[2], .port = std::stoll(argv[3]), .username = "alice",
      .data_connection_mode = elder_terms::FtpDataConnectionMode::passive,
      .local_directory = {}, .remote_directory = {},
      .tls_mode = implicit ? elder_terms::FtpTlsMode::implicit_tls : elder_terms::FtpTlsMode::explicit_tls};
  const auto policy = prompt ? elder_terms::create_ftp_certificate_policy(connection) : nullptr;
  // The dotted numeric DNS name need not exist in external DNS. Only this test
  // handle maps it to the fixture; URL identity and TLS verification stay intact.
  const auto mapping = connection.address + ':' + argv[3] + ":127.0.0.1";
  const auto resolves = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>(
      curl_slist_append(nullptr, mapping.c_str()), curl_slist_free_all);
  const auto easy = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(curl_easy_init(), curl_easy_cleanup);
  if (!easy || !resolves) throw std::bad_alloc();
  const auto url = std::string(implicit ? "ftps://" : "ftp://") +
      connection.address + ':' + argv[3] + "//home/";
  require(curl_easy_setopt(easy.get(), CURLOPT_URL, url.c_str()));
  require(curl_easy_setopt(easy.get(), CURLOPT_PROTOCOLS_STR, implicit ? "ftps" : "ftp"));
  require(curl_easy_setopt(easy.get(), CURLOPT_PROXY, ""));
  require(curl_easy_setopt(easy.get(), CURLOPT_RESOLVE, resolves.get()));
  require(curl_easy_setopt(easy.get(), CURLOPT_USERNAME, "alice"));
  require(curl_easy_setopt(easy.get(), CURLOPT_PASSWORD, "secret"));
  require(curl_easy_setopt(easy.get(), CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL)));
  require(curl_easy_setopt(easy.get(), CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2)));
  require(curl_easy_setopt(easy.get(), CURLOPT_CAINFO, argv[4]));
  require(curl_easy_setopt(easy.get(), CURLOPT_SSL_VERIFYPEER, 1L));
  require(curl_easy_setopt(easy.get(), CURLOPT_SSL_VERIFYHOST, prompt ? 0L : 2L));
  require(curl_easy_setopt(easy.get(), CURLOPT_CONNECTTIMEOUT, 30L));
  require(curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT, 60L));
  require(curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, discard_listing));
  if (policy) {
    require(curl_easy_setopt(easy.get(), CURLOPT_SSL_CTX_FUNCTION, elder_terms::configure_ftp_certificate_context));
    require(curl_easy_setopt(easy.get(), CURLOPT_SSL_CTX_DATA, policy.get()));
  }
  const auto result = curl_easy_perform(easy.get());
  if (policy && policy->callback_failure) std::rethrow_exception(policy->callback_failure);
  if (policy && policy->failure) std::cout << "CONFIRM control " << policy->failure->validation_code << '\n';
  std::cout << "RESULT curl " << result << '\n';
  const auto wanted = expected ? CURLE_OK : CURLE_PEER_FAILED_VERIFICATION;
  const bool accepted_result = expectation == "reference"
      ? result == CURLE_OK || result == CURLE_PEER_FAILED_VERIFICATION : result == wanted;
  if (!accepted_result) throw std::runtime_error("Unexpected hostname verification result: " + std::string(curl_easy_strerror(result)));
  if (prompt && policy->failure.has_value() != (result == CURLE_PEER_FAILED_VERIFICATION))
    throw std::runtime_error("Only invalid names should require confirmation");
}

int main(int argc, char **argv) {
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
  int status = 0;
  try {
    verify(argc, argv);
    std::cout << "FTPS hostname PASS\n";
  } catch (const std::exception &error) {
    std::cerr << "FTPS hostname FAIL: " << error.what() << '\n';
    status = 1;
  }
  curl_global_cleanup();
  return status;
}

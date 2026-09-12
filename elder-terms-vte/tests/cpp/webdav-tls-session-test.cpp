// Verify certificate approval boundaries across real HTTPS handshakes.
#include "curl-http-session.h"

#include <iostream>
#include <stdexcept>
#include <unistd.h>

struct CertificateConfirmations {
  unsigned count = 0;
  std::string previous;
};

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<bool> accept_certificate_async(
    CertificateConfirmations *state, std::string fingerprint,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  expect(!fingerprint.empty() && fingerprint != state->previous,
         "The replacement peer must require confirmation of its new identity");
  state->previous = std::move(fingerprint);
  ++state->count;
  co_return true;
}

static elder_terms::CurlHttpRequest listing_request(const std::string &base) {
  elder_terms::CurlHttpRequest request;
  request.url = base + '/';
  request.body = "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/></d:prop></d:propfind>";
  request.headers = {"Depth: 0", "Content-Type: application/xml"};
  return request;
}

static elder_terms::CurlHttpRequest move_request(const std::string &base) {
  elder_terms::CurlHttpRequest request;
  request.method = "MOVE";
  request.url = base + "/hello.txt";
  request.headers = {"Destination: " + base + "/renamed.txt", "Overwrite: F"};
  return request;
}

static cardio::promise<void> check_async(
    unsigned port, cardio::dispatcher_group_glib &group,
    std::exception_ptr &failure) {
  std::shared_ptr<elder_terms::CurlHttpSession> session;
  std::shared_ptr<elder_terms::CurlHttpSession> second_session;
  CertificateConfirmations confirmations;
  CertificateConfirmations separate_confirmations;
  try {
    elder_terms::WebdavConnectionSettings settings;
    settings.scheme = "https";
    settings.address = "127.0.0.1";
    settings.port = port;
    settings.base_path = "/dav/";
    settings.authentication = elder_terms::WebdavAuthentication::basic;
    settings.username = "alice";
    settings.prompt_certificate = true;
    session = elder_terms::create_curl_http_session(settings, "secret",
        [&confirmations](const auto &certificate, cardio::cancellation cancellation) {
          return accept_certificate_async(&confirmations, certificate.sha256, cancellation);
        });
    const auto base = "https://127.0.0.1:" + std::to_string(port) + "/dav";
    const auto initial = co_await elder_terms::perform_http_request_async(session, listing_request(base), {});
    expect(initial.code == CURLE_OK && initial.status == 207 && confirmations.count == 1,
           "Initial read-only request must continue after approving its certificate");
    std::cout << "READY" << std::endl;
    co_await cardio::from_fd(STDIN_FILENO, cardio::fd_event::read);
    char command = 0;
    expect(::read(STDIN_FILENO, &command, 1) == 1 && command == 'r',
           "Parent must rotate the certificate before the mutation starts");
    bool failed = false;
    try {
      const auto result = co_await elder_terms::perform_http_request_async(session, move_request(base), {});
      failed = result.code != CURLE_OK || result.status != 201;
    } catch (const std::exception &) { failed = true; }
    expect(failed && confirmations.count == 2,
           "Approving a changed certificate must fail the pending mutation without replay");
    std::cout << "FAILED_MUTATION" << std::endl;
    co_await cardio::from_fd(STDIN_FILENO, cardio::fd_event::read);
    expect(::read(STDIN_FILENO, &command, 1) == 1 && command == 't',
           "Parent must verify no mutation reached the server before explicit retry");
    const auto retry = co_await elder_terms::perform_http_request_async(session, move_request(base), {});
    expect(retry.code == CURLE_OK && retry.status == 201 && confirmations.count == 2,
           "A distinct explicit retry may use the approved replacement certificate");
    second_session = elder_terms::create_curl_http_session(settings, "secret",
        [&separate_confirmations](const auto &certificate, cardio::cancellation cancellation) {
          return accept_certificate_async(&separate_confirmations, certificate.sha256, cancellation);
        });
    const auto separate = co_await elder_terms::perform_http_request_async(second_session, listing_request(base), {});
    expect(separate.code == CURLE_OK && separate.status == 207 &&
               separate_confirmations.count == 1 && confirmations.count == 2,
           "A separate logical session in the same process must not inherit a certificate exception or TLS connection");
  } catch (...) { failure = std::current_exception(); }
  try { if (second_session) co_await elder_terms::stop_curl_http_session_async(second_session); }
  catch (...) { if (!failure) failure = std::current_exception(); }
  try { if (session) co_await elder_terms::stop_curl_http_session_async(session); }
  catch (...) { if (!failure) failure = std::current_exception(); }
  group.shutdown();
}

int main(int argc, char **argv) {
  try {
    expect(argc == 2, "Expected TLS fixture port");
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib_auto dispatcher(group);
    std::exception_ptr failure;
    auto pending = check_async(static_cast<unsigned>(std::stoul(argv[1])), group, failure);
    dispatcher.park();
    if (failure) std::rethrow_exception(failure);
    std::cout << "WebDAV certificate mutation boundary PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

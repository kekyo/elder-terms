#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cardio.h>
#include <curl/curl.h>

#include "ftp-client.h"

namespace elder_terms {

/** One request executed exclusively on the session's curl worker. */
struct CurlFtpRequest {
  /** FTP URL, without credentials. */
  std::string url;
  /** Commands before libcurl changes directories. */
  std::vector<std::string> quote;
  /** Commands after the transfer type has been selected. */
  std::vector<std::string> prequote;
  /** Directory-listing command, or empty for the default. */
  std::string custom_request;
  /** True to send a file using STOR. */
  bool upload = false;
  /** True for a control-only request. */
  bool no_body = false;
  /** Only initial authentication requests may restart after an approved control certificate. */
  bool initial_authentication = false;
  /** Receives bytes on the worker; CURL_WRITEFUNC_PAUSE waits for resume(). */
  std::function<std::size_t(std::span<const std::byte>)> receive;
  /** Supplies bytes on the worker; CURL_READFUNC_PAUSE waits for resume(). */
  std::function<std::size_t(std::span<std::byte>)> send;
  /** Observes complete server response lines on the worker. */
  std::function<void(std::string_view)> header;
};

/** Completed request, including the final FTP response. */
struct CurlFtpResult {
  /** libcurl result; protocol errors remain observable to the caller. */
  CURLcode code = CURLE_OK;
  /** Last FTP response code. */
  long response_code = 0;
  /** Detailed error text, without outgoing credentials. */
  std::string error;
  /** Login directory reported by libcurl. */
  std::string entry_path;
  /** Unapproved certificate failure that ended the handshake. */
  std::optional<TlsCertificateFailure> certificate_failure{};
  /** Current operation failed, but its approved certificate permits a fresh next operation. */
  bool certificate_accepted = false;
};

/** Serial curl executor running outside the caller's dispatcher. */
class CurlFtpSession {
public:
  /** Requests worker shutdown without blocking the calling thread. */
  virtual ~CurlFtpSession() = default;

  /**
   * Executes one request; the caller serializes requests for this session.
   * @param request Request and callbacks retained until completion.
   * @param cancellation Cancellation observed even during FTP response waits.
   * @returns Final result, or a cancellation/callback exception.
   */
  virtual cardio::promise<CurlFtpResult>
  perform_async(CurlFtpRequest request, cardio::cancellation cancellation) = 0;

  /** Notifies the owner thread that a paused byte callback may be retried. */
  virtual void resume() = 0;

  /**
   * Cancels work and asynchronously waits for curl and worker dispatcher cleanup.
   * @remarks The caller's dispatcher must remain alive until this completes.
   */
  virtual cardio::promise<void> stop_async() = 0;
};

/**
 * Starts a worker using cardio::promises::start_new().
 * @param options Endpoint settings and runtime credentials.
 * @returns Executor ready to accept FTP requests.
 */
cardio::promise<std::shared_ptr<CurlFtpSession>>
open_curl_ftp_session_async(FtpClientOpenOptions options);

} // namespace elder_terms

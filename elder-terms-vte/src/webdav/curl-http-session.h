#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <cardio.h>
#include <curl/curl.h>
#include <elder-terms/settings/webdav-settings.h>
#include "../tls/certificate-failure.h"

namespace elder_terms {

/**
 * Asynchronous decision for one immutable TLS failure on the owning dispatcher.
 * @param failure Borrowed failure data, valid until the decision promise settles.
 * @param cancellation Cancellation of the owning request or connection.
 * @returns True to accept exactly this failure for this connection; false to stop.
 * @remarks Runs after the failed handshake has released its request resources.
 * Decisions are retained only by the logical session and are never persisted.
 */
using WebdavCertificateConfirmation = std::function<cardio::promise<bool>(
    const TlsCertificateFailure &, cardio::cancellation)>;

/** HTTP transport owned by one logical WebDAV connection and dispatcher. */
struct CurlHttpSession;

/** One HTTP request; all referenced data is owned until completion. */
struct CurlHttpRequest {
  /** Absolute URL without credentials. */
  std::string url;
  /** Method selected by the WebDAV backend, never arbitrary user input. */
  std::string method = "PROPFIND";
  /** Small, repeatable XML request body, or empty. */
  std::string body;
  /** Request headers, without terminating CRLF. */
  std::vector<std::string> headers;
  /** Known upload length; present only with a forward-only send callback. */
  std::optional<std::uint64_t> upload_size;
  /** Bounded upload producer; may return CURL_READFUNC_PAUSE, never rewind. */
  std::function<std::size_t(std::span<std::byte>)> send;
  /** Optional successful-body receiver; may return CURL_WRITEFUNC_PAUSE. */
  std::function<std::size_t(std::span<const std::byte>)> receive;
};

/** Final HTTP response, including protocol errors rather than hiding them. */
struct CurlHttpResult {
  /** Transport completion code. */
  CURLcode code = CURLE_OK;
  /** Final HTTP response status. */
  long status = 0;
  /** Final response headers, indexed by lowercase name. */
  std::map<std::string, std::string> headers;
  /** Bounded response body when no successful-body receiver is provided. */
  std::string body;
  /** Diagnostic without outgoing credentials. */
  std::string error;
};

/**
 * Describes a failed HTTP result without including outgoing credentials.
 * @param result Completed transport and HTTP status.
 * @returns Exception with the status and supported failure category.
 */
std::runtime_error webdav_http_error(const CurlHttpResult &result);

/**
 * Creates an HTTP transport without starting a worker or network request.
 * @param settings Immutable connection settings.
 * @param password Runtime password, retained only by the session.
 * @param confirm_certificate Optional session-local confirmation; absent means reject.
 * @returns Session bound to the caller's dispatcher.
 */
std::shared_ptr<CurlHttpSession> create_curl_http_session(
    WebdavConnectionSettings settings, std::string password,
    WebdavCertificateConfirmation confirm_certificate = {});

/**
 * Performs one serialized request using socket readiness and timer events.
 * @param session Transport retained until all event watchers finish.
 * @param request Request retained until libcurl releases its pointers.
 * @param cancellation Cancellation for queued or active work.
 * @returns Final HTTP response, or a callback/cancellation exception.
 * @remarks The operation slot remains held during certificate confirmation.
 * Approval may resume read-only requests that failed before any HTTP response;
 * mutations remain failed and require a distinct explicit retry.
 */
cardio::promise<CurlHttpResult> perform_http_request_async(
    std::shared_ptr<CurlHttpSession> session, CurlHttpRequest request,
    cardio::cancellation cancellation);

/**
 * Schedules resumption of a paused request or response callback.
 * @param session Session accessed on its owning dispatcher.
 */
void resume_curl_http_session(const std::shared_ptr<CurlHttpSession> &session);

/**
 * Cancels requests and waits for every request to release its curl resources.
 * @param session Session to stop; repeated calls are permitted.
 */
cardio::promise<void> stop_curl_http_session_async(
    std::shared_ptr<CurlHttpSession> session);

} // namespace elder_terms

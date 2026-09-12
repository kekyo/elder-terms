#pragma once

#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <cardio.h>
#include <curl/curl.h>
#include <elder-terms/settings/webdav-settings.h>

namespace elder_terms {

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
 * Creates an HTTP transport without starting a worker or network request.
 * @param settings Immutable connection settings.
 * @param password Runtime password, retained only by the session.
 * @returns Session bound to the caller's dispatcher.
 */
std::shared_ptr<CurlHttpSession> create_curl_http_session(
    WebdavConnectionSettings settings, std::string password);

/**
 * Performs one serialized request using socket readiness and timer events.
 * @param session Transport retained until all event watchers finish.
 * @param request Request retained until libcurl releases its pointers.
 * @param cancellation Cancellation for queued or active work.
 * @returns Final HTTP response, or a callback/cancellation exception.
 */
cardio::promise<CurlHttpResult> perform_http_request_async(
    std::shared_ptr<CurlHttpSession> session, CurlHttpRequest request,
    cardio::cancellation cancellation);

/**
 * Schedules resumption of a paused response callback.
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

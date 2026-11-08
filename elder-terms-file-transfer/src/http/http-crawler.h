// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include "curl-http-session.h"
#include <elder-terms/http-url.h>

namespace elder_terms {

/** Limits applied per page and per recursive receive operation. */
struct HttpCrawlerLimits {
  /** Maximum HTML response bytes per page. */
  std::size_t html_bytes = 2 * 1024 * 1024;
  /** Maximum redirect hops per request. */
  std::size_t redirects = 5;
  /** Maximum nesting depth per operation. */
  std::size_t depth = 32;
  /** Maximum pages per operation. */
  std::size_t pages = 1000;
  /** Maximum links per page and per operation. */
  std::size_t links = 10000;
};

/** One classified response, with children obtained only from this page. */
struct HttpCrawlerPage {
  /** True for an expandable HTML response. */
  bool html = false;
  /** Classified resource, retaining its final URL. */
  RemoteFileAttributes resource;
  /** Immediate links; no child has been requested. */
  RemoteDirectorySnapshot directory;
  /** Requested and redirected URLs identifying this resource. */
  std::vector<std::string> aliases;
};

/**
 * Checks successful-response media type and attachment disposition.
 * @param response Successful HTTP response headers.
 * @returns True for navigable HTML or XHTML.
 */
bool http_response_is_html(const CurlHttpResult &response);

/**
 * Builds an immediate listing from an already received bounded response.
 * @param scope Canonical allowed directory URL.
 * @param url Final canonical response URL.
 * @param response Successful response headers.
 * @param body Complete HTML body; empty for ordinary content.
 * @param aliases Requested and redirected resource URLs.
 * @param force_html Explicit media-type override.
 * @param limits Page parsing limits.
 * @returns Classified resource and unresolved immediate children.
 */
HttpCrawlerPage parse_http_page(const std::string &scope, const std::string &url,
    const CurlHttpResult &response, const std::string &body,
    const std::vector<std::string> &aliases, bool force_html, HttpCrawlerLimits limits);

/**
 * Fetches and classifies just one page, checking each redirect before connecting.
 * @param session Shared asynchronous HTTP session.
 * @param scope Validated directory scope URL.
 * @param url Requested URL inside the scope.
 * @param force_html Explicitly interpret a successful response as HTML.
 * @param limits Bounded parsing and redirects.
 * @param cancellation Cancellation of the complete operation.
 * @returns HTML links or a content resource whose body was deliberately stopped.
 */
cardio::promise<HttpCrawlerPage> fetch_http_page_async(
    std::shared_ptr<CurlHttpSession> session, std::string scope, std::string url,
    bool force_html, HttpCrawlerLimits limits, cardio::cancellation cancellation);

} // namespace elder_terms

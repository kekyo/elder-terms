// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include "../webdav/curl-http-session.h"

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
 * Resolves and normalizes an HTTP reference, preserving encoded URL bytes.
 * @param page Absolute resolution base, or empty for an absolute reference.
 * @param reference Absolute or relative URL without embedded credentials.
 * @returns Canonical URL without a fragment.
 * @throws std::invalid_argument For unsupported or ambiguous URLs.
 */
std::string http_resolve_url(const std::string &page, const std::string &reference);

/**
 * Validates a directory URL defining the permitted origin and path boundary.
 * @param url Absolute URL ending in a slash, without query or credentials.
 * @returns Canonical scope URL.
 */
std::string http_scope_url(const std::string &url);

/**
 * Checks a canonical URL against the configured origin and directory boundary.
 * @param scope Canonical directory URL.
 * @param url Canonical resource URL.
 * @returns True only inside the configured scope.
 */
bool http_url_in_scope(const std::string &scope, const std::string &url);

/**
 * Produces a safe filename candidate without changing the resource URL.
 * @param url Canonical resource URL.
 * @returns UTF-8 name or an encoded name when decoding is not possible.
 */
std::string http_resource_name(const std::string &url);

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

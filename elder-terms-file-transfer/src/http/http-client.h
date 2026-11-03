// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/
#pragma once
#include "http-crawler.h"

namespace elder_terms {
/** Settings and runtime credentials for a read-only HTTP connection. */
struct HttpClientOpenOptions {
  /** HTTP transport, authentication and TLS settings. */
  CurlHttpSettings transport;
  /** Directory URL restricting every request. */
  std::string base_url;
  /** Initial page URL; empty uses base_url. */
  std::string start_url;
  /** HTML and traversal limits. */
  HttpCrawlerLimits limits;
  /** Runtime password, never persisted. */
  std::string password;
  /** Optional connection-local certificate confirmation. */
  HttpCertificateConfirmation confirm_certificate;
  /** Owned SSH route, or null for direct access. */
  std::shared_ptr<SshSocksProxy> proxy;
};
/**
 * Opens a read-only connection, fetching only the initial page.
 * @param options Connection settings and credentials.
 * @param cancellation Cancellation for connection setup.
 * @returns Lazy HTTP file client on the owning dispatcher.
 */
cardio::promise<std::shared_ptr<RemoteFileClient>> open_http_client_async(
    HttpClientOpenOptions options, cardio::cancellation cancellation);
/**
 * Stops outstanding HTTP requests before dispatcher shutdown.
 * @param client Client created by open_http_client_async().
 */
cardio::promise<void> stop_http_client_async(std::shared_ptr<RemoteFileClient> client);
}

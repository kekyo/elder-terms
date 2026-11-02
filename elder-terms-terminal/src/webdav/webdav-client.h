// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include "../file-transfer/remote-file-client.h"
#include "../http/curl-http-session.h"
#include <elder-terms/settings/webdav-settings.h>

namespace elder_terms {

/**
 * Extracts protocol-independent transport settings from a WebDAV endpoint.
 * @param settings Effective WebDAV settings.
 * @returns HTTP transport settings without WebDAV path or XML policy.
 */
inline CurlHttpSettings webdav_http_settings(const WebdavConnectionSettings &settings) {
  const auto auth = settings.authentication;
  return {.scheme = settings.scheme, .address = settings.address, .port = settings.port,
      .authentication = auth == WebdavAuthentication::none ? HttpAuthentication::none
          : auth == WebdavAuthentication::basic ? HttpAuthentication::basic
          : auth == WebdavAuthentication::digest ? HttpAuthentication::digest : HttpAuthentication::automatic,
      .username = settings.username, .ca_file = settings.ca_file,
      .prompt_certificate = settings.prompt_certificate,
      .connect_timeout_seconds = settings.connect_timeout_seconds,
      .idle_timeout_seconds = settings.idle_timeout_seconds,
      .validation_errors = settings.validation_errors};
}

/** Parameters owned by one authenticated WebDAV connection. */
struct WebdavClientOpenOptions {
  /** Persisted endpoint and runtime user name. */
  WebdavConnectionSettings connection;
  /** Runtime password, never written to settings. */
  std::string password;
  /** Optional asynchronous decision for this connection only. */
  HttpCertificateConfirmation confirm_certificate;
  /** Owned SSH route, or null for direct HTTP connections. */
  std::shared_ptr<SshSocksProxy> proxy{};
};

/**
 * Opens a WebDAV session and verifies its initial collection using PROPFIND.
 * @param options Endpoint and runtime credentials.
 * @param cancellation Cancellation for the initial connection.
 * @returns Common file client using the caller's cardio dispatcher.
 */
cardio::promise<std::shared_ptr<RemoteFileClient>> open_webdav_client_async(
    WebdavClientOpenOptions options, cardio::cancellation cancellation);

/**
 * Cancels all requests and releases event watches before dispatcher shutdown.
 * @param client Client returned by open_webdav_client_async().
 */
cardio::promise<void> stop_webdav_client_async(std::shared_ptr<RemoteFileClient> client);

} // namespace elder_terms

#pragma once

#include "../file-transfer/remote-file-client.h"
#include "curl-http-session.h"
#include <elder-terms/settings/webdav-settings.h>

namespace elder_terms {

/** Parameters owned by one authenticated WebDAV connection. */
struct WebdavClientOpenOptions {
  /** Persisted endpoint and runtime user name. */
  WebdavConnectionSettings connection;
  /** Runtime password, never written to settings. */
  std::string password;
  /** Optional asynchronous decision for this connection only. */
  WebdavCertificateConfirmation confirm_certificate;
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

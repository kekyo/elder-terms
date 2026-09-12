#pragma once

#include "curl-http-session.h"
#include "webdav-url.h"
#include "../file-transfer/remote-file-client.h"

namespace elder_terms {

/**
 * Starts a bounded, cancellable GET stream within one WebDAV virtual root.
 * @param session HTTP transport retained through response completion.
 * @param endpoint Validated origin and virtual root.
 * @param path Decoded absolute virtual file path.
 * @param cancellation Cancellation retained for the complete response lifetime.
 * @returns Reader whose early close or destruction cancels unfinished HTTP work.
 */
cardio::promise<std::unique_ptr<RemoteFileReader>> open_webdav_reader_async(
    std::shared_ptr<CurlHttpSession> session, WebdavEndpoint endpoint,
    std::string path, cardio::cancellation cancellation);

} // namespace elder_terms

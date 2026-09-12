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

/**
 * Starts a bounded, exclusive PUT of one known-size regular file.
 * @param session Transport retained until the final response retires.
 * @param endpoint Validated origin and virtual root.
 * @param path Temporary virtual file path, never overwritten on a collision.
 * @param expected_size Exact body size from discovery, including zero.
 * @param cancellation Cancellation retained for the request lifetime.
 * @returns Writer reporting proven creation after its final response.
 */
cardio::promise<std::unique_ptr<RemoteFileWriter>> open_webdav_writer_async(
    std::shared_ptr<CurlHttpSession> session, WebdavEndpoint endpoint,
    std::string path, std::uint64_t expected_size, cardio::cancellation cancellation);

} // namespace elder_terms

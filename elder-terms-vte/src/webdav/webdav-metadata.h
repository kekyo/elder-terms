#pragma once

#include "webdav-url.h"
#include "../file-transfer/remote-file-client.h"

namespace elder_terms {

/** One resource reported by a DAV multistatus response. */
struct WebdavResource {
  /** Decoded virtual path and successful properties. */
  RemoteFileAttributes attributes;
  /** Resource-level status, or 200 when successful propstat values exist. */
  int status = 0;
};

/**
 * Parses bounded DAV XML without loading DTDs, external entities or XInclude.
 * @param xml Complete 207 response body, limited to 32 MiB.
 * @param endpoint Connection origin and virtual root.
 * @param request_url Final URL that produced the response.
 * @returns Resource statuses and properties; malformed/ambiguous data throws.
 */
std::vector<WebdavResource> parse_webdav_multistatus(
    const std::string &xml, const WebdavEndpoint &endpoint,
    const std::string &request_url);

} // namespace elder_terms

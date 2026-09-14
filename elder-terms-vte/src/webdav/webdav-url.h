#pragma once

#include <string>
#include <elder-terms/settings/webdav-settings.h>

namespace elder_terms {

/** Canonical endpoint and decoded server path forming the virtual root. */
struct WebdavEndpoint {
  /** Canonical scheme, host and explicit port, without a trailing slash. */
  std::string origin;
  /** Decoded absolute server path without a trailing slash, or empty for /. */
  std::string base_path;
};

/**
 * Validates an endpoint and separates its origin from its virtual root.
 * @param settings Configured endpoint.
 * @returns Canonical endpoint; invalid paths or authorities throw.
 */
WebdavEndpoint webdav_endpoint(const WebdavConnectionSettings &settings);

/**
 * Validates a decoded virtual path without resolving dot components.
 * @param path Absolute virtual path.
 * @returns Path without a trailing slash, except for the root.
 */
std::string webdav_virtual_path(std::string path);

/**
 * Encodes a virtual path exactly once under the endpoint root.
 * @param endpoint Validated endpoint.
 * @param path Decoded absolute virtual path.
 * @returns Absolute HTTP(S) URL.
 */
std::string webdav_resource_url(const WebdavEndpoint &endpoint, const std::string &path);

/**
 * Resolves a server href within the same origin and virtual root.
 * @param endpoint Validated endpoint.
 * @param request_url Absolute URL of the response containing the reference.
 * @param reference Encoded absolute or relative URI reference.
 * @returns Decoded virtual path; ambiguous or escaping references throw.
 */
std::string webdav_reference_path(const WebdavEndpoint &endpoint,
                                  const std::string &request_url,
                                  const std::string &reference);

} // namespace elder_terms

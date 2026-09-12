#include "webdav-url.h"

#include <glib.h>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace elder_terms {

using OwnedUri = std::unique_ptr<GUri, decltype(&g_uri_unref)>;

static OwnedUri parse_uri(const std::string &text) {
  if (text.find('\0') != std::string::npos || text.find_first_of("\r\n\t \\") != std::string::npos)
    throw std::invalid_argument("Invalid WebDAV URL characters");
  GError *error = nullptr;
  OwnedUri uri(g_uri_parse(text.c_str(), G_URI_FLAGS_ENCODED, &error), g_uri_unref);
  if (error) g_error_free(error);
  if (!uri || !g_uri_get_host(uri.get()) || g_uri_get_userinfo(uri.get()) ||
      g_uri_get_query(uri.get()) || g_uri_get_fragment(uri.get()))
    throw std::invalid_argument("Invalid WebDAV URL authority, query or fragment");
  return uri;
}

static std::string uri_origin(GUri *uri) {
  std::string scheme = g_uri_get_scheme(uri);
  if (scheme != "https" && scheme != "http") throw std::invalid_argument("Invalid WebDAV URL scheme");
  const auto lower = std::unique_ptr<gchar, decltype(&g_free)>(g_ascii_strdown(g_uri_get_host(uri), -1), g_free);
  std::string host(lower.get());
  if (host.empty() || host.find('%') != std::string::npos) throw std::invalid_argument("Invalid WebDAV host");
  if (host.find(':') != std::string::npos) host = "[" + host + "]";
  auto port = g_uri_get_port(uri);
  if (port < 0) port = scheme == "https" ? 443 : 80;
  if (port < 1 || port > 65535) throw std::invalid_argument("Invalid WebDAV port");
  return scheme + "://" + host + ":" + std::to_string(port);
}

std::string webdav_virtual_path(std::string path) {
  if (path.empty() || path.front() != '/' || path.find('\\') != std::string::npos ||
      !g_utf8_validate(path.data(), static_cast<gssize>(path.size()), nullptr))
    throw std::invalid_argument("Invalid WebDAV path");
  for (const unsigned char ch : path)
    if (ch < 32 || ch == 127) throw std::invalid_argument("Invalid WebDAV path character");
  for (std::size_t offset = 1; offset < path.size();) {
    auto end = path.find('/', offset);
    if (end == std::string::npos) end = path.size();
    const auto component = std::string_view(path).substr(offset, end - offset);
    if (component.empty() || component == "." || component == "..")
      throw std::invalid_argument("Ambiguous WebDAV path component");
    offset = end + 1;
  }
  if (path.size() > 1 && path.back() == '/') path.pop_back();
  return path;
}

static std::string decode_path(const std::string &path) {
  if (path.find('\0') != std::string::npos)
    throw std::invalid_argument("Invalid WebDAV path character");
  const auto decoded = std::unique_ptr<gchar, decltype(&g_free)>(
      g_uri_unescape_string(path.c_str(), "/\\"), g_free);
  if (!decoded) throw std::invalid_argument("Invalid WebDAV path escaping");
  return webdav_virtual_path(decoded.get());
}

WebdavEndpoint webdav_endpoint(const WebdavConnectionSettings &settings) {
  if (!settings.validation_errors.empty()) throw std::invalid_argument(settings.validation_errors.front());
  auto host = settings.address;
  if (host.empty() || host.find_first_of("/@?# \\") != std::string::npos)
    throw std::invalid_argument("WebDAV server address is required");
  if (host.find(':') != std::string::npos && !host.starts_with('[')) host = "[" + host + "]";
  const auto uri = parse_uri(settings.scheme + "://" + host + ":" + std::to_string(settings.port) + "/");
  auto base = decode_path(settings.base_path);
  if (base == "/") base.clear();
  return {uri_origin(uri.get()), std::move(base)};
}

std::string webdav_resource_url(const WebdavEndpoint &endpoint, const std::string &path) {
  auto full = endpoint.base_path + webdav_virtual_path(path);
  const auto encoded = std::unique_ptr<gchar, decltype(&g_free)>(
      g_uri_escape_string(full.c_str(), "/", false), g_free);
  if (!encoded) throw std::bad_alloc();
  return endpoint.origin + encoded.get();
}

std::string webdav_reference_path(const WebdavEndpoint &endpoint,
                                  const std::string &request_url,
                                  const std::string &reference) {
  if (reference.empty() || reference.size() > 64 * 1024 || reference.starts_with("//"))
    throw std::invalid_argument("Invalid WebDAV resource reference");
  std::string absolute;
  if (reference.find("://") != std::string::npos) absolute = reference;
  else if (reference.front() == '/') absolute = endpoint.origin + reference;
  else absolute = request_url.substr(0, request_url.find_last_of('/') + 1) + reference;
  // Inspect the encoded path before URI processing could normalize dot segments.
  const auto authority_end = absolute.find('/', absolute.find("://") + 3);
  if (authority_end == std::string::npos) throw std::invalid_argument("WebDAV reference has no path");
  const auto raw_path = absolute.substr(authority_end);
  if (raw_path.find_first_of("?#") != std::string::npos)
    throw std::invalid_argument("WebDAV references cannot contain queries or fragments");
  auto decoded = decode_path(raw_path);
  const auto uri = parse_uri(absolute);
  if (uri_origin(uri.get()) != endpoint.origin)
    throw std::invalid_argument("WebDAV reference changes the connection origin");
  if (decoded == endpoint.base_path) return "/";
  if (!decoded.starts_with(endpoint.base_path + "/"))
    throw std::invalid_argument("WebDAV reference escapes the configured base path");
  return webdav_virtual_path(decoded.substr(endpoint.base_path.size()));
}

} // namespace elder_terms

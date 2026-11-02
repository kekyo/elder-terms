// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "http-crawler.h"
#include <algorithm>
#include <charconv>
#include <set>
#include <string_view>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>

namespace elder_terms {

using HttpUrl = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
using HttpDocument = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;

static std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return g_ascii_tolower(c); });
  return text;
}

static std::string trim(std::string text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

static void set_url(CURLU *url, CURLUPart part, const std::string &value) {
  const auto result = curl_url_set(url, part, value.c_str(), CURLU_DISALLOW_USER);
  if (result != CURLUE_OK) throw std::invalid_argument(curl_url_strerror(result));
}

static std::string url_part(const CURLU *url, CURLUPart part, unsigned flags) {
  char *value = nullptr;
  const auto result = curl_url_get(url, part, &value, flags);
  if (result == CURLUE_NO_QUERY || result == CURLUE_NO_FRAGMENT) return {};
  if (result != CURLUE_OK) throw std::invalid_argument(curl_url_strerror(result));
  const std::unique_ptr<char, decltype(&curl_free)> owned(value, curl_free);
  return value;
}

static HttpUrl parse_url(const std::string &url) {
  HttpUrl parsed(curl_url(), curl_url_cleanup);
  if (!parsed) throw std::bad_alloc();
  set_url(parsed.get(), CURLUPART_URL, url);
  return parsed;
}

static std::string encoded_reference(const std::string &reference) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : reference) {
    if (c < 0x20 || c == 0x7f || c == '\\') throw std::invalid_argument("Ambiguous HTTP URL character");
    if (c >= 0x80 || c == ' ') {
      result += '%'; result += hex[c >> 4]; result += hex[c & 15];
    } else result += static_cast<char>(c);
  }
  return result;
}

std::string http_resolve_url(const std::string &page, const std::string &reference) {
  auto url = page.empty() ? HttpUrl(curl_url(), curl_url_cleanup) : parse_url(page);
  if (!url) throw std::bad_alloc();
  if (!reference.empty()) set_url(url.get(), CURLUPART_URL, encoded_reference(reference));
  auto scheme = lower(url_part(url.get(), CURLUPART_SCHEME, 0));
  if (scheme != "http" && scheme != "https") throw std::invalid_argument("HTTP or HTTPS URL required");
  set_url(url.get(), CURLUPART_SCHEME, scheme);
  set_url(url.get(), CURLUPART_HOST, lower(url_part(url.get(), CURLUPART_HOST, 0)));
  auto path = url_part(url.get(), CURLUPART_PATH, 0);
  // Decode only unreserved bytes. Reject encoded separators and controls;
  // %25 remains encoded and must never initiate another decoding pass.
  std::string normalized;
  static constexpr char hex[] = "0123456789ABCDEF";
  for (std::size_t index = 0; index < path.size(); ++index) {
    const auto c = static_cast<unsigned char>(path[index]);
    if (c != '%') { normalized += static_cast<char>(c); continue; }
    if (index + 2 >= path.size() || !g_ascii_isxdigit(path[index + 1]) || !g_ascii_isxdigit(path[index + 2]))
      throw std::invalid_argument("Malformed HTTP URL escape");
    const auto value = (g_ascii_xdigit_value(path[index + 1]) << 4) | g_ascii_xdigit_value(path[index + 2]);
    index += 2;
    if (value < 0x20 || value == 0x7f || value == '/' || value == '\\')
      throw std::invalid_argument("Ambiguous encoded HTTP path separator");
    if (g_ascii_isalnum(value) || value == '-' || value == '.' || value == '_' || value == '~')
      normalized += static_cast<char>(value);
    else { normalized += '%'; normalized += hex[value >> 4]; normalized += hex[value & 15]; }
  }
  set_url(url.get(), CURLUPART_PATH, normalized);
  (void)curl_url_set(url.get(), CURLUPART_FRAGMENT, nullptr, 0);
  // Reparse the serialized URL to remove dot segments decoded above.
  const auto serialized = url_part(url.get(), CURLUPART_URL, CURLU_NO_DEFAULT_PORT);
  set_url(url.get(), CURLUPART_URL, serialized);
  auto canonical = url_part(url.get(), CURLUPART_URL, CURLU_NO_DEFAULT_PORT);
  for (std::size_t index = 0; index + 2 < canonical.size() && canonical[index] != '?'; ++index)
    if (canonical[index] == '%') {
      canonical[index + 1] = g_ascii_toupper(canonical[index + 1]);
      canonical[index + 2] = g_ascii_toupper(canonical[index + 2]);
      index += 2;
    }
  return canonical;
}

std::string http_scope_url(const std::string &url) {
  if (url.find_first_of("?#") != std::string::npos)
    throw std::invalid_argument("HTTP base URL must not contain a query or fragment");
  const auto scope = http_resolve_url({}, url);
  if (!scope.ends_with('/')) throw std::invalid_argument("HTTP base URL must end in a slash");
  return scope;
}

bool http_url_in_scope(const std::string &scope, const std::string &url) {
  // Both arguments use the canonical serialization sent to libcurl.
  return url.starts_with(scope);
}

std::string http_resource_name(const std::string &url) {
  const auto parsed = parse_url(url);
  auto path = url_part(parsed.get(), CURLUPART_PATH, 0);
  while (path.size() > 1 && path.ends_with('/')) path.pop_back();
  auto name = path.substr(path.find_last_of('/') + 1);
  if (name.empty()) name = "index";
  std::string decoded;
  for (std::size_t index = 0; index < name.size(); ++index) {
    if (name[index] == '%' && index + 2 < name.size()) {
      decoded += static_cast<char>((g_ascii_xdigit_value(name[index + 1]) << 4) | g_ascii_xdigit_value(name[index + 2]));
      index += 2;
    } else decoded += name[index];
  }
  if (g_utf8_validate(decoded.data(), decoded.size(), nullptr) && decoded != "." && decoded != ".." &&
      decoded.find_first_of("/\\") == std::string::npos) name = std::move(decoded);
  return name;
}

static std::string header_value(const CurlHttpResult &result, const std::string &name) {
  const auto found = result.headers.find(name);
  return found == result.headers.end() ? "" : found->second;
}

static std::string media_token(const std::string &value) {
  return lower(trim(value.substr(0, value.find(';'))));
}

static std::string charset_parameter(const std::string &value) {
  auto remaining = std::string_view(value);
  for (;;) {
    const auto separator = remaining.find(';');
    if (separator == std::string_view::npos) return {};
    remaining.remove_prefix(separator + 1);
    const auto part = std::string(remaining.substr(0, remaining.find(';')));
    const auto equal = part.find('=');
    if (equal != std::string::npos && lower(trim(part.substr(0, equal))) == "charset") {
      auto encoding = trim(part.substr(equal + 1));
      if (encoding.size() >= 2 && (encoding.front() == '\'' || encoding.front() == '"') && encoding.back() == encoding.front())
        encoding = encoding.substr(1, encoding.size() - 2);
      return encoding;
    }
  }
}

static std::string property(xmlNode *node, const char *name) {
  const std::unique_ptr<xmlChar, decltype(xmlFree)> value(xmlGetProp(node, BAD_CAST name), xmlFree);
  return value ? reinterpret_cast<const char *>(value.get()) : "";
}

static std::vector<xmlNode *> elements(xmlDoc *document) {
  std::vector<xmlNode *> result;
  std::vector<xmlNode *> pending;
  if (auto *root = xmlDocGetRootElement(document)) pending.push_back(root);
  while (!pending.empty()) {
    auto *node = pending.back(); pending.pop_back();
    if (node->next) pending.push_back(node->next);
    if (node->children) pending.push_back(node->children);
    if (node->type == XML_ELEMENT_NODE) result.push_back(node);
  }
  return result;
}

static bool named(xmlNode *node, const char *name, bool xhtml) {
  return xmlStrEqual(node->name, BAD_CAST name) && (!xhtml ||
      (node->ns && xmlStrEqual(node->ns->href, BAD_CAST "http://www.w3.org/1999/xhtml")));
}

static HttpDocument parse_html(const std::string &body, const std::string &url,
                               const std::string &content_type, bool xhtml) {
  auto encoding = charset_parameter(content_type);
  // HTML priority: BOM, transport charset, then a prescanned meta declaration.
  if (body.starts_with("\xef\xbb\xbf")) encoding = "UTF-8";
  else if (body.starts_with("\xff\xfe")) encoding = "UTF-16LE";
  else if (body.starts_with("\xfe\xff")) encoding = "UTF-16BE";
  if (!xhtml && encoding.empty()) {
    HttpDocument prescan(htmlReadMemory(body.data(), static_cast<int>(std::min<std::size_t>(1024, body.size())),
        url.c_str(), "ISO-8859-1", HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING), xmlFreeDoc);
    if (prescan) for (auto *node : elements(prescan.get())) {
      if (!named(node, "meta", false)) continue;
      encoding = property(node, "charset");
      if (encoding.empty() && lower(property(node, "http-equiv")) == "content-type")
        encoding = charset_parameter(property(node, "content"));
      if (!encoding.empty()) break;
    }
    if (encoding.empty()) encoding = "UTF-8";
  }
  HttpDocument document(xhtml
      ? xmlReadMemory(body.data(), static_cast<int>(body.size()), url.c_str(), encoding.empty() ? nullptr : encoding.c_str(),
          XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING)
      : htmlReadMemory(body.data(), static_cast<int>(body.size()), url.c_str(), encoding.c_str(),
          HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING), xmlFreeDoc);
  if (!document) throw std::runtime_error("HTTP HTML parsing failed");
  // No DTD loading, entity substitution or XInclude. Reject declared entities
  // before reading attributes so external/expanded entities cannot become URLs.
  if (document->extSubset || (document->intSubset && document->intSubset->entities))
    throw std::runtime_error("HTTP HTML entity declarations are not supported");
  return document;
}

cardio::promise<HttpCrawlerPage> fetch_http_page_async(
    std::shared_ptr<CurlHttpSession> session, std::string scope, std::string url,
    bool force_html, HttpCrawlerLimits limits, cardio::cancellation cancellation) {
  HttpCrawlerPage page;
  url = http_resolve_url(scope, url);
  CurlHttpResult response;
  std::string body;
  for (std::size_t redirects = 0;; ++redirects) {
    if (!http_url_in_scope(scope, url)) throw std::invalid_argument("HTTP URL is outside the configured base URL");
    if (std::find(page.aliases.begin(), page.aliases.end(), url) != page.aliases.end())
      throw std::runtime_error("HTTP redirect cycle");
    page.aliases.push_back(url);
    CurlHttpRequest request;
    request.method = "GET";
    request.url = url;
    request.accept_response = [&page, force_html](const CurlHttpResult &headers) {
      if (headers.status < 200 || headers.status == 401) return true;
      if (headers.status != 200) return false;
      page.html = force_html || http_response_is_html(headers);
      return page.html;
    };
    request.receive = [&body, limits](std::span<const std::byte> bytes) {
      if (bytes.size() > limits.html_bytes - body.size()) throw std::runtime_error("HTTP HTML body limit exceeded (2 MiB)");
      body.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
      return bytes.size();
    };
    response = co_await perform_http_request_async(session, std::move(request), cancellation);
    if (response.code != CURLE_OK && !response.body_stopped) throw http_error(response);
    if (response.status == 200) break;
    if (response.status != 301 && response.status != 302 && response.status != 303 && response.status != 307 && response.status != 308)
      throw http_error(response);
    if (redirects == limits.redirects) throw std::runtime_error("HTTP redirect limit exceeded");
    const auto location = header_value(response, "location");
    if (location.empty()) throw std::runtime_error("HTTP redirect is missing Location");
    url = http_resolve_url(url, location);
  }
  co_return parse_http_page(scope, url, response, body, page.aliases, force_html, limits);
}

bool http_response_is_html(const CurlHttpResult &response) {
  const auto type = media_token(header_value(response, "content-type"));
  return (type == "text/html" || type == "application/xhtml+xml") &&
      media_token(header_value(response, "content-disposition")) != "attachment";
}

HttpCrawlerPage parse_http_page(const std::string &scope, const std::string &url,
    const CurlHttpResult &response, const std::string &body,
    const std::vector<std::string> &aliases, bool force_html, HttpCrawlerLimits limits) {
  HttpCrawlerPage page;
  page.html = force_html || http_response_is_html(response);
  page.aliases = aliases;
  page.resource.path = url;
  page.resource.name = http_resource_name(url);
  page.resource.type = page.html ? RemoteFileType::directory : RemoteFileType::regular;
  const auto length = header_value(response, "content-length");
  std::uint64_t size = 0;
  const auto converted = std::from_chars(length.data(), length.data() + length.size(), size);
  if (!length.empty() && converted.ec == std::errc{} && converted.ptr == length.data() + length.size()) page.resource.size = size;
  page.directory.canonical_path = url;
  if (!page.html) return page;
  const bool xhtml = media_token(header_value(response, "content-type")) == "application/xhtml+xml";
  const auto document = parse_html(body, url, header_value(response, "content-type"), xhtml);
  auto base = url;
  const auto nodes = elements(document.get());
  for (auto *node : nodes) if (named(node, "base", xhtml) && xmlHasProp(node, BAD_CAST "href")) {
    try { base = http_resolve_url(url, property(node, "href")); }
    catch (const std::invalid_argument &) { base.clear(); }
    break;
  }
  std::set<std::string> seen(page.aliases.begin(), page.aliases.end());
  std::size_t links = 0;
  for (auto *node : nodes) {
    if (!named(node, "a", xhtml) || !xmlHasProp(node, BAD_CAST "href")) continue;
    if (++links > limits.links) throw std::runtime_error("HTTP link limit exceeded");
    if (base.empty()) continue;
    std::string child;
    try { child = http_resolve_url(base, trim(property(node, "href"))); }
    catch (const std::invalid_argument &) { continue; }
    if (!http_url_in_scope(scope, child) || !seen.insert(child).second ||
        (child.ends_with('/') && url.starts_with(child))) continue;
    RemoteFileAttributes item;
    item.name = http_resource_name(child);
    item.path = std::move(child);
    item.type = RemoteFileType::unresolved;
    page.directory.entries.push_back(std::move(item));
  }
  return page;
}

} // namespace elder_terms

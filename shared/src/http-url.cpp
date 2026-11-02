// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/http-url.h>
#include <curl/curl.h>
#include <glib.h>
#include <algorithm>
#include <memory>
#include <stdexcept>
namespace elder_terms {
using HttpUrl = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
static std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return g_ascii_tolower(c); });
  return text;
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

}

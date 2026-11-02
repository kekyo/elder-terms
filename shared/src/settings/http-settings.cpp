// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/
#include <elder-terms/settings/http-settings.h>
#include <elder-terms/settings/general-settings.h>
#include <elder-terms/http-url.h>
#include <curl/curl.h>
#include <memory>
#include <stdexcept>
#include <algorithm>

namespace elder_terms {
static bool validate_text(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (g_utf8_validate(text.data(), text.size(), nullptr) &&
      std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; })) return true;
  *reason = "must contain valid UTF-8 without control characters"; return false;
}
static bool validate_base(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (text.empty()) return true;
  try { (void)http_scope_url(text); return true; }
  catch (const std::exception &error) { *reason = error.what(); return false; }
}
static bool validate_timeout(const SettingValue &value, std::string *reason) {
  const auto seconds = std::get<gint64>(value);
  if (seconds >= 1 && seconds <= 86400) return true;
  *reason = "must be between 1 and 86400 seconds"; return false;
}
static bool validate_authentication(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (text == "none" || text == "auto" || text == "basic" || text == "digest") return true;
  *reason = "must be none, auto, basic, or digest"; return false;
}
static bool validate_certificate(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (text == "reject" || text == "prompt") return true;
  *reason = "must be reject or prompt"; return false;
}
static bool validate_ca(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (validate_text(value, reason) && (text.empty() || text.starts_with('/'))) return true;
  *reason = "must be an absolute PEM CA bundle path or empty"; return false;
}
SettingKey http_setting_key(std::string name) { return make_setting_key("http", std::move(name)); }
std::vector<SettingDefinition> http_connection_setting_definitions() {
  return {
    {.key = http_setting_key("base_url"), .default_value = std::string(), .validate = validate_base, .retain_invalid = true},
    {.key = http_setting_key("start_url"), .default_value = std::string(), .validate = validate_text, .retain_invalid = true},
    {.key = http_setting_key("authentication"), .default_value = std::string("none"), .validate = validate_authentication, .retain_invalid = true},
    {.key = http_setting_key("username"), .default_value = std::string(), .validate = validate_text, .retain_invalid = true},
    {.key = http_setting_key("local_directory"), .default_value = std::string(), .validate = validate_text, .retain_invalid = true},
    {.key = http_setting_key("ca_file"), .default_value = std::string(), .validate = validate_ca, .retain_invalid = true},
    {.key = http_setting_key("certificate_error_action"), .default_value = std::string("reject"), .validate = validate_certificate, .retain_invalid = true},
    {.key = http_setting_key("connect_timeout_seconds"), .default_value = gint64{30}, .validate = validate_timeout, .retain_invalid = true},
    {.key = http_setting_key("idle_timeout_seconds"), .default_value = gint64{60}, .validate = validate_timeout, .retain_invalid = true},
  };
}
HttpConnectionSettings http_connection_settings(const SettingsStore &store) {
  const auto text = [&store](const char *name, const char *fallback) { return setting_string_value_or_default(store, http_setting_key(name), fallback); };
  HttpConnectionSettings result;
  result.base_url = text("base_url", ""); result.start_url = text("start_url", "");
  if (result.base_url.empty() && general_connection_kind(store) == ConnectionKind::http)
    result.validation_errors.push_back("HTTP base URL is required");
  result.username = text("username", ""); result.local_directory = text("local_directory", "");
  result.ca_file = text("ca_file", ""); result.prompt_certificate = text("certificate_error_action", "reject") == "prompt";
  const auto auth = text("authentication", "none");
  result.authentication = auth == "none" ? HttpAuthentication::none : auth == "basic" ? HttpAuthentication::basic
      : auth == "digest" ? HttpAuthentication::digest : HttpAuthentication::automatic;
  result.connect_timeout_seconds = setting_integer_value_or_default(store, http_setting_key("connect_timeout_seconds"), 30);
  result.idle_timeout_seconds = setting_integer_value_or_default(store, http_setting_key("idle_timeout_seconds"), 60);
  for (const auto &entry : store.entries) if (entry.definition.key.section == "http" && !entry.validation_error.empty())
    result.validation_errors.push_back("[http] " + entry.definition.key.name + ": " + entry.validation_error);
  if (!result.base_url.empty()) try {
    result.base_url = http_scope_url(result.base_url);
    result.start_url = http_resolve_url(result.base_url, result.start_url);
    if (!http_url_in_scope(result.base_url, result.start_url)) throw std::invalid_argument("HTTP start page is outside the base URL");
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, result.base_url.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK)
      throw std::invalid_argument("Invalid HTTP base URL");
    const auto part = [&url](CURLUPart field) {
      char *value = nullptr;
      if (curl_url_get(url.get(), field, &value, CURLU_DEFAULT_PORT) != CURLUE_OK) throw std::invalid_argument("Invalid HTTP endpoint");
      const std::unique_ptr<char, decltype(&curl_free)> owned(value, curl_free); return std::string(value);
    };
    result.scheme = part(CURLUPART_SCHEME); result.address = part(CURLUPART_HOST); result.port = std::stoll(part(CURLUPART_PORT));
    if (result.address.starts_with('[') && result.address.ends_with(']')) result.address = result.address.substr(1, result.address.size() - 2);
  } catch (const std::exception &error) { result.validation_errors.push_back(error.what()); }
  return result;
}
}

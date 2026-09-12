#include <elder-terms/settings/webdav-settings.h>

#include <algorithm>
#include <utility>

namespace elder_terms {

static bool clean_text(const std::string &text) {
  return g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr) &&
      std::none_of(text.begin(), text.end(), [](unsigned char value) {
        return value < 32 || value == 127;
      });
}

static bool validate_text(const SettingValue &value, std::string *reason) {
  if (clean_text(std::get<std::string>(value))) return true;
  *reason = "must contain valid UTF-8 without control characters";
  return false;
}

static bool validate_scheme(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (text == "http" || text == "https") return true;
  *reason = "must be http or https";
  return false;
}

static bool validate_address(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (clean_text(text) && text.find_first_of(" /\\@?#") == std::string::npos)
    return true;
  *reason = "must be a hostname or IP address without a scheme, path, or credentials";
  return false;
}

static bool validate_port(const SettingValue &value, std::string *reason) {
  const auto port = std::get<gint64>(value);
  if (port >= 1 && port <= 65535) return true;
  *reason = "must be an integer between 1 and 65535";
  return false;
}

static bool validate_timeout(const SettingValue &value, std::string *reason) {
  const auto seconds = std::get<gint64>(value);
  if (seconds >= 1 && seconds <= 86400) return true;
  *reason = "must be an integer between 1 and 86400";
  return false;
}

static bool validate_authentication(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (text == "auto" || text == "basic" || text == "digest" || text == "none")
    return true;
  *reason = "must be auto, basic, digest, or none";
  return false;
}

static bool valid_absolute_path(const std::string &path) {
  if (path.empty() || path.front() != '/' || !clean_text(path) ||
      path.find('\\') != std::string::npos) return false;
  std::size_t start = 1;
  while (start < path.size()) {
    const auto end = path.find('/', start);
    const auto part = path.substr(start, end == std::string::npos ? end : end - start);
    if (part == "." || part == ".." || part.empty()) return false;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return true;
}

static bool validate_remote_directory(const SettingValue &value, std::string *reason) {
  if (valid_absolute_path(std::get<std::string>(value))) return true;
  *reason = "must be an absolute path without dot segments or control characters";
  return false;
}

static bool validate_base_path(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (text.find_first_of(" ?#") == std::string::npos && valid_absolute_path(text)) {
    // URI escapes are decoded once. Encoded separators must not create a new
    // hierarchy when the browser maps a server resource to a local filename.
    auto *decoded = g_uri_unescape_string(text.c_str(), "/\\");
    const bool valid = decoded && valid_absolute_path(decoded);
    g_free(decoded);
    if (valid) return true;
  }
  *reason = "must be an encoded absolute URL path without a query, fragment, or dot segments";
  return false;
}

static bool validate_ca_file(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (clean_text(text) && (text.empty() || text.front() == '/')) return true;
  *reason = "must be an absolute PEM CA bundle path or empty";
  return false;
}

static bool validate_certificate_action(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (text == "reject" || text == "prompt") return true;
  *reason = "must be reject or prompt";
  return false;
}

SettingKey webdav_setting_key(std::string name) {
  return make_setting_key("webdav", std::move(name));
}

std::vector<SettingDefinition> webdav_connection_setting_definitions() {
  return {
      {.key = webdav_setting_key("scheme"), .default_value = std::string("https"), .validate = validate_scheme, .retain_invalid = true},
      {.key = webdav_setting_key("address"), .default_value = std::string(), .validate = validate_address, .retain_invalid = true},
      {.key = webdav_setting_key("port"), .default_value = gint64{443}, .validate = validate_port, .retain_invalid = true},
      {.key = webdav_setting_key("base_path"), .default_value = std::string("/"), .validate = validate_base_path, .retain_invalid = true},
      {.key = webdav_setting_key("authentication"), .default_value = std::string("auto"), .validate = validate_authentication, .retain_invalid = true},
      {.key = webdav_setting_key("username"), .default_value = std::string(), .validate = validate_text, .retain_invalid = true},
      {.key = webdav_setting_key("local_directory"), .default_value = std::string(), .validate = validate_text, .retain_invalid = true},
      {.key = webdav_setting_key("remote_directory"), .default_value = std::string("/"), .validate = validate_remote_directory, .retain_invalid = true},
      {.key = webdav_setting_key("ca_file"), .default_value = std::string(), .validate = validate_ca_file, .retain_invalid = true},
      {.key = webdav_setting_key("certificate_error_action"), .default_value = std::string("reject"), .validate = validate_certificate_action, .retain_invalid = true},
      {.key = webdav_setting_key("connect_timeout_seconds"), .default_value = gint64{30}, .validate = validate_timeout, .retain_invalid = true},
      {.key = webdav_setting_key("idle_timeout_seconds"), .default_value = gint64{60}, .validate = validate_timeout, .retain_invalid = true},
  };
}

WebdavConnectionSettings webdav_connection_settings(const SettingsStore &store) {
  const auto text = [&store](const char *name, const char *fallback) {
    return setting_string_value_or_default(store, webdav_setting_key(name), fallback);
  };
  WebdavConnectionSettings result;
  result.scheme = text("scheme", "https");
  result.address = text("address", "");
  result.port = setting_value_source(store, webdav_setting_key("port")) == SettingValueSource::built_in
      ? (result.scheme == "http" ? 80 : 443)
      : setting_integer_value_or_default(store, webdav_setting_key("port"), 443);
  result.base_path = text("base_path", "/");
  const auto authentication = text("authentication", "auto");
  result.authentication = authentication == "none" ? WebdavAuthentication::none
      : authentication == "basic" ? WebdavAuthentication::basic
      : authentication == "digest" ? WebdavAuthentication::digest
      : WebdavAuthentication::automatic;
  result.username = text("username", "");
  result.local_directory = text("local_directory", "");
  result.remote_directory = text("remote_directory", "/");
  result.ca_file = text("ca_file", "");
  result.prompt_certificate = text("certificate_error_action", "reject") == "prompt";
  result.connect_timeout_seconds = setting_integer_value_or_default(store, webdav_setting_key("connect_timeout_seconds"), 30);
  result.idle_timeout_seconds = setting_integer_value_or_default(store, webdav_setting_key("idle_timeout_seconds"), 60);
  for (const auto &entry : store.entries) {
    if (entry.definition.key.section != "webdav" || entry.validation_error.empty()) continue;
    const auto source = setting_value_source(store, entry.definition.key);
    result.validation_errors.push_back("[webdav] " + entry.definition.key.name + " (" +
        (source == SettingValueSource::global ? "global" : "connection") + "): " + entry.validation_error);
  }
  return result;
}

} // namespace elder_terms

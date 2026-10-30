// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings/smb-settings.h>

#include <algorithm>
#include <utility>

#include <elder-terms/settings/general-settings.h>

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

static bool validate_remote_directory(const SettingValue &value,
                                      std::string *reason) {
  const auto &path = std::get<std::string>(value);
  if (!path.empty() && path.front() == '/' && clean_text(path) &&
      path.find('\\') == std::string::npos) {
    std::size_t start = 1;
    while (start < path.size()) {
      const auto end = path.find('/', start);
      const auto part = path.substr(start, end == std::string::npos ? end : end - start);
      if (part.empty() || part == "." || part == ".." ||
          part.find(':') != std::string::npos || part.back() == '.' ||
          part.back() == ' ') break;
      if (end == std::string::npos) return true;
      start = end + 1;
    }
    if (start == path.size()) return true;
  }
  *reason = "must be an absolute path without dot segments, colons, trailing dots or spaces, or control characters";
  return false;
}

SettingKey smb_setting_key(std::string name) {
  return make_setting_key("smb", std::move(name));
}

std::vector<SettingDefinition> smb_connection_setting_definitions() {
  return {
      {.key = smb_setting_key("address"), .default_value = std::string(), .validate = validate_address, .retain_invalid = true},
      {.key = smb_setting_key("port"), .default_value = gint64{445}, .validate = validate_port, .retain_invalid = true},
      {.key = smb_setting_key("domain"), .default_value = std::string(), .validate = validate_text, .retain_invalid = true},
      {.key = smb_setting_key("username"), .default_value = std::string(), .validate = validate_text, .retain_invalid = true},
      {.key = smb_setting_key("local_directory"), .default_value = std::string(), .validate = validate_text, .retain_invalid = true},
      {.key = smb_setting_key("remote_directory"), .default_value = std::string("/"), .validate = validate_remote_directory, .retain_invalid = true},
      {.key = smb_setting_key("require_signing"), .default_value = true},
      {.key = smb_setting_key("require_encryption"), .default_value = false},
      {.key = smb_setting_key("connect_timeout_seconds"), .default_value = gint64{30}, .validate = validate_timeout, .retain_invalid = true},
      {.key = smb_setting_key("idle_timeout_seconds"), .default_value = gint64{60}, .validate = validate_timeout, .retain_invalid = true},
  };
}

SmbConnectionSettings smb_connection_settings(const SettingsStore &store) {
  const auto text = [&store](const char *name, const char *fallback) {
    return setting_string_value_or_default(store, smb_setting_key(name), fallback);
  };
  SmbConnectionSettings result;
  result.address = text("address", "");
  result.port = setting_integer_value_or_default(store, smb_setting_key("port"), 445);
  result.domain = text("domain", "");
  result.username = text("username", "");
  result.local_directory = text("local_directory", "");
  result.remote_directory = text("remote_directory", "/");
  result.require_signing = setting_boolean_value_or_default(store, smb_setting_key("require_signing"), true);
  result.require_encryption = setting_boolean_value_or_default(store, smb_setting_key("require_encryption"), false);
  result.connect_timeout_seconds = setting_integer_value_or_default(store, smb_setting_key("connect_timeout_seconds"), 30);
  result.idle_timeout_seconds = setting_integer_value_or_default(store, smb_setting_key("idle_timeout_seconds"), 60);
  for (const auto &entry : store.entries) {
    if (entry.definition.key.section != "smb" || entry.validation_error.empty()) continue;
    const auto source = setting_value_source(store, entry.definition.key);
    result.validation_errors.push_back("[smb] " + entry.definition.key.name + " (" +
        (source == SettingValueSource::global ? "global" : "connection") + "): " + entry.validation_error);
  }
  if (general_connection_kind(store) == ConnectionKind::smb) {
    if (result.address.empty()) result.validation_errors.emplace_back("SMB server address is required");
  }
  return result;
}

} // namespace elder_terms

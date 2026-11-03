// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings/smb-settings.h>
#include "validation-issues.h"

#include <algorithm>
#include <utility>

#include <elder-terms/settings/general-settings.h>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

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

static bool validate_version(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (text == "auto" || text == "1" || text == "2" || text == "3") return true;
  *reason = "must be auto, 1, 2, or 3";
  return false;
}

static bool validate_netbios_name(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (text.size() <= 15 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return g_ascii_isalnum(c) || c == '-' || c == '_';
      })) return true;
  *reason = "must be at most 15 ASCII letters, digits, hyphens, or underscores";
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
      {.key = smb_setting_key("version"), .default_value = std::string("auto"), .validate = validate_version, .retain_invalid = true},
      {.key = smb_setting_key("address"), .default_value = std::string(), .validate = validate_address, .retain_invalid = true},
      {.key = smb_setting_key("port"), .default_value = gint64{445}, .validate = validate_port, .retain_invalid = true},
      {.key = smb_setting_key("netbios_name"), .default_value = std::string(), .validate = validate_netbios_name, .retain_invalid = true},
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
  result.version = text("version", "auto");
  result.address = text("address", "");
  result.port = setting_integer_value_or_default(store, smb_setting_key("port"), 445);
  result.netbios_name = text("netbios_name", "");
  result.domain = text("domain", "");
  result.username = text("username", "");
  result.local_directory = text("local_directory", "");
  result.remote_directory = text("remote_directory", "/");
  result.require_signing = setting_boolean_value_or_default(store, smb_setting_key("require_signing"), true);
  if (result.version == "1" && setting_value_source(store, smb_setting_key("require_signing")) == SettingValueSource::built_in)
    result.require_signing = false;
  result.require_encryption = setting_boolean_value_or_default(store, smb_setting_key("require_encryption"), false);
  result.connect_timeout_seconds = setting_integer_value_or_default(store, smb_setting_key("connect_timeout_seconds"), 30);
  result.idle_timeout_seconds = setting_integer_value_or_default(store, smb_setting_key("idle_timeout_seconds"), 60);
  for (const auto &entry : store.entries) {
    if (entry.definition.key.section != "smb" || entry.validation_error.empty()) continue;
    result.validation_issues.push_back({{entry.definition.key}, entry.validation_error});
    const auto source = setting_value_source(store, entry.definition.key);
    result.validation_errors.push_back("[smb] " + entry.definition.key.name + " (" +
        (source == SettingValueSource::global ? "global" : "connection") + "): " + entry.validation_error);
  }
  if (general_connection_kind(store) == ConnectionKind::smb) {
    if (result.address.empty()) add_validation_issue(result, {smb_setting_key("address")}, "SMB server address is required");
  }
  if (result.version == "1") {
    if (result.port != 445 && result.port != 139)
      add_validation_issue(result, {smb_setting_key("port"), smb_setting_key("version")}, _("[smb] port: SMB 1 supports only 445 and 139"));
    if (result.port == 139 && result.netbios_name.empty())
      add_validation_issue(result, {smb_setting_key("netbios_name")}, _("[smb] netbios_name: SMB 1 on port 139 requires the server NetBIOS name"));
    if (result.address.find(':') != std::string::npos)
      add_validation_issue(result, {smb_setting_key("address"), smb_setting_key("version")}, _("[smb] address: SMB 1 requires IPv4"));
    if (result.require_signing)
      add_validation_issue(result, {smb_setting_key("require_signing"), smb_setting_key("version")}, _("[smb] require_signing: SMB 1 cannot satisfy required signing"));
    if (result.require_encryption)
      add_validation_issue(result, {smb_setting_key("require_encryption"), smb_setting_key("version")}, _("[smb] require_encryption: SMB 1 cannot satisfy required encryption"));
  } else if (result.version == "2" && result.require_encryption)
    add_validation_issue(result, {smb_setting_key("require_encryption"), smb_setting_key("version")}, _("[smb] require_encryption: SMB encryption requires SMB 3"));
  return result;
}

} // namespace elder_terms

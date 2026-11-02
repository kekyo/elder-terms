// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings/nfs-settings.h>
#include <elder-terms/settings/general-settings.h>
#include <algorithm>
#include <charconv>
#include <iconv.h>
#include <utility>

namespace elder_terms {

static bool clean_text(const std::string &text) {
  return g_utf8_validate(text.data(), text.size(), nullptr) &&
      std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

static bool validate_text(const SettingValue &value, std::string *reason) {
  if (clean_text(std::get<std::string>(value))) return true;
  *reason = "must contain valid UTF-8 without control characters";
  return false;
}

static bool validate_address(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (clean_text(text) && text.find_first_of(" /\\@?#") == std::string::npos) return true;
  *reason = "must be a hostname or IP address without a scheme, path, or credentials";
  return false;
}

static bool validate_version(const SettingValue &value, std::string *reason) {
  if (std::get<std::string>(value) == "3") return true;
  *reason = "must select the supported NFS version 3";
  return false;
}

static bool validate_port(const SettingValue &value, std::string *reason) {
  const auto port = std::get<gint64>(value);
  if (port >= 0 && port <= 65535) return true;
  *reason = "must be between 0 (automatic) and 65535";
  return false;
}

static bool validate_timeout(const SettingValue &value, std::string *reason) {
  const auto seconds = std::get<gint64>(value);
  if (seconds >= 1 && seconds <= 86400) return true;
  *reason = "must be between 1 and 86400 seconds";
  return false;
}

static bool validate_id(const SettingValue &value, std::string *reason) {
  const auto id = std::get<gint64>(value);
  if (id >= -1 && id <= UINT32_MAX) return true;
  *reason = "must be -1 (current process) or an unsigned 32-bit ID";
  return false;
}

static std::optional<std::uint32_t> group_id(const std::string &text) {
  std::uint32_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

static bool validate_groups(const SettingValue &value, std::string *reason) {
  const auto &groups = std::get<std::vector<std::string>>(value);
  if (groups.size() <= 16 && std::all_of(groups.begin(), groups.end(),
      [](const auto &id) { return group_id(id).has_value(); })) return true;
  *reason = "must contain at most 16 unsigned 32-bit group IDs";
  return false;
}

static bool valid_path(const std::string &path) {
  if (path.empty() || path == "/") return true;
  if (!clean_text(path) || path.front() != '/') return false;
  for (std::size_t start = 1; start < path.size();) {
    const auto next = path.find('/', start);
    const auto part = path.substr(start, next == std::string::npos ? next : next - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (next == std::string::npos) return true;
    start = next + 1;
  }
  return false;
}

static bool validate_path(const SettingValue &value, std::string *reason) {
  if (valid_path(std::get<std::string>(value))) return true;
  *reason = "must be empty or an absolute path without empty, dot, or parent components";
  return false;
}

static bool validate_paths(const SettingValue &value, std::string *reason) {
  const auto &paths = std::get<std::vector<std::string>>(value);
  if (std::all_of(paths.begin(), paths.end(), [](const auto &path) { return !path.empty() && valid_path(path); })) return true;
  *reason = "must contain absolute paths without empty, dot, or parent components";
  return false;
}

static bool validate_encoding(const SettingValue &value, std::string *reason) {
  const auto &encoding = std::get<std::string>(value);
  if (!encoding.empty() && clean_text(encoding) && encoding.find_first_of(" /") == std::string::npos) {
    const auto decoder = iconv_open("UTF-8", encoding.c_str());
    const auto encoder = iconv_open(encoding.c_str(), "UTF-8");
    const auto invalid = reinterpret_cast<iconv_t>(-1);
    if (decoder != invalid) iconv_close(decoder);
    if (encoder != invalid) iconv_close(encoder);
    if (decoder != invalid && encoder != invalid) return true;
  }
  *reason = "must be an available encoding without replacement or ignore modifiers";
  return false;
}

SettingKey nfs_setting_key(std::string name) { return make_setting_key("nfs", std::move(name)); }

std::vector<SettingDefinition> nfs_connection_setting_definitions() {
  return {
      {.key = nfs_setting_key("address"), .default_value = std::string(), .validate = validate_address, .retain_invalid = true},
      {.key = nfs_setting_key("version"), .default_value = std::string("3"), .validate = validate_version, .retain_invalid = true},
      {.key = nfs_setting_key("nfs_port"), .default_value = gint64{0}, .validate = validate_port, .retain_invalid = true},
      {.key = nfs_setting_key("mount_port"), .default_value = gint64{0}, .validate = validate_port, .retain_invalid = true},
      {.key = nfs_setting_key("export_path"), .default_value = std::string(), .validate = validate_path, .retain_invalid = true},
      {.key = nfs_setting_key("known_exports"), .default_value = std::vector<std::string>(), .validate = validate_paths, .retain_invalid = true},
      {.key = nfs_setting_key("discover_exports"), .default_value = true},
      {.key = nfs_setting_key("filename_encoding"), .default_value = std::string("UTF-8"), .validate = validate_encoding, .retain_invalid = true},
      {.key = nfs_setting_key("local_directory"), .default_value = std::string(), .validate = validate_text, .retain_invalid = true},
      {.key = nfs_setting_key("uid"), .default_value = gint64{-1}, .validate = validate_id, .retain_invalid = true},
      {.key = nfs_setting_key("gid"), .default_value = gint64{-1}, .validate = validate_id, .retain_invalid = true},
      {.key = nfs_setting_key("supplementary_gids"), .default_value = std::vector<std::string>(), .validate = validate_groups, .retain_invalid = true},
      {.key = nfs_setting_key("connect_timeout_seconds"), .default_value = gint64{30}, .validate = validate_timeout, .retain_invalid = true},
      {.key = nfs_setting_key("request_timeout_seconds"), .default_value = gint64{60}, .validate = validate_timeout, .retain_invalid = true},
  };
}

NfsConnectionSettings nfs_connection_settings(const SettingsStore &store) {
  const auto text = [&store](const char *name, const char *fallback) {
    return setting_string_value_or_default(store, nfs_setting_key(name), fallback);
  };
  const auto number = [&store](const char *name, gint64 fallback) {
    return setting_integer_value_or_default(store, nfs_setting_key(name), fallback);
  };
  const auto list = [&store](const char *name) {
    return std::get<std::vector<std::string>>(setting_value_or_default(store, nfs_setting_key(name), std::vector<std::string>()));
  };
  NfsConnectionSettings result;
  result.address = text("address", "");
  result.version = text("version", "3");
  result.nfs_port = number("nfs_port", 0);
  result.mount_port = number("mount_port", 0);
  result.export_path = text("export_path", "");
  result.known_exports = list("known_exports");
  result.discover_exports = setting_boolean_value_or_default(store, nfs_setting_key("discover_exports"), true);
  result.filename_encoding = text("filename_encoding", "UTF-8");
  result.local_directory = text("local_directory", "");
  const auto uid = number("uid", -1), gid = number("gid", -1);
  if (uid >= 0 && uid <= UINT32_MAX) result.uid = static_cast<std::uint32_t>(uid);
  if (gid >= 0 && gid <= UINT32_MAX) result.gid = static_cast<std::uint32_t>(gid);
  for (const auto &text : list("supplementary_gids")) {
    if (const auto id = group_id(text)) result.supplementary_gids.push_back(*id);
  }
  result.connect_timeout_seconds = number("connect_timeout_seconds", 30);
  result.request_timeout_seconds = number("request_timeout_seconds", 60);
  for (const auto &entry : store.entries) {
    if (entry.definition.key.section != "nfs" || entry.validation_error.empty()) continue;
    const auto source = setting_value_source(store, entry.definition.key);
    result.validation_errors.push_back("[nfs] " + entry.definition.key.name + " (" +
        (source == SettingValueSource::global ? "global" : "connection") + "): " + entry.validation_error);
  }
  if (general_connection_kind(store) == ConnectionKind::nfs && result.address.empty())
    result.validation_errors.emplace_back("NFS server address is required");
  if (!result.discover_exports && result.export_path.empty() && result.known_exports.empty())
    result.validation_errors.emplace_back("A known export path is required when export discovery is disabled");
  return result;
}

} // namespace elder_terms

#include <elder-terms/settings/log-settings.h>

#include <array>
#include <filesystem>
#include <string_view>

namespace elder_terms {

static constexpr char log_section[] = "log";
static constexpr char log_enabled_key[] = "enabled";
static constexpr char log_base_directory_key[] = "base_directory";
static constexpr char log_file_name_format_key[] = "file_name_format";
static constexpr char log_mode_key[] = "mode";
static constexpr bool default_log_enabled = false;
static constexpr char default_log_base_directory[] = ".";
static constexpr char default_log_file_name_format[] =
    "{YYYYMMDD}_{hhmmss}_{fff}.txt";
static constexpr char default_log_mode[] = "raw";
static constexpr std::array<std::string_view, 3> log_format_placeholders{
    "{YYYYMMDD}",
    "{hhmmss}",
    "{fff}",
};

static SettingKey log_key(const char *name) {
  return make_setting_key(log_section, name);
}

static bool validate_terminal_log_file_name_format(
    const SettingValue &value, std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr) {
    *reason = "must be a string";
    return false;
  }
  return terminal_log_file_name_format_is_valid(*text, reason);
}

static bool validate_terminal_log_mode(const SettingValue &value,
                                       std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr || (*text != "raw" && *text != "cooked")) {
    *reason = "must be raw or cooked";
    return false;
  }
  return true;
}

static bool path_has_parent_traversal(const std::filesystem::path &path) {
  for (const std::filesystem::path &component : path) {
    if (component == "..") {
      return true;
    }
  }
  return false;
}

static bool placeholders_are_valid(const std::string &format,
                                   std::string *reason) {
  std::size_t index = 0;
  while (index < format.size()) {
    if (format[index] == '}') {
      *reason = "contains an unmatched closing brace";
      return false;
    }
    if (format[index] != '{') {
      ++index;
      continue;
    }

    const std::size_t end = format.find('}', index + 1);
    if (end == std::string::npos) {
      *reason = "contains an unmatched opening brace";
      return false;
    }
    const std::string_view placeholder(format.data() + index,
                                       end - index + 1);
    bool known = false;
    for (std::string_view candidate : log_format_placeholders) {
      if (placeholder == candidate) {
        known = true;
        break;
      }
    }
    if (!known) {
      *reason = "contains an unknown placeholder";
      return false;
    }
    index = end + 1;
  }
  return true;
}

SettingKey terminal_log_enabled_setting_key() {
  return log_key(log_enabled_key);
}

SettingKey terminal_log_base_directory_setting_key() {
  return log_key(log_base_directory_key);
}

SettingKey terminal_log_file_name_format_setting_key() {
  return log_key(log_file_name_format_key);
}

SettingKey terminal_log_mode_setting_key() {
  return log_key(log_mode_key);
}

bool terminal_log_file_name_format_is_valid(const std::string &format,
                                            std::string *reason) {
  std::string ignored_reason;
  std::string *failure_reason = reason == nullptr ? &ignored_reason : reason;
  if (format.empty()) {
    *failure_reason = "must not be empty";
    return false;
  }

  const std::filesystem::path path(format);
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    *failure_reason = "must be relative to the log base directory";
    return false;
  }
  if (path_has_parent_traversal(path)) {
    *failure_reason = "must not contain parent directory traversal";
    return false;
  }
  if (!path.has_filename() || path.filename() == "." ||
      path.filename() == "..") {
    *failure_reason = "must name a log file";
    return false;
  }
  if (!placeholders_are_valid(format, failure_reason)) {
    return false;
  }

  failure_reason->clear();
  return true;
}

const char *terminal_log_mode_to_string(TerminalLogMode mode) {
  return mode == TerminalLogMode::cooked ? "cooked" : "raw";
}

std::vector<SettingDefinition> terminal_log_setting_definitions() {
  return {
      {
          .key = terminal_log_enabled_setting_key(),
          .default_value = SettingValue{default_log_enabled},
          .validate = nullptr,
      },
      {
          .key = terminal_log_base_directory_setting_key(),
          .default_value =
              SettingValue{std::string(default_log_base_directory)},
          .validate = nullptr,
      },
      {
          .key = terminal_log_file_name_format_setting_key(),
          .default_value =
              SettingValue{std::string(default_log_file_name_format)},
          .validate = validate_terminal_log_file_name_format,
      },
      {
          .key = terminal_log_mode_setting_key(),
          .default_value = SettingValue{std::string(default_log_mode)},
          .validate = validate_terminal_log_mode,
      },
  };
}

TerminalLogSettings terminal_log_settings(const SettingsStore &store) {
  const std::string mode = setting_string_value_or_default(
      store, terminal_log_mode_setting_key(), default_log_mode);
  return {
      .enabled = setting_boolean_value_or_default(
          store, terminal_log_enabled_setting_key(), default_log_enabled),
      .base_directory = setting_string_value_or_default(
          store, terminal_log_base_directory_setting_key(),
          default_log_base_directory),
      .file_name_format = setting_string_value_or_default(
          store, terminal_log_file_name_format_setting_key(),
          default_log_file_name_format),
      .mode = mode == "cooked" ? TerminalLogMode::cooked
                               : TerminalLogMode::raw,
  };
}

} // namespace elder_terms

#include <elder-terms/settings/log-settings.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <string_view>

#include <glib.h>

#include <elder-terms/settings/general-settings.h>

namespace elder_terms {

static constexpr char log_section[] = "log";
static constexpr char log_enabled_key[] = "enabled";
static constexpr char log_base_directory_key[] = "base_directory";
static constexpr char log_file_name_format_key[] = "file_name_format";
static constexpr char log_mode_key[] = "mode";
static constexpr bool default_log_enabled = false;
static constexpr char default_log_base_directory[] =
    "{documents}/logs/";
static constexpr char default_log_file_name_format[] =
    "{YYYYMMDD}_{hhmmss}_{fff}.txt";
static constexpr char default_log_mode[] = "raw";

struct LogPathFormatValues {
  std::string documents;
  std::string downloads;
  std::string home;
  std::string name;
  std::string year;
  std::string month;
  std::string day;
  std::string hour;
  std::string minute;
  std::string second;
  std::string millisecond;
};

struct TemporalFormatField {
  std::string_view token;
  const std::string LogPathFormatValues::*value;
};

static constexpr std::array<TemporalFormatField, 7> temporal_format_fields{
    TemporalFormatField{"YYYY", &LogPathFormatValues::year},
    TemporalFormatField{"fff", &LogPathFormatValues::millisecond},
    TemporalFormatField{"MM", &LogPathFormatValues::month},
    TemporalFormatField{"DD", &LogPathFormatValues::day},
    TemporalFormatField{"hh", &LogPathFormatValues::hour},
    TemporalFormatField{"mm", &LogPathFormatValues::minute},
    TemporalFormatField{"ss", &LogPathFormatValues::second},
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

static bool append_temporal_format(std::string_view format,
                                   const LogPathFormatValues &values,
                                   std::string *output) {
  bool appended_field = false;
  std::size_t index = 0;
  while (index < format.size()) {
    bool matched = false;
    for (const TemporalFormatField &field : temporal_format_fields) {
      if (!format.substr(index).starts_with(field.token)) {
        continue;
      }
      output->append(values.*(field.value));
      index += field.token.size();
      appended_field = true;
      matched = true;
      break;
    }
    if (matched) {
      continue;
    }

    // Punctuation is copied as a separator; unrecognized field text is not.
    const unsigned char character =
        static_cast<unsigned char>(format[index]);
    if (character == '\0' || character == '{' || character == '}' ||
        g_ascii_isalnum(character)) {
      return false;
    }
    output->push_back(static_cast<char>(character));
    ++index;
  }
  return appended_field;
}

static bool append_named_format(std::string_view format,
                                const LogPathFormatValues &values,
                                std::string *output) {
  if (format == "documents") {
    output->append(values.documents);
  } else if (format == "downloads") {
    output->append(values.downloads);
  } else if (format == "home") {
    output->append(values.home);
  } else if (format == "name") {
    output->append(values.name);
  } else {
    return false;
  }
  return true;
}

static bool expand_log_path_format(const std::string &format,
                                   const LogPathFormatValues &values,
                                   bool preserve_unknown,
                                   std::string *output,
                                   std::string *reason) {
  output->clear();
  std::size_t index = 0;
  while (index < format.size()) {
    if (format[index] == '\0') {
      *reason = "contains a NUL byte";
      return false;
    }
    if (format[index] == '}') {
      *reason = "contains an unmatched closing brace";
      return false;
    }
    if (format[index] != '{') {
      output->push_back(format[index]);
      ++index;
      continue;
    }

    const std::size_t end = format.find('}', index + 1);
    if (end == std::string::npos) {
      *reason = "contains an unmatched opening brace";
      return false;
    }
    const std::string_view placeholder(format.data() + index + 1,
                                       end - index - 1);
    std::string expanded;
    if (!append_named_format(placeholder, values, &expanded) &&
        !append_temporal_format(placeholder, values, &expanded)) {
      if (!preserve_unknown) {
        *reason = "contains an unknown placeholder";
        return false;
      }
      output->append(format, index, end - index + 1);
    } else {
      output->append(expanded);
    }
    index = end + 1;
  }
  return true;
}

static std::string format_number(int value, int width) {
  std::array<char, 32> buffer{};
  const int written =
      std::snprintf(buffer.data(), buffer.size(), "%0*d", width, value);
  if (written < 0 || static_cast<std::size_t>(written) >= buffer.size()) {
    throw std::runtime_error("failed to format terminal log timestamp");
  }
  return std::string(buffer.data(), static_cast<std::size_t>(written));
}

static std::filesystem::path user_home_directory() {
  const char *home = g_get_home_dir();
  if (home == nullptr || home[0] == '\0') {
    throw std::runtime_error(
        "failed to resolve the user home for terminal logging");
  }
  return home;
}

static std::filesystem::path user_special_directory(
    GUserDirectory directory, const std::filesystem::path &home,
    const char *fallback_name) {
  const char *configured = g_get_user_special_dir(directory);
  return configured == nullptr || configured[0] == '\0'
             ? home / fallback_name
             : std::filesystem::path(configured);
}

static std::string sanitized_connection_name(std::string name) {
  for (char &character : name) {
    if (character == '/' || character == '\0') {
      character = '_';
    }
  }
  if (name.empty()) {
    return "_";
  }
  if (name == "." || name == "..") {
    for (char &character : name) {
      character = '_';
    }
  }
  return name;
}

static LogPathFormatValues validation_format_values() {
  return {
      .documents = "documents",
      .downloads = "downloads",
      .home = "home",
      .name = "name",
      .year = "2000",
      .month = "01",
      .day = "02",
      .hour = "03",
      .minute = "04",
      .second = "05",
      .millisecond = "006",
  };
}

static LogPathFormatValues runtime_format_values(
    const TerminalLogSettings &settings,
    std::chrono::system_clock::time_point now) {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  if (::localtime_r(&seconds, &local) == nullptr) {
    throw std::runtime_error("failed to resolve local terminal log time");
  }
  const auto elapsed_milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count();
  const int millisecond = static_cast<int>(
      (elapsed_milliseconds % 1000 + 1000) % 1000);
  const std::filesystem::path home = user_home_directory();

  return {
      .documents =
          user_special_directory(G_USER_DIRECTORY_DOCUMENTS, home,
                                 "Documents")
              .string(),
      .downloads =
          user_special_directory(G_USER_DIRECTORY_DOWNLOAD, home,
                                 "Downloads")
              .string(),
      .home = home.string(),
      .name = sanitized_connection_name(settings.connection_name),
      .year = format_number(local.tm_year + 1900, 4),
      .month = format_number(local.tm_mon + 1, 2),
      .day = format_number(local.tm_mday, 2),
      .hour = format_number(local.tm_hour, 2),
      .minute = format_number(local.tm_min, 2),
      .second = format_number(local.tm_sec, 2),
      .millisecond = format_number(millisecond, 3),
  };
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

  std::string expanded;
  if (!expand_log_path_format(format, validation_format_values(), false,
                              &expanded, failure_reason)) {
    return false;
  }

  const std::filesystem::path path(expanded);
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

  failure_reason->clear();
  return true;
}

std::filesystem::path resolve_terminal_log_path(
    const TerminalLogSettings &settings,
    std::chrono::system_clock::time_point now) {
  std::string reason;
  if (!terminal_log_file_name_format_is_valid(settings.file_name_format,
                                               &reason)) {
    throw std::invalid_argument("invalid terminal log file name format: " +
                                reason);
  }

  const LogPathFormatValues values = runtime_format_values(settings, now);
  std::string base;
  if (!expand_log_path_format(settings.base_directory, values, true, &base,
                              &reason)) {
    throw std::invalid_argument("invalid terminal log base directory format: " +
                                reason);
  }
  std::string file_name;
  if (!expand_log_path_format(settings.file_name_format, values, false,
                              &file_name, &reason)) {
    throw std::invalid_argument("invalid terminal log file name format: " +
                                reason);
  }

  return (std::filesystem::path(base) / std::filesystem::path(file_name))
      .lexically_normal();
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
      .connection_name = general_connection_name(store),
      .mode = mode == "cooked" ? TerminalLogMode::cooked
                               : TerminalLogMode::raw,
  };
}

} // namespace elder_terms

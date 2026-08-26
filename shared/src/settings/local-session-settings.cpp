#include <elder-terms/settings/local-session-settings.h>

#include <algorithm>
#include <cctype>
#include <utility>

#include <glib.h>

namespace elder_terms {

static constexpr char local_section[] = "local";
static constexpr char local_command_line_key[] = "command_line";

static bool string_is_blank(const std::string &value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isspace(character) != 0;
  });
}

static bool parse_command_line(const std::string &command_line,
                               std::vector<std::string> *arguments,
                               std::string *reason) {
  std::string ignored_reason;
  std::string *failure_reason = reason == nullptr ? &ignored_reason : reason;
  if (arguments != nullptr) {
    arguments->clear();
  }
  if (string_is_blank(command_line)) {
    failure_reason->clear();
    return true;
  }

  gint argument_count = 0;
  gchar **parsed_arguments = nullptr;
  GError *error = nullptr;
  const gboolean parsed = g_shell_parse_argv(
      command_line.c_str(), &argument_count, &parsed_arguments, &error);
  if (!parsed || argument_count <= 0 || parsed_arguments == nullptr) {
    *failure_reason =
        error != nullptr && error->message != nullptr
            ? error->message
            : "must be a valid shell-style argument list";
    g_clear_error(&error);
    g_strfreev(parsed_arguments);
    return false;
  }

  if (arguments != nullptr) {
    arguments->reserve(static_cast<std::size_t>(argument_count));
    for (gint index = 0; index < argument_count; ++index) {
      arguments->emplace_back(parsed_arguments[index]);
    }
  }
  failure_reason->clear();
  g_strfreev(parsed_arguments);
  return true;
}

static bool validate_command_line(const SettingValue &value,
                                  std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr) {
    *reason = "must be text";
    return false;
  }
  return local_command_line_is_valid(*text, reason);
}

SettingKey local_command_line_setting_key() {
  return make_setting_key(local_section, local_command_line_key);
}

std::vector<SettingDefinition> local_shell_connection_setting_definitions() {
  return {
      {
          .key = local_command_line_setting_key(),
          .default_value = SettingValue{std::string()},
          .validate = validate_command_line,
      },
  };
}

bool local_command_line_is_valid(const std::string &command_line,
                                 std::string *reason) {
  return parse_command_line(command_line, nullptr, reason);
}

LocalShellConnectionSettings
local_shell_connection_settings(const SettingsStore &store) {
  const std::string command_line = setting_string_value_or_default(
      store, local_command_line_setting_key(), std::string());
  std::vector<std::string> arguments;
  (void)parse_command_line(command_line, &arguments, nullptr);
  return {
      .argv = std::move(arguments),
  };
}

} // namespace elder_terms

#include <elder-terms/settings/macro-settings.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>

#include <glib.h>

namespace elder_terms {

static std::string trim_ascii_whitespace(const std::string &value) {
  const auto first = std::find_if_not(
      value.begin(), value.end(),
      [](unsigned char character) { return std::isspace(character) != 0; });
  const auto last = std::find_if_not(
                        value.rbegin(), value.rend(),
                        [](unsigned char character) {
                          return std::isspace(character) != 0;
                        })
                        .base();
  if (first >= last) {
    return {};
  }
  return std::string(first, last);
}

static bool decimal_capture_is_valid(const std::string &capture,
                                     gint capture_count) {
  if (capture.empty() ||
      !std::all_of(capture.begin(), capture.end(), [](unsigned char value) {
        return std::isdigit(value) != 0;
      })) {
    return false;
  }

  guint64 number = 0;
  for (unsigned char value : capture) {
    const guint digit = static_cast<guint>(value - '0');
    if (number > (std::numeric_limits<guint64>::max() - digit) / 10) {
      return false;
    }
    number = number * 10 + digit;
  }
  return number <= static_cast<guint64>(capture_count);
}

static bool macro_template_is_valid(const std::string &value, GRegex *regex,
                                    std::string *reason) {
  for (std::size_t index = 0; index < value.size();) {
    if (value[index] != '$') {
      ++index;
      continue;
    }
    if (index + 1 < value.size() && value[index + 1] == '$') {
      index += 2;
      continue;
    }
    if (index + 1 >= value.size() || value[index + 1] != '{') {
      *reason = "literal '$' must be written as '$$'";
      return false;
    }

    const std::size_t closing = value.find('}', index + 2);
    if (closing == std::string::npos) {
      *reason = "contains an unmatched capture placeholder";
      return false;
    }
    const std::string capture = value.substr(index + 2, closing - index - 2);
    if (capture.empty()) {
      *reason = "contains an empty capture placeholder";
      return false;
    }

    const bool starts_with_digit =
        std::isdigit(static_cast<unsigned char>(capture.front())) != 0;
    if (starts_with_digit) {
      if (!decimal_capture_is_valid(capture,
                                    g_regex_get_capture_count(regex))) {
        *reason = "references an unknown capture: " + capture;
        return false;
      }
    } else if (g_regex_get_string_number(regex, capture.c_str()) < 0) {
      *reason = "references an unknown capture: " + capture;
      return false;
    }
    index = closing + 1;
  }
  return true;
}

bool macro_rule_id_is_valid(const std::string &id, std::string *reason) {
  if (id.empty()) {
    *reason = "identifier must not be empty";
    return false;
  }
  if (!std::all_of(id.begin(), id.end(), [](unsigned char value) {
        const bool ascii_letter = (value >= 'A' && value <= 'Z') ||
                                  (value >= 'a' && value <= 'z');
        const bool ascii_digit = value >= '0' && value <= '9';
        return ascii_letter || ascii_digit || value == '-' || value == '_';
      })) {
    *reason = "identifier may contain only letters, digits, '-' and '_'";
    return false;
  }
  return true;
}

bool macro_rule_is_valid(const MacroRule &rule, std::string *reason) {
  if (!macro_rule_id_is_valid(rule.id, reason)) {
    return false;
  }
  if (rule.pattern.empty()) {
    *reason = "regular expression must not be empty";
    return false;
  }

  GError *error = nullptr;
  GRegex *regex = g_regex_new(rule.pattern.c_str(), G_REGEX_DEFAULT,
                              G_REGEX_MATCH_DEFAULT, &error);
  if (regex == nullptr) {
    *reason = error == nullptr || error->message == nullptr
                  ? "invalid regular expression"
                  : std::string(error->message);
    g_clear_error(&error);
    return false;
  }

  bool valid = false;
  if (const auto *send = std::get_if<MacroSendAction>(&rule.action)) {
    if (send->text.empty()) {
      *reason = "send text must not be empty";
    } else {
      valid = macro_template_is_valid(send->text, regex, reason);
    }
  } else {
    const auto &command = std::get<MacroCommandAction>(rule.action);
    if (trim_ascii_whitespace(command.command).empty()) {
      *reason = "command must not be empty";
    } else if (!macro_template_is_valid(command.command, regex, reason)) {
      valid = false;
    } else {
      valid = std::all_of(
          command.arguments.begin(), command.arguments.end(),
          [regex, reason](const std::string &argument) {
            return macro_template_is_valid(argument, regex, reason);
          });
    }
  }

  g_regex_unref(regex);
  return valid;
}

} // namespace elder_terms

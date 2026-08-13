#include <elder-terms/settings/macro-settings.h>

#include <algorithm>
#include <cctype>
#include <string>

#include <glib.h>

#include <elder-terms/settings/regex-capture-template.h>

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

static bool macro_template_is_valid(const std::string &value, GRegex *regex,
                                    std::string *reason) {
  return regex_capture_template_is_valid(
      value, regex,
      RegexCaptureTemplateOptions{.allow_uri_decode = false}, reason);
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

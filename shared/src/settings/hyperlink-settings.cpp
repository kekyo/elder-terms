#include <elder-terms/settings/hyperlink-settings.h>

#include <algorithm>
#include <cctype>
#include <string>

#include <elder-terms/settings/regular-expression.h>
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

std::vector<HyperlinkActionRule> default_hyperlink_action_rules() {
  return {
      {
          .id = "vscode-line-column",
          .pattern = R"(^vscode://file(?<path>/[^:\r\n]+):(?<line>[1-9][0-9]*):(?<column>[1-9][0-9]*)$)",
          .command = "code",
          .arguments = {"--reuse-window", "--goto",
                        "${path|uri-decode}:${line}:${column}"},
      },
      {
          .id = "vscode-line",
          .pattern = R"(^vscode://file(?<path>/[^:\r\n]+):(?<line>[1-9][0-9]*)$)",
          .command = "code",
          .arguments = {"--reuse-window", "--goto",
                        "${path|uri-decode}:${line}"},
      },
  };
}

bool hyperlink_action_rule_id_is_valid(const std::string &id,
                                       std::string *reason) {
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

bool hyperlink_action_rule_is_valid(const HyperlinkActionRule &rule,
                                    std::string *reason) {
  if (!hyperlink_action_rule_id_is_valid(rule.id, reason)) {
    return false;
  }
  if (rule.pattern.empty()) {
    *reason = "regular expression must not be empty";
    return false;
  }
  if (trim_ascii_whitespace(rule.command).empty()) {
    *reason = "command must not be empty";
    return false;
  }
  if (rule.command.find('\0') != std::string::npos) {
    *reason = "command must not contain a NUL byte";
    return false;
  }

  RegularExpressionState *regex =
      create_regular_expression(rule.pattern, false, reason);
  if (regex == nullptr) {
    return false;
  }

  const bool valid = std::all_of(
      rule.arguments.begin(), rule.arguments.end(),
      [regex, reason](const std::string &argument) {
        return regex_capture_template_is_valid(
            argument, regex,
            RegexCaptureTemplateOptions{.allow_uri_decode = true}, reason);
      });
  destroy_regular_expression(regex);
  return valid;
}

} // namespace elder_terms

// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

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
  const std::string local_path = R"(~?/(?:[\p{L}\p{M}\p{Nd}_.-]+(?:/[\p{L}\p{M}\p{Nd}_.-]+)*/?)?)";
  return {
      {
          .id = "http-url-osc8",
          .recognition_source = HyperlinkRecognitionSource::osc8,
          .pattern = R"(^https?://[^\s\p{Z})" "\u0085\u180e" R"(<>"']+$)",
          .command = "xdg-open",
          .arguments = {"${0}"},
          .path_validation = HyperlinkPathValidation::none,
          .path_template = {},
      },
      {
          .id = "http-url-text",
          .recognition_source = HyperlinkRecognitionSource::terminal_text,
          .pattern = R"(https?://[^\s\p{Z})" "\u0085\u180e" R"(<>"']+)",
          .command = "xdg-open",
          .arguments = {"${0}"},
          .path_validation = HyperlinkPathValidation::none,
          .path_template = {},
      },
      {
          .id = "vscode-line-column",
          .recognition_source = HyperlinkRecognitionSource::osc8,
          .pattern = R"(^vscode://file(?<path>/[^:\r\n]+):(?<line>[1-9][0-9]*):(?<column>[1-9][0-9]*)$)",
          .command = "code",
          .arguments = {"--reuse-window", "--goto",
                        "${path|uri-decode}:${line}:${column}"},
          .path_validation = HyperlinkPathValidation::none,
          .path_template = {},
      },
      {
          .id = "vscode-line",
          .recognition_source = HyperlinkRecognitionSource::osc8,
          .pattern = R"(^vscode://file(?<path>/[^:\r\n]+):(?<line>[1-9][0-9]*)$)",
          .command = "code",
          .arguments = {"--reuse-window", "--goto",
                        "${path|uri-decode}:${line}"},
          .path_validation = HyperlinkPathValidation::none,
          .path_template = {},
      },
      {
          .id = "local-path-text",
          .recognition_source = HyperlinkRecognitionSource::terminal_text,
          .pattern = local_path,
          .command = "xdg-open",
          .arguments = {"${0}"},
          .path_validation = HyperlinkPathValidation::existing_local_path,
          .path_template = "${0}",
          .whole_token = true,
      },
      {
          .id = "local-path-osc8",
          .recognition_source = HyperlinkRecognitionSource::osc8,
          .pattern = "^(?:" + local_path + ")$",
          .command = "xdg-open",
          .arguments = {"${0}"},
          .path_validation = HyperlinkPathValidation::existing_local_path,
          .path_template = "${0}",
      },
      {
          .id = "local-file-uri-osc8",
          .recognition_source = HyperlinkRecognitionSource::osc8,
          .pattern = R"(^file://(?:localhost)?(?<path>/[^\r\n?#]*)$)",
          .command = "xdg-open",
          .arguments = {"${path|uri-decode}"},
          .path_validation = HyperlinkPathValidation::existing_local_path,
          .path_template = "${path|uri-decode}",
      },
  };
}

const char *hyperlink_recognition_source_to_string(
    HyperlinkRecognitionSource source) {
  return source == HyperlinkRecognitionSource::terminal_text
             ? "terminal-text"
             : "osc8";
}

std::optional<HyperlinkRecognitionSource>
hyperlink_recognition_source_from_string(const std::string &value) {
  if (value == "osc8") {
    return HyperlinkRecognitionSource::osc8;
  }
  if (value == "terminal-text") {
    return HyperlinkRecognitionSource::terminal_text;
  }
  return std::nullopt;
}

const char *hyperlink_path_validation_to_string(
    HyperlinkPathValidation validation) {
  return validation == HyperlinkPathValidation::existing_local_path
             ? "existing-local-path"
             : "none";
}

std::optional<HyperlinkPathValidation>
hyperlink_path_validation_from_string(const std::string &value) {
  if (value == "none") {
    return HyperlinkPathValidation::none;
  }
  if (value == "existing-local-path") {
    return HyperlinkPathValidation::existing_local_path;
  }
  return std::nullopt;
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
                                    std::string *reason,
                                    RuleValidationLocation *location) {
  RuleValidationLocation ignored;
  if (!location) location = &ignored;
  location->field = RuleValidationField::identifier;
  if (!hyperlink_action_rule_id_is_valid(rule.id, reason)) {
    return false;
  }
  location->field = RuleValidationField::pattern;
  if (rule.pattern.empty()) {
    *reason = "regular expression must not be empty";
    return false;
  }
  location->field = RuleValidationField::command;
  if (trim_ascii_whitespace(rule.command).empty()) {
    *reason = "command must not be empty";
    return false;
  }
  if (rule.command.find('\0') != std::string::npos) {
    *reason = "command must not contain a NUL byte";
    return false;
  }

  location->field = RuleValidationField::pattern;
  RegularExpressionState *regex =
      create_regular_expression(rule.pattern, reason);
  if (regex == nullptr) {
    return false;
  }

  location->field = RuleValidationField::argument;
  bool valid = true;
  for (std::size_t index = 0; index < rule.arguments.size(); ++index) {
    location->argument_index = index;
    if (!regex_capture_template_is_valid(rule.arguments[index], regex,
        RegexCaptureTemplateOptions{.allow_uri_decode = true}, reason)) {
      valid = false;
      break;
    }
  }
  if (valid && rule.path_validation ==
                   HyperlinkPathValidation::existing_local_path) {
    location->field = RuleValidationField::path_template;
    if (rule.path_template.empty()) {
      *reason = "path template must not be empty when path validation is "
                "enabled";
      valid = false;
    } else {
      valid = regex_capture_template_is_valid(
          rule.path_template, regex,
          RegexCaptureTemplateOptions{.allow_uri_decode = true}, reason);
    }
  }
  destroy_regular_expression(regex);
  return valid;
}

} // namespace elder_terms

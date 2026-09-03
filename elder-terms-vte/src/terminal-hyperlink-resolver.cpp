#include "terminal-hyperlink-resolver.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <elder-terms/settings/regular-expression.h>
#include <elder-terms/settings/regex-capture-template.h>

namespace elder_terms {

static constexpr std::size_t maximum_hyperlink_target_bytes = 8192;

struct CompiledHyperlinkRule {
  HyperlinkActionRule rule;
  std::shared_ptr<RegularExpressionState> regex;
  std::optional<std::size_t> terminal_text_index;
};

struct TerminalHyperlinkResolverState {
  std::vector<CompiledHyperlinkRule> rules;
};

static std::vector<CompiledHyperlinkRule>
compile_hyperlink_rules(std::vector<HyperlinkActionRule> rules) {
  std::vector<CompiledHyperlinkRule> compiled;
  compiled.reserve(rules.size());
  std::size_t terminal_text_index = 0;
  for (HyperlinkActionRule &rule : rules) {
    const std::optional<std::size_t> rule_terminal_text_index =
        rule.recognition_source == HyperlinkRecognitionSource::terminal_text
            ? std::optional<std::size_t>(terminal_text_index++)
            : std::nullopt;
    std::string reason;
    if (!hyperlink_action_rule_is_valid(rule, &reason)) {
      continue;
    }

    RegularExpressionState *regex =
        create_regular_expression(
            rule.pattern,
            rule.recognition_source ==
                HyperlinkRecognitionSource::terminal_text,
            &reason);
    if (regex == nullptr) {
      continue;
    }
    compiled.push_back({
        .rule = std::move(rule),
        .regex = std::shared_ptr<RegularExpressionState>(
            regex, destroy_regular_expression),
        .terminal_text_index = rule_terminal_text_index,
    });
  }
  return compiled;
}

static bool match_covers_target(const RegularExpressionMatch &match,
                                std::size_t target_size) {
  return match.start == 0 && match.end == target_size;
}

static std::optional<std::string> expand_hyperlink_template(
    const std::string &value, const RegularExpressionMatch *match,
    std::string *reason) {
  return expand_regex_capture_template(
      value, match,
      RegexCaptureTemplateOptions{.allow_uri_decode = true}, reason);
}

static bool validate_hyperlink_path(const std::string &value,
                                    std::string *reason) {
  if (value.find('\0') != std::string::npos) {
    *reason = "expanded path contains a NUL byte";
    return false;
  }

  const std::filesystem::path path(value);
  if (!path.is_absolute()) {
    *reason = "expanded path must be absolute: " + value;
    return false;
  }

  std::error_code status_error;
  const std::filesystem::file_status status =
      std::filesystem::status(path, status_error);
  if (status_error) {
    if (status_error == std::errc::no_such_file_or_directory) {
      *reason = "expanded path does not exist: " + value;
    } else {
      *reason = "expanded path could not be inspected: " + value + ": " +
                status_error.message();
    }
    return false;
  }
  if (!std::filesystem::exists(status)) {
    *reason = "expanded path does not exist: " + value;
    return false;
  }
  if (!std::filesystem::is_regular_file(status) &&
      !std::filesystem::is_directory(status)) {
    *reason = "expanded path is not a regular file or directory: " + value;
    return false;
  }
  return true;
}

static std::optional<TerminalHyperlinkAction> expand_hyperlink_action(
    const HyperlinkActionRule &rule, const RegularExpressionMatch *match,
    std::string *reason) {
  TerminalHyperlinkAction action{
      .command = rule.command,
      .arguments = {},
  };
  action.arguments.reserve(rule.arguments.size());
  for (const std::string &argument : rule.arguments) {
    const std::optional<std::string> expanded =
        expand_hyperlink_template(argument, match, reason);
    if (!expanded.has_value()) {
      return std::nullopt;
    }
    action.arguments.push_back(*expanded);
  }

  if (rule.path_validation ==
      HyperlinkPathValidation::existing_local_path) {
    const std::optional<std::string> path =
        expand_hyperlink_template(rule.path_template, match, reason);
    if (!path.has_value() || !validate_hyperlink_path(*path, reason)) {
      return std::nullopt;
    }
  }
  return action;
}

static const std::optional<std::string> *candidate_for_rule(
    const CompiledHyperlinkRule &rule,
    const TerminalHyperlinkCandidates &candidates) {
  if (!rule.terminal_text_index.has_value()) {
    return &candidates.osc8_target;
  }
  if (*rule.terminal_text_index >= candidates.terminal_text.size()) {
    return nullptr;
  }
  return &candidates.terminal_text[*rule.terminal_text_index];
}

TerminalHyperlinkResolverState *create_terminal_hyperlink_resolver(
    std::vector<HyperlinkActionRule> rules) {
  auto *state = new TerminalHyperlinkResolverState();
  state->rules = compile_hyperlink_rules(std::move(rules));
  return state;
}

std::optional<TerminalHyperlinkResolution>
resolve_terminal_hyperlink(TerminalHyperlinkResolverState *state,
                           const TerminalHyperlinkCandidates &candidates) {
  if (state == nullptr) {
    return std::nullopt;
  }

  std::optional<std::string> first_rejection;
  for (const CompiledHyperlinkRule &rule : state->rules) {
    const std::optional<std::string> *candidate =
        candidate_for_rule(rule, candidates);
    if (candidate == nullptr || !candidate->has_value() ||
        (*candidate)->empty() ||
        (*candidate)->size() > maximum_hyperlink_target_bytes) {
      continue;
    }

    std::string match_reason;
    const std::optional<RegularExpressionMatch> match =
        search_regular_expression(rule.regex.get(), **candidate,
                                  &match_reason);
    if (!match.has_value() ||
        !match_covers_target(*match, (*candidate)->size())) {
      continue;
    }

    std::string rejection;
    const std::optional<TerminalHyperlinkAction> action =
        expand_hyperlink_action(rule.rule, &*match, &rejection);
    if (action.has_value()) {
      return TerminalHyperlinkResolution{
          .action = action,
          .error = {},
      };
    }
    if (!first_rejection.has_value()) {
      first_rejection = "rule " + rule.rule.id + ": " + rejection;
    }
  }
  if (first_rejection.has_value()) {
    return TerminalHyperlinkResolution{
        .action = std::nullopt,
        .error = *first_rejection,
    };
  }
  return std::nullopt;
}

void destroy_terminal_hyperlink_resolver(
    TerminalHyperlinkResolverState *state) {
  delete state;
}

} // namespace elder_terms

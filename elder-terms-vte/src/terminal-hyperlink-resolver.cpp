#include "terminal-hyperlink-resolver.h"

#include <cstddef>
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
};

struct TerminalHyperlinkResolverState {
  std::vector<CompiledHyperlinkRule> rules;
};

static std::vector<CompiledHyperlinkRule>
compile_hyperlink_rules(std::vector<HyperlinkActionRule> rules) {
  std::vector<CompiledHyperlinkRule> compiled;
  compiled.reserve(rules.size());
  for (HyperlinkActionRule &rule : rules) {
    std::string reason;
    if (!hyperlink_action_rule_is_valid(rule, &reason)) {
      continue;
    }

    RegularExpressionState *regex =
        create_regular_expression(rule.pattern, false, &reason);
    if (regex == nullptr) {
      continue;
    }
    compiled.push_back({
        .rule = std::move(rule),
        .regex = std::shared_ptr<RegularExpressionState>(
            regex, destroy_regular_expression),
    });
  }
  return compiled;
}

static bool match_covers_target(const RegularExpressionMatch &match,
                                std::size_t target_size) {
  return match.start == 0 && match.end == target_size;
}

static std::optional<TerminalHyperlinkAction>
expand_hyperlink_action(const HyperlinkActionRule &rule,
                        const RegularExpressionMatch *match) {
  TerminalHyperlinkAction action{
      .command = rule.command,
      .arguments = {},
  };
  action.arguments.reserve(rule.arguments.size());
  for (const std::string &argument : rule.arguments) {
    std::string reason;
    const std::optional<std::string> expanded =
        expand_regex_capture_template(
            argument, match,
            RegexCaptureTemplateOptions{.allow_uri_decode = true}, &reason);
    if (!expanded.has_value()) {
      return std::nullopt;
    }
    action.arguments.push_back(*expanded);
  }
  return action;
}

TerminalHyperlinkResolverState *create_terminal_hyperlink_resolver(
    std::vector<HyperlinkActionRule> rules) {
  auto *state = new TerminalHyperlinkResolverState();
  state->rules = compile_hyperlink_rules(std::move(rules));
  return state;
}

std::optional<TerminalHyperlinkAction>
resolve_terminal_hyperlink(TerminalHyperlinkResolverState *state,
                           const std::string &target) {
  if (state == nullptr || target.empty() ||
      target.size() > maximum_hyperlink_target_bytes) {
    return std::nullopt;
  }

  for (const CompiledHyperlinkRule &rule : state->rules) {
    std::string reason;
    const std::optional<RegularExpressionMatch> match =
        search_regular_expression(rule.regex.get(), target, &reason);
    if (match.has_value() && match_covers_target(*match, target.size())) {
      const std::optional<TerminalHyperlinkAction> action =
          expand_hyperlink_action(rule.rule, &*match);
      return action;
    }
  }
  return std::nullopt;
}

void destroy_terminal_hyperlink_resolver(
    TerminalHyperlinkResolverState *state) {
  delete state;
}

} // namespace elder_terms

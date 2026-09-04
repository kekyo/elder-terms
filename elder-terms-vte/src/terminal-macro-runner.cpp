#include "terminal-macro-runner.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <elder-terms/settings/regular-expression.h>
#include <elder-terms/settings/regex-capture-template.h>

namespace elder_terms {

static constexpr std::size_t maximum_macro_line_bytes = 1024 * 1024;

struct CompiledMacroRule {
  MacroRule rule;
  std::shared_ptr<RegularExpressionState> regex;
};

struct TerminalMacroRunnerState {
  std::vector<CompiledMacroRule> rules;
  TerminalMacroRunnerCallbacks callbacks;
  std::string line;
  bool pending_carriage_return = false;
  bool action_executed = false;
};

static std::vector<CompiledMacroRule>
compile_macro_rules(std::vector<MacroRule> rules) {
  std::vector<CompiledMacroRule> compiled;
  compiled.reserve(rules.size());
  for (MacroRule &rule : rules) {
    std::string reason;
    if (!macro_rule_is_valid(rule, &reason)) {
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

static std::optional<std::string>
expand_macro_template(const std::string &value,
                      const RegularExpressionMatch *match) {
  std::string reason;
  return expand_regex_capture_template(
      value, match,
      RegexCaptureTemplateOptions{.allow_uri_decode = false}, &reason);
}

static void execute_macro_action(TerminalMacroRunnerState *state,
                                 const MacroAction &action,
                                 const RegularExpressionMatch *match) {
  if (const auto *send = std::get_if<MacroSendAction>(&action)) {
    if (state->callbacks.send) {
      const std::optional<std::string> expanded =
          expand_macro_template(send->text, match);
      if (expanded.has_value()) {
        state->callbacks.send(*expanded);
      }
    }
    return;
  }

  if (!state->callbacks.command) {
    return;
  }
  const auto &command = std::get<MacroCommandAction>(action);
  std::vector<std::string> arguments;
  arguments.reserve(command.arguments.size());
  for (const std::string &argument : command.arguments) {
    const std::optional<std::string> expanded =
        expand_macro_template(argument, match);
    if (!expanded.has_value()) {
      return;
    }
    arguments.push_back(*expanded);
  }
  const std::optional<std::string> expanded_command =
      expand_macro_template(command.command, match);
  if (expanded_command.has_value()) {
    state->callbacks.command(*expanded_command, std::move(arguments));
  }
}

static void match_current_macro_line(TerminalMacroRunnerState *state) {
  if (state->action_executed) {
    return;
  }

  for (const CompiledMacroRule &rule : state->rules) {
    std::string reason;
    const std::optional<RegularExpressionMatch> match =
        search_regular_expression(rule.regex.get(), state->line, &reason);
    if (match.has_value()) {
      state->action_executed = true;
      execute_macro_action(state, rule.rule.action, &*match);
      return;
    }
  }
}

static bool is_utf8_continuation(unsigned char byte) {
  return (byte & 0xc0) == 0x80;
}

static void append_macro_line(TerminalMacroRunnerState *state,
                              const unsigned char *data, std::size_t size) {
  if (size == 0) {
    return;
  }
  state->line.append(reinterpret_cast<const char *>(data), size);
  if (state->line.size() > maximum_macro_line_bytes) {
    state->line.erase(0, state->line.size() - maximum_macro_line_bytes);
    while (!state->line.empty() && is_utf8_continuation(
                                       static_cast<unsigned char>(
                                           state->line.front()))) {
      state->line.erase(0, 1);
    }
  }
  match_current_macro_line(state);
}

static void flush_pending_carriage_return(
    TerminalMacroRunnerState *state) {
  if (!state->pending_carriage_return) {
    return;
  }
  const unsigned char carriage_return = '\r';
  state->pending_carriage_return = false;
  append_macro_line(state, &carriage_return, 1);
}

TerminalMacroRunnerState *create_terminal_macro_runner(
    std::vector<MacroRule> rules, TerminalMacroRunnerCallbacks callbacks) {
  auto *state = new TerminalMacroRunnerState();
  state->rules = compile_macro_rules(std::move(rules));
  state->callbacks = std::move(callbacks);
  return state;
}

void feed_terminal_macro_runner(TerminalMacroRunnerState *state,
                                std::span<const unsigned char> bytes) {
  if (state == nullptr || bytes.empty() || state->rules.empty()) {
    return;
  }

  std::size_t segment_start = 0;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const unsigned char byte = bytes[index];
    if (byte != '\r' && byte != '\n') {
      continue;
    }

    const std::size_t segment_size = index - segment_start;
    if (segment_size != 0) {
      flush_pending_carriage_return(state);
      append_macro_line(state, bytes.data() + segment_start, segment_size);
    }
    if (byte == '\r') {
      flush_pending_carriage_return(state);
      state->pending_carriage_return = true;
    } else {
      state->pending_carriage_return = false;
      state->line.clear();
      state->action_executed = false;
    }
    segment_start = index + 1;
  }

  if (segment_start < bytes.size()) {
    flush_pending_carriage_return(state);
    append_macro_line(state, bytes.data() + segment_start,
                      bytes.size() - segment_start);
  }
}

void replace_terminal_macro_runner_rules(TerminalMacroRunnerState *state,
                                         std::vector<MacroRule> rules) {
  if (state == nullptr) {
    return;
  }
  state->rules = compile_macro_rules(std::move(rules));
  state->line.clear();
  state->pending_carriage_return = false;
  state->action_executed = false;
}

void destroy_terminal_macro_runner(TerminalMacroRunnerState *state) {
  delete state;
}

} // namespace elder_terms

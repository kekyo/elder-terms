#include "terminal-macro-runner.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <glib.h>

namespace elder_terms {

static constexpr std::size_t maximum_macro_line_bytes = 1024 * 1024;

struct CompiledMacroRule {
  MacroRule rule;
  std::shared_ptr<GRegex> regex;
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

    GError *error = nullptr;
    GRegex *regex = g_regex_new(rule.pattern.c_str(), G_REGEX_DEFAULT,
                                G_REGEX_MATCH_DEFAULT, &error);
    g_clear_error(&error);
    if (regex == nullptr) {
      continue;
    }
    compiled.push_back({
        .rule = std::move(rule),
        .regex = std::shared_ptr<GRegex>(regex, g_regex_unref),
    });
  }
  return compiled;
}

static int decimal_capture_number(const std::string &capture) {
  int number = 0;
  for (unsigned char character : capture) {
    number = number * 10 + static_cast<int>(character - '0');
  }
  return number;
}

static std::string expand_macro_template(const std::string &value,
                                         GMatchInfo *match) {
  std::string expanded;
  for (std::size_t index = 0; index < value.size();) {
    if (value[index] != '$') {
      expanded.push_back(value[index]);
      ++index;
      continue;
    }
    if (index + 1 < value.size() && value[index + 1] == '$') {
      expanded.push_back('$');
      index += 2;
      continue;
    }

    const std::size_t closing = value.find('}', index + 2);
    if (index + 1 >= value.size() || value[index + 1] != '{' ||
        closing == std::string::npos) {
      expanded.push_back('$');
      ++index;
      continue;
    }

    const std::string capture =
        value.substr(index + 2, closing - index - 2);
    const bool numeric =
        !capture.empty() &&
        std::all_of(capture.begin(), capture.end(), [](unsigned char value) {
          return std::isdigit(value) != 0;
        });
    gchar *matched = numeric
                         ? g_match_info_fetch(
                               match, decimal_capture_number(capture))
                         : g_match_info_fetch_named(match, capture.c_str());
    if (matched != nullptr) {
      expanded.append(matched);
    }
    g_free(matched);
    index = closing + 1;
  }
  return expanded;
}

static void execute_macro_action(TerminalMacroRunnerState *state,
                                 const MacroAction &action,
                                 GMatchInfo *match) {
  if (const auto *send = std::get_if<MacroSendAction>(&action)) {
    if (state->callbacks.send) {
      state->callbacks.send(expand_macro_template(send->text, match));
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
    arguments.push_back(expand_macro_template(argument, match));
  }
  state->callbacks.command(expand_macro_template(command.command, match),
                           std::move(arguments));
}

static void match_current_macro_line(TerminalMacroRunnerState *state) {
  if (state->action_executed) {
    return;
  }

  for (const CompiledMacroRule &rule : state->rules) {
    GMatchInfo *match = nullptr;
    const gboolean matched = g_regex_match_full(
        rule.regex.get(), state->line.data(),
        static_cast<gssize>(state->line.size()), 0, G_REGEX_MATCH_DEFAULT,
        &match, nullptr);
    if (matched != FALSE) {
      state->action_executed = true;
      execute_macro_action(state, rule.rule.action, match);
      g_match_info_free(match);
      return;
    }
    if (match != nullptr) {
      g_match_info_free(match);
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

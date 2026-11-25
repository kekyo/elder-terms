// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-received-macros.h"
#include <elder-terms/settings/regular-expression.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <elder-terms/settings/regex-capture-template.h>

namespace elder_terms {

static constexpr std::size_t line_capacity = 1024 * 1024;
static constexpr std::size_t queue_capacity = 2 * line_capacity;
static constexpr std::size_t send_chunk_capacity = 4096;

struct ReceivedMacroRule {
  MacroRule rule;
  std::shared_ptr<RegularExpressionState> program;
};

struct ReceivedMacroNotification {
  std::string bytes;
  std::size_t offset = 0;
};

struct TerminalReceivedMacros {
  std::vector<ReceivedMacroRule> rules;
  TerminalReceivedMacroCallbacks callbacks;
  std::deque<ReceivedMacroNotification> notifications;
  std::size_t queued_bytes = 0;
  std::uint64_t generation = 0;
  std::string line;
  bool pending_carriage_return = false;
  bool action_executed = false;
  bool matching = false;
  std::size_t rule_index = 0;
  std::unique_ptr<boxrig_regex_matcher, decltype(&boxrig_regex_matcher_destroy)> matcher{
      nullptr, boxrig_regex_matcher_destroy};
  std::string send;
  std::size_t send_offset = 0;
};

static void diagnose(TerminalReceivedMacros *macros, std::string reason) {
  if (macros->callbacks.error) macros->callbacks.error(std::move(reason));
}

static bool is_continuation(unsigned char byte) { return (byte & 0xc0) == 0x80; }

static void append_line(TerminalReceivedMacros *macros, const char *bytes, std::size_t length) {
  if (length >= line_capacity) {
    macros->line.assign(bytes + length - line_capacity, line_capacity);
  } else {
    if (macros->line.size() + length > line_capacity)
      macros->line.erase(0, macros->line.size() + length - line_capacity);
    macros->line.append(bytes, length);
  }
  std::size_t prefix = 0;
  while (prefix < macros->line.size() && is_continuation(macros->line[prefix])) ++prefix;
  macros->line.erase(0, prefix);
  macros->matching = !macros->action_executed && !macros->rules.empty();
  macros->rule_index = 0;
}

static void execute_action(TerminalReceivedMacros *macros, const boxrig_regex_result *result) {
  macros->action_executed = true;
  const RegexCaptureLookup capture = [result](const std::string &name) {
    return regular_expression_capture(result, name);
  };
  const auto expand = [&](const std::string &value) {
    std::string reason;
    auto expanded = expand_regex_capture_template(value, capture, {}, &reason);
    if (!expanded) diagnose(macros, std::move(reason));
    return expanded;
  };
  const auto &action = macros->rules[macros->rule_index].rule.action;
  if (const auto *send = std::get_if<MacroSendAction>(&action)) {
    if (!macros->callbacks.send) return;
    if (auto text = expand(send->text)) {
      macros->send = macros->callbacks.prepare_send ?
          macros->callbacks.prepare_send(std::move(*text)) : std::move(*text);
      macros->send_offset = 0;
    }
    return;
  }
  if (!macros->callbacks.command) return;
  const auto &action_command = std::get<MacroCommandAction>(action);
  auto command = expand(action_command.command);
  if (!command) return;
  if (command->empty() || command->find('\0') != std::string::npos) {
    diagnose(macros, "Macro command is empty or contains NUL");
    return;
  }
  std::vector<std::string> args;
  for (const auto &argument : action_command.arguments) {
    auto arg = expand(argument);
    if (!arg) return;
    if (arg->find('\0') != std::string::npos) {
      diagnose(macros, "Macro argument contains NUL");
      return;
    }
    args.push_back(std::move(*arg));
  }
  macros->callbacks.command(std::move(*command), std::move(args));
}

static void advance_match(TerminalReceivedMacros *macros, std::size_t budget) {
  if (!macros->matcher) {
    boxrig_regex_matcher *matcher = nullptr;
    std::size_t error_offset = 0;
    const auto status = boxrig_regex_matcher_create(macros->rules[macros->rule_index].program.get(),
        reinterpret_cast<const uint8_t *>(macros->line.data()), macros->line.size(), 0, &error_offset, &matcher);
    if (status != BOXRIG_STATUS_OK) {
      diagnose(macros, "Macro input error at byte " + std::to_string(error_offset));
      macros->matching = false;
      return;
    }
    macros->matcher.reset(matcher);
  }
  std::size_t used = 0;
  boxrig_regex_progress progress{};
  boxrig_regex_match match{};
  const auto status = boxrig_regex_matcher_resume(macros->matcher.get(), budget, &used, &progress, &match);
  if (status == BOXRIG_STATUS_OK && progress == BOXRIG_REGEX_IN_PROGRESS) return;
  if (status != BOXRIG_STATUS_OK) {
    diagnose(macros, "Macro matching failed (status " + std::to_string(status) + ")");
  } else if (progress == BOXRIG_REGEX_MATCH) {
    boxrig_regex_result *result = nullptr;
    if (boxrig_regex_matcher_result(macros->matcher.get(), &result) == BOXRIG_STATUS_OK) {
      const std::shared_ptr<boxrig_regex_result> owned(result, boxrig_regex_result_destroy);
      execute_action(macros, owned.get());
    } else {
      diagnose(macros, "Macro captures could not be retained");
    }
    macros->matching = false;
  }
  macros->matcher.reset();
  if (++macros->rule_index == macros->rules.size()) macros->matching = false;
}

static void advance_notification(TerminalReceivedMacros *macros) {
  auto &notification = macros->notifications.front();
  const char byte = notification.bytes[notification.offset];
  // A CR not followed by LF is itself a matching snapshot, before later data.
  if (macros->pending_carriage_return && byte != '\n') {
    macros->pending_carriage_return = false;
    append_line(macros, "\r", 1);
    return;
  }
  const auto before = notification.offset;
  if (byte == '\n') {
    macros->line.clear();
    macros->pending_carriage_return = false;
    macros->action_executed = false;
    ++notification.offset;
  } else if (byte == '\r') {
    macros->pending_carriage_return = true;
    ++notification.offset;
  } else {
    const auto end = notification.bytes.find_first_of("\r\n", before);
    notification.offset = end == std::string::npos ? notification.bytes.size() : end;
    append_line(macros, notification.bytes.data() + before, notification.offset - before);
  }
  macros->queued_bytes -= notification.offset - before;
  if (notification.offset == notification.bytes.size()) macros->notifications.pop_front();
}

TerminalReceivedMacros *create_terminal_received_macros(
    std::vector<MacroRule> rules, TerminalReceivedMacroCallbacks callbacks) {
  auto macros = std::make_unique<TerminalReceivedMacros>();
  macros->callbacks = std::move(callbacks);
  for (auto &rule : rules) {
    std::string reason;
    std::shared_ptr<RegularExpressionState> program(create_regular_expression(rule.pattern, &reason), destroy_regular_expression);
    if (!program) diagnose(macros.get(), rule.id + ": " + reason);
    else macros->rules.push_back({std::move(rule), std::move(program)});
  }
  return macros.release();
}

bool terminal_received_macros_feed(TerminalReceivedMacros *macros,
    std::uint64_t generation, std::span<const unsigned char> bytes) {
  if (generation < macros->generation) return true;
  if (generation != macros->generation) terminal_received_macros_reset(macros, generation);
  if (bytes.empty() || macros->rules.empty()) return true;
  if (bytes.size() > queue_capacity - macros->queued_bytes) return false;
  macros->notifications.push_back({std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()), 0});
  macros->queued_bytes += bytes.size();
  return true;
}

TerminalReceivedMacroProgress terminal_received_macros_dispatch(
    TerminalReceivedMacros *macros, std::size_t work_budget) {
  bool blocked = false;
  if (macros->send_offset != macros->send.size()) {
    auto end = std::min(macros->send_offset + send_chunk_capacity, macros->send.size());
    while (end > macros->send_offset && end < macros->send.size() && is_continuation(macros->send[end])) --end;
    if (end == macros->send_offset) {
      diagnose(macros, "Macro text contains an invalid UTF-8 sequence");
      macros->send.clear();
      macros->send_offset = 0;
      return {macros->matching || !macros->notifications.empty(), false, macros->queued_bytes, macros->line.size()};
    }
    const std::span bytes(reinterpret_cast<const unsigned char *>(macros->send.data() + macros->send_offset),
        end - macros->send_offset);
    if (!macros->callbacks.send(bytes)) blocked = true;
    else if ((macros->send_offset = end) == macros->send.size()) {
      macros->send.clear();
      macros->send_offset = 0;
    }
  } else if (macros->matching) {
    advance_match(macros, work_budget);
  } else if (!macros->notifications.empty()) {
    advance_notification(macros);
  }
  return {macros->matching || !macros->notifications.empty() || !macros->send.empty(),
      blocked, macros->queued_bytes, macros->line.size()};
}

void terminal_received_macros_reset(TerminalReceivedMacros *macros, std::uint64_t generation) {
  macros->matcher.reset();
  macros->notifications.clear();
  macros->queued_bytes = 0;
  macros->generation = generation;
  macros->line.clear();
  macros->pending_carriage_return = false;
  macros->action_executed = false;
  macros->matching = false;
  macros->rule_index = 0;
  macros->send.clear();
  macros->send_offset = 0;
}

void destroy_terminal_received_macros(TerminalReceivedMacros *macros) { delete macros; }

std::optional<ReceivedMacroLine> terminal_received_macros_snapshot(const TerminalReceivedMacros *macros) {
  if (macros->matching || !macros->notifications.empty() || !macros->send.empty()) return std::nullopt;
  return ReceivedMacroLine{macros->line, macros->pending_carriage_return, macros->action_executed};
}

void terminal_received_macros_restore(TerminalReceivedMacros *macros, std::uint64_t generation,
    const ReceivedMacroLine &line) {
  terminal_received_macros_reset(macros, generation);
  macros->line = line.bytes;
  macros->pending_carriage_return = line.pending_carriage_return;
  macros->action_executed = line.action_executed;
}

}
